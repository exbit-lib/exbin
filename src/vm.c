/* EXBIN: the virtual machine — stack-based bytecode interpreter with
 * closures, upvalues, safepoint GC and re-entrant native calls. */
#include <errno.h>
#include <time.h>

#include "vm.h"

/* ---------------------------------------------------------------- basics -- */
void vm_reset_stack(VM* vm) {
  vm->stackTop = vm->stack;
  vm->frameCount = 0;
  vm->openUpvalues = NULL;
}
void vm_push(VM* vm, Value v) { *vm->stackTop++ = v; }
Value vm_pop(VM* vm) { return *--vm->stackTop; }

void vm_write(VM* vm, const char* s, size_t n) {
  if (vm->out) vm->out(vm->outUd, s, n);
  else fwrite(s, 1, n, stdout);
}

void vm_set_errmsg(VM* vm, const char* msg) {
  free(vm->errmsg);
  size_t l = strlen(msg);
  vm->errmsg = (char*)malloc(l + 1);
  memcpy(vm->errmsg, msg, l + 1);
}

char* vm_read_file(const char* path, size_t* len) {
  FILE* f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) {
    fclose(f);
    return NULL;
  }
  long sz = ftell(f);
  if (sz < 0) {
    fclose(f);
    return NULL;
  }
  rewind(f);
  char* buf = (char*)malloc((size_t)sz + 1);
  if (!buf) {
    fclose(f);
    return NULL;
  }
  size_t n = fread(buf, 1, (size_t)sz, f);
  fclose(f);
  buf[n] = 0;
  if (len) *len = n;
  return buf;
}

bool vm_verror(VM* vm, const char* fmt, va_list ap) {
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  char* tmp = (char*)malloc((size_t)(n < 0 ? 0 : n) + 1);
  if (n >= 0) vsnprintf(tmp, (size_t)n + 1, fmt, ap2);
  else tmp[0] = 0;
  va_end(ap2);
  vm->errval = OBJ_VAL(str_copy(vm, tmp, strlen(tmp)));
  free(tmp);
  vm->trace.len = 0;
  if (vm->trace.d) vm->trace.d[0] = 0;
  return false;
}

bool vm_error(VM* vm, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vm_verror(vm, fmt, ap);
  va_end(ap);
  return false;
}

bool vm_throw(VM* vm, Value v) {
  vm->errval = v;
  vm->trace.len = 0;
  if (vm->trace.d) vm->trace.d[0] = 0;
  return false;
}

void vm_define_global(VM* vm, const char* name, Value v) { map_set_str(vm, vm->globals, name, v); }

ObjMap* methods_for(VM* vm, Value v, bool create) {
  int slot;
  switch (v.type) {
    case EX_NIL: slot = METH_NIL; break;
    case EX_BOOL: slot = METH_BOOL; break;
    case EX_NUM: slot = METH_NUM; break;
    default:
      slot = (int)AS_OBJ(v)->type;
      if (slot == OBJ_FUNCTION || slot == OBJ_CLOSURE) slot = OBJ_CLOSURE;
      if (slot == OBJ_HANDLE) {
        ObjMap* all = vm->methods[OBJ_HANDLE];
        if (!all) {
          if (!create) return NULL;
          all = vm->methods[OBJ_HANDLE] = map_new(vm);
        }
        Value key = OBJ_VAL(str_from(vm, AS_HANDLE(v)->kind));
        Value sub;
        if (map_get(all, key, &sub)) return AS_MAP(sub);
        if (!create) return NULL;
        ObjMap* m = map_new(vm);
        map_set(vm, all, key, OBJ_VAL(m));
        return m;
      }
  }
  if (!vm->methods[slot]) {
    if (!create) return NULL;
    vm->methods[slot] = map_new(vm);
  }
  return vm->methods[slot];
}

/* -------------------------------------------------------------- upvalues -- */
static ObjUpvalue* capture_upvalue(VM* vm, Value* local) {
  ObjUpvalue *prev = NULL, *up = vm->openUpvalues;
  while (up != NULL && up->location > local) {
    prev = up;
    up = up->next;
  }
  if (up != NULL && up->location == local) return up;
  ObjUpvalue* created = upvalue_new(vm, local);
  created->next = up;
  if (prev == NULL) vm->openUpvalues = created;
  else prev->next = created;
  return created;
}

static void close_upvalues(VM* vm, Value* last) {
  while (vm->openUpvalues != NULL && vm->openUpvalues->location >= last) {
    ObjUpvalue* u = vm->openUpvalues;
    u->closed = *u->location;
    u->location = &u->closed;
    vm->openUpvalues = u->next;
  }
}

/* ------------------------------------------------------------- semantics -- */
bool deep_equal(Value a, Value b, int depth) {
  if (values_equal(a, b)) return true;
  if (!IS_OBJ(a) || !IS_OBJ(b) || depth > 64) return false;
  if (AS_OBJ(a)->type != AS_OBJ(b)->type) return false;
  if (IS_LIST(a)) {
    ObjList *x = AS_LIST(a), *y = AS_LIST(b);
    if (x->count != y->count) return false;
    for (int i = 0; i < x->count; i++)
      if (!deep_equal(x->items[i], y->items[i], depth + 1)) return false;
    return true;
  }
  if (IS_MAP(a)) {
    ObjMap *x = AS_MAP(a), *y = AS_MAP(b);
    if (x->count != y->count) return false;
    for (int i = 0; i < x->used; i++) {
      if (IS_NIL(x->entries[i].key)) continue;
      Value other;
      if (!map_get(y, x->entries[i].key, &other)) return false;
      if (!deep_equal(x->entries[i].value, other, depth + 1)) return false;
    }
    return true;
  }
  if (IS_RANGE(a)) {
    ObjRange *x = AS_RANGE(a), *y = AS_RANGE(b);
    return x->start == y->start && x->stop == y->stop && x->step == y->step;
  }
  return false;
}

static int64_t to_i64(double d) {
  if (d != d) return 0;
  if (d >= 9.2233720368547758e18) return INT64_MAX;
  if (d <= -9.2233720368547758e18) return INT64_MIN;
  return (int64_t)d;
}

static int str_cmp(const ObjString* a, const ObjString* b) {
  int n = a->len < b->len ? a->len : b->len;
  int c = memcmp(a->chars, b->chars, (size_t)n);
  if (c != 0) return c;
  return a->len - b->len;
}

static bool add_slow(VM* vm, Value a, Value b, Value* out) {
  if (IS_STRING(a) || IS_STRING(b)) {
    ObjString* x = value_tostring(vm, a);
    ObjString* y = value_tostring(vm, b);
    *out = OBJ_VAL(str_concat(vm, x, y));
    return true;
  }
  if (IS_LIST(a) && IS_LIST(b)) {
    ObjList* r = list_new(vm);
    *out = OBJ_VAL(r);
    ObjList *x = AS_LIST(a), *y = AS_LIST(b);
    for (int i = 0; i < x->count; i++) list_push(vm, r, x->items[i]);
    for (int i = 0; i < y->count; i++) list_push(vm, r, y->items[i]);
    return true;
  }
  if (IS_BYTES(a) && IS_BYTES(b)) {
    ObjBytes *x = AS_BYTES(a), *y = AS_BYTES(b);
    ObjBytes* r = bytes_new(vm, x->len + y->len);
    if (x->len) memcpy(r->data, x->data, (size_t)x->len);
    if (y->len) memcpy(r->data + x->len, y->data, (size_t)y->len);
    *out = OBJ_VAL(r);
    return true;
  }
  return vm_error(vm, "Cannot add %s and %s.", value_type_name(a), value_type_name(b));
}

static bool mul_slow(VM* vm, Value a, Value b, Value* out) {
  Value seq = a, cnt = b;
  if (IS_NUM(a) && (IS_STRING(b) || IS_LIST(b))) {
    seq = b;
    cnt = a;
  }
  if (IS_NUM(cnt) && (IS_STRING(seq) || IS_LIST(seq))) {
    double n = AS_NUM(cnt);
    if (!(n >= 0) || n != floor(n)) return vm_error(vm, "Repeat count must be a non-negative integer.");
    if (IS_STRING(seq)) {
      ObjString* s = AS_STRING(seq);
      double total = n * s->len;
      if (total > 1e9) return vm_error(vm, "Repeated string too large.");
      Buf buf;
      buf_init(&buf);
      for (int64_t i = 0; i < (int64_t)n; i++) buf_add(&buf, s->chars, (size_t)s->len);
      *out = OBJ_VAL(str_copy(vm, buf.d ? buf.d : "", buf.len));
      buf_free(&buf);
      return true;
    }
    ObjList* l = AS_LIST(seq);
    if (n * l->count > 5e7) return vm_error(vm, "Repeated list too large.");
    ObjList* r = list_new(vm);
    *out = OBJ_VAL(r);
    for (int64_t i = 0; i < (int64_t)n; i++)
      for (int j = 0; j < l->count; j++) list_push(vm, r, l->items[j]);
    return true;
  }
  return vm_error(vm, "Cannot multiply %s and %s.", value_type_name(a), value_type_name(b));
}

static bool check_index(VM* vm, Value idx, int count, const char* what, int* out) {
  if (!IS_NUM(idx) || AS_NUM(idx) != floor(AS_NUM(idx)))
    return vm_error(vm, "%s index must be an integer, got %s.", what, value_type_name(idx));
  double d = AS_NUM(idx);
  if (d < 0) d += count;
  if (d < 0 || d >= count)
    return vm_error(vm, "%s index %.0f out of range (length %d).", what, AS_NUM(idx), count);
  *out = (int)d;
  return true;
}

static bool slice_bounds(VM* vm, ObjRange* r, int count, int* a, int* b) {
  if (r->step != 1) return vm_error(vm, "Slices need a step of 1.");
  double s = r->start, e = r->stop;
  if (s != floor(s) || e != floor(e)) return vm_error(vm, "Slice bounds must be integers.");
  if (s < 0) s += count;
  if (e < 0) e += count;
  if (s < 0) s = 0;
  if (e > count) e = count;
  if (s > e) s = e;
  *a = (int)s;
  *b = (int)e;
  return true;
}

static bool index_get(VM* vm, Value obj, Value idx, Value* out) {
  if (!IS_OBJ(obj)) return vm_error(vm, "Cannot index %s.", value_type_name(obj));
  int i, a = 0, b = 0;
  if (IS_RANGE(idx) && (IS_LIST(obj) || IS_STRING(obj) || IS_BYTES(obj))) {
    int count = IS_LIST(obj) ? AS_LIST(obj)->count : IS_STRING(obj) ? AS_STRING(obj)->len : AS_BYTES(obj)->len;
    if (!slice_bounds(vm, AS_RANGE(idx), count, &a, &b)) return false;
    if (IS_LIST(obj)) {
      ObjList* r = list_new(vm);
      *out = OBJ_VAL(r);
      for (int k = a; k < b; k++) list_push(vm, r, AS_LIST(obj)->items[k]);
    } else if (IS_STRING(obj)) {
      *out = OBJ_VAL(str_copy(vm, AS_CSTRING(obj) + a, (size_t)(b - a)));
    } else {
      ObjBytes* r = bytes_new(vm, b - a);
      if (b > a) memcpy(r->data, AS_BYTES(obj)->data + a, (size_t)(b - a));
      *out = OBJ_VAL(r);
    }
    return true;
  }
  switch (AS_OBJ(obj)->type) {
    case OBJ_LIST:
      if (!check_index(vm, idx, AS_LIST(obj)->count, "List", &i)) return false;
      *out = AS_LIST(obj)->items[i];
      return true;
    case OBJ_STRING:
      if (!check_index(vm, idx, AS_STRING(obj)->len, "String", &i)) return false;
      *out = OBJ_VAL(str_copy(vm, AS_CSTRING(obj) + i, 1));
      return true;
    case OBJ_BYTES:
      if (!check_index(vm, idx, AS_BYTES(obj)->len, "Bytes", &i)) return false;
      *out = NUM_VAL(AS_BYTES(obj)->data[i]);
      return true;
    case OBJ_RANGE: {
      int64_t n = range_len(AS_RANGE(obj));
      if (!check_index(vm, idx, (int)n, "Range", &i)) return false;
      *out = NUM_VAL(AS_RANGE(obj)->start + AS_RANGE(obj)->step * i);
      return true;
    }
    case OBJ_MAP:
      if (!map_get(AS_MAP(obj), idx, out)) *out = NIL_VAL;
      return true;
    default: return vm_error(vm, "Cannot index %s.", value_type_name(obj));
  }
}

static bool valid_key(VM* vm, Value k) {
  if (IS_NIL(k)) return vm_error(vm, "Map keys cannot be nil.");
  if (IS_NUM(k) && AS_NUM(k) != AS_NUM(k)) return vm_error(vm, "Map keys cannot be NaN.");
  return true;
}

static bool index_set(VM* vm, Value obj, Value idx, Value val) {
  if (!IS_OBJ(obj)) return vm_error(vm, "Cannot assign to an index of %s.", value_type_name(obj));
  int i;
  switch (AS_OBJ(obj)->type) {
    case OBJ_LIST:
      if (!check_index(vm, idx, AS_LIST(obj)->count, "List", &i)) return false;
      AS_LIST(obj)->items[i] = val;
      return true;
    case OBJ_BYTES:
      if (!check_index(vm, idx, AS_BYTES(obj)->len, "Bytes", &i)) return false;
      if (!IS_NUM(val) || AS_NUM(val) < 0 || AS_NUM(val) > 255 || AS_NUM(val) != floor(AS_NUM(val)))
        return vm_error(vm, "Bytes elements must be integers 0..255.");
      AS_BYTES(obj)->data[i] = (uint8_t)AS_NUM(val);
      return true;
    case OBJ_MAP:
      if (!valid_key(vm, idx)) return false;
      map_set(vm, AS_MAP(obj), idx, val);
      return true;
    default: return vm_error(vm, "Cannot assign to an index of %s.", value_type_name(obj));
  }
}

static bool field_get(VM* vm, Value obj, ObjString* name, Value* out) {
  if (IS_MAP(obj)) {
    if (!map_get(AS_MAP(obj), OBJ_VAL(name), out)) {
      /* `m.len` is the entry count unless the map has a real "len" key */
      if (name->len == 3 && memcmp(name->chars, "len", 3) == 0) *out = NUM_VAL(AS_MAP(obj)->count);
      else *out = NIL_VAL;
    }
    return true;
  }
  if (name->len == 3 && memcmp(name->chars, "len", 3) == 0 && IS_OBJ(obj)) {
    switch (AS_OBJ(obj)->type) {
      case OBJ_STRING: *out = NUM_VAL(AS_STRING(obj)->len); return true;
      case OBJ_LIST: *out = NUM_VAL(AS_LIST(obj)->count); return true;
      case OBJ_BYTES: *out = NUM_VAL(AS_BYTES(obj)->len); return true;
      case OBJ_RANGE: *out = NUM_VAL((double)range_len(AS_RANGE(obj))); return true;
      default: break;
    }
  }
  return vm_error(vm, "Cannot read property '%s' of %s.", name->chars, value_type_name(obj));
}

/* --------------------------------------------------------------- calling -- */
static bool call_closure(VM* vm, ObjClosure* c, int argc) {
  ObjFunction* f = c->fn;
  if (argc != f->arity)
    return vm_error(vm, "%s expects %d argument%s but got %d.", f->name ? f->name->chars : "function", f->arity,
                    f->arity == 1 ? "" : "s", argc);
  if (vm->frameCount == FRAMES_MAX) return vm_error(vm, "Stack overflow (call depth exceeded).");
  Value* slots = vm->stackTop - argc - 1;
  if (slots + f->maxStack + 64 >= vm->stack + STACK_MAX) return vm_error(vm, "Stack overflow.");
  CallFrame* fr = &vm->frames[vm->frameCount++];
  fr->closure = c;
  fr->ip = f->chunk.code;
  fr->slots = slots;
  return true;
}

static bool check_native_args(VM* vm, ObjNative* n, int argc, int selfAdjust) {
  if (argc < n->minArgs || (n->maxArgs >= 0 && argc > n->maxArgs)) {
    int lo = n->minArgs - selfAdjust, hi = n->maxArgs < 0 ? -1 : n->maxArgs - selfAdjust, got = argc - selfAdjust;
    if (hi < 0) return vm_error(vm, "%s expects at least %d argument%s but got %d.", n->name, lo, lo == 1 ? "" : "s", got);
    if (lo == hi) return vm_error(vm, "%s expects %d argument%s but got %d.", n->name, lo, lo == 1 ? "" : "s", got);
    return vm_error(vm, "%s expects %d to %d arguments but got %d.", n->name, lo, hi, got);
  }
  return true;
}

static bool call_value(VM* vm, Value callee, int argc) {
  if (IS_OBJ(callee)) {
    switch (AS_OBJ(callee)->type) {
      case OBJ_CLOSURE: return call_closure(vm, AS_CLOSURE(callee), argc);
      case OBJ_FUNCTION: {
        ObjFunction* f = AS_FUNCTION(callee);
        if (f->upvalueCount > 0) return vm_error(vm, "Cannot call a function that needs upvalues directly.");
        ObjClosure* c = closure_new(vm, f);
        vm->stackTop[-argc - 1] = OBJ_VAL(c);
        return call_closure(vm, c, argc);
      }
      case OBJ_NATIVE: {
        ObjNative* n = AS_NATIVE(callee);
        if (!check_native_args(vm, n, argc, 0)) return false;
        if (vm->stackTop + 64 >= vm->stack + STACK_MAX) return vm_error(vm, "Stack overflow.");
        Value ret = NIL_VAL;
        if (!n->fn(vm, argc, vm->stackTop - argc, &ret, n->ud)) return false;
        vm->stackTop -= argc + 1;
        vm_push(vm, ret);
        return true;
      }
      default: break;
    }
  }
  return vm_error(vm, "Can only call functions, not %s.", value_type_name(callee));
}

static bool run(VM* vm, int baseFrames);

bool vm_call(VM* vm, Value fn, int argc, const Value* args, Value* out) {
  if (!IS_CALLABLE(fn)) return vm_error(vm, "Can only call functions, not %s.", value_type_name(fn));
  if (vm->cdepth >= C_DEPTH_MAX) return vm_error(vm, "Native call depth exceeded.");
  if (vm->stackTop + argc + 16 >= vm->stack + STACK_MAX) return vm_error(vm, "Stack overflow.");
  int baseFrames = vm->frameCount;
  Value* baseTop = vm->stackTop;
  vm_push(vm, fn);
  for (int i = 0; i < argc; i++) vm_push(vm, args[i]);
  int before = vm->frameCount;
  if (!call_value(vm, fn, argc)) {
    close_upvalues(vm, baseTop);
    vm->frameCount = baseFrames;
    vm->stackTop = baseTop;
    return false;
  }
  if (vm->frameCount == before) { /* native: result already on the stack */
    *out = vm_pop(vm);
    return true;
  }
  vm->cdepth++;
  bool ok = run(vm, baseFrames);
  vm->cdepth--;
  if (!ok) {
    close_upvalues(vm, baseTop);
    vm->frameCount = baseFrames;
    vm->stackTop = baseTop;
    return false;
  }
  *out = vm_pop(vm);
  return true;
}

int vm_exec_function(VM* vm, ObjFunction* fn, Value* result) {
  vm->errval = NIL_VAL;
  vm->trace.len = 0;
  Value out = NIL_VAL;
  vm->cdepth = 0;
  if (!vm_call(vm, OBJ_VAL(fn), 0, NULL, &out)) {
    Buf b;
    buf_init(&b);
    buf_adds(&b, "Runtime error: ");
    value_tostr_buf(vm, &b, vm->errval);
    buf_addc(&b, '\n');
    if (vm->trace.d) buf_add(&b, vm->trace.d, vm->trace.len);
    vm_set_errmsg(vm, buf_str(&b));
    buf_free(&b);
    vm_reset_stack(vm);
    return EX_RUNTIME_ERROR;
  }
  if (result) *result = out;
  return EX_OK;
}

/* ------------------------------------------------------------ iteration -- */
static bool iter_init(VM* vm, Value* slot) {
  Value v = *slot;
  if (IS_OBJ(v)) {
    switch (AS_OBJ(v)->type) {
      case OBJ_LIST:
      case OBJ_STRING:
      case OBJ_BYTES:
      case OBJ_RANGE: return true;
      case OBJ_MAP: {
        ObjMap* m = AS_MAP(v);
        ObjList* keys = list_new(vm);
        *slot = OBJ_VAL(keys);
        for (int i = 0; i < m->used; i++)
          if (!IS_NIL(m->entries[i].key)) list_push(vm, keys, m->entries[i].key);
        return true;
      }
      default: break;
    }
  }
  return vm_error(vm, "'%s' is not iterable.", value_type_name(v));
}

static int utf8_len_at(const char* s, int remaining) {
  unsigned char c = (unsigned char)s[0];
  int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
  return n > remaining ? 1 : n;
}

/* ------------------------------------------------------------------ run -- */
static bool run(VM* vm, int baseFrames) {
  CallFrame* frame = &vm->frames[vm->frameCount - 1];
  uint8_t* ip = frame->ip;

#define READ_BYTE() (*ip++)
#define READ_U16() (ip += 2, (uint16_t)((ip[-2] << 8) | ip[-1]))
#define READ_CONST() (frame->closure->fn->chunk.consts.v[READ_U16()])
#define READ_STR() AS_STRING(READ_CONST())
#define PUSH(v) (*vm->stackTop++ = (v))
#define POP() (*--vm->stackTop)
#define PEEK(n) (vm->stackTop[-1 - (n)])
#define SAVE() (frame->ip = ip)
#define LOAD() (frame = &vm->frames[vm->frameCount - 1], ip = frame->ip)
#define ERR(...)                \
  do {                          \
    SAVE();                     \
    vm_error(vm, __VA_ARGS__);  \
    goto fail;                  \
  } while (0)
#define CHECK(expr)  \
  do {               \
    if (!(expr)) {   \
      SAVE();        \
      goto fail;     \
    }                \
  } while (0)
#define SAFEPOINT()                                              \
  do {                                                           \
    if (vm->gcStress || vm->bytesAllocated > vm->nextGC) {       \
      SAVE();                                                    \
      gc_collect(vm);                                            \
    }                                                            \
  } while (0)
#define NUM_BINOP(op, expr)                                                          \
  do {                                                                               \
    Value b_ = PEEK(0), a_ = PEEK(1);                                                \
    if (!IS_NUM(a_) || !IS_NUM(b_))                                                  \
      ERR("Operands of '%s' must be numbers (got %s and %s).", op, value_type_name(a_), value_type_name(b_)); \
    double x = AS_NUM(a_), y = AS_NUM(b_);                                           \
    vm->stackTop -= 2;                                                               \
    PUSH(expr);                                                                      \
  } while (0)
#define INT_BINOP(op, expr)                                                          \
  do {                                                                               \
    Value b_ = PEEK(0), a_ = PEEK(1);                                                \
    if (!IS_NUM(a_) || !IS_NUM(b_))                                                  \
      ERR("Operands of '%s' must be numbers (got %s and %s).", op, value_type_name(a_), value_type_name(b_)); \
    int64_t x = to_i64(AS_NUM(a_)), y = to_i64(AS_NUM(b_));                          \
    vm->stackTop -= 2;                                                               \
    PUSH(NUM_VAL((double)(expr)));                                                   \
  } while (0)
#define CMP_OP(op, opstr)                                                            \
  do {                                                                               \
    Value b_ = PEEK(0), a_ = PEEK(1);                                                \
    bool r_;                                                                         \
    if (IS_NUM(a_) && IS_NUM(b_)) r_ = AS_NUM(a_) op AS_NUM(b_);                     \
    else if (IS_STRING(a_) && IS_STRING(b_)) r_ = str_cmp(AS_STRING(a_), AS_STRING(b_)) op 0; \
    else ERR("Cannot compare %s and %s with '%s'.", value_type_name(a_), value_type_name(b_), opstr); \
    vm->stackTop -= 2;                                                               \
    PUSH(BOOL_VAL(r_));                                                              \
  } while (0)

  for (;;) {
    uint8_t instruction = READ_BYTE();
    switch (instruction) {
      case OP_CONSTANT: PUSH(READ_CONST()); break;
      case OP_NIL: PUSH(NIL_VAL); break;
      case OP_TRUE: PUSH(BOOL_VAL(true)); break;
      case OP_FALSE: PUSH(BOOL_VAL(false)); break;
      case OP_POP: vm->stackTop--; break;
      case OP_DUP: {
        Value v = PEEK(0);
        PUSH(v);
        break;
      }
      case OP_DUP2: {
        Value a = PEEK(1), b = PEEK(0);
        PUSH(a);
        PUSH(b);
        break;
      }
      case OP_GET_LOCAL: PUSH(frame->slots[READ_BYTE()]); break;
      case OP_SET_LOCAL: frame->slots[READ_BYTE()] = PEEK(0); break;
      case OP_GET_GLOBAL: {
        ObjString* name = READ_STR();
        Value v;
        if (!map_get(vm->globals, OBJ_VAL(name), &v)) ERR("Undefined variable '%s'.", name->chars);
        PUSH(v);
        break;
      }
      case OP_DEFINE_GLOBAL: {
        ObjString* name = READ_STR();
        map_set(vm, vm->globals, OBJ_VAL(name), PEEK(0));
        vm->stackTop--;
        break;
      }
      case OP_SET_GLOBAL: {
        ObjString* name = READ_STR();
        if (!map_get(vm->globals, OBJ_VAL(name), NULL)) ERR("Undefined variable '%s'.", name->chars);
        map_set(vm, vm->globals, OBJ_VAL(name), PEEK(0));
        break;
      }
      case OP_GET_UPVALUE: PUSH(*frame->closure->upvalues[READ_BYTE()]->location); break;
      case OP_SET_UPVALUE: *frame->closure->upvalues[READ_BYTE()]->location = PEEK(0); break;
      case OP_CLOSE_UPVALUE:
        close_upvalues(vm, vm->stackTop - 1);
        vm->stackTop--;
        break;
      case OP_GET_INDEX: {
        Value r;
        CHECK(index_get(vm, PEEK(1), PEEK(0), &r));
        vm->stackTop -= 2;
        PUSH(r);
        break;
      }
      case OP_SET_INDEX: {
        Value val = PEEK(0);
        CHECK(index_set(vm, PEEK(2), PEEK(1), val));
        vm->stackTop -= 3;
        PUSH(val);
        break;
      }
      case OP_GET_FIELD: {
        ObjString* name = READ_STR();
        Value r;
        CHECK(field_get(vm, PEEK(0), name, &r));
        vm->stackTop[-1] = r;
        break;
      }
      case OP_SET_FIELD: {
        ObjString* name = READ_STR();
        Value obj = PEEK(1), val = PEEK(0);
        if (!IS_MAP(obj)) ERR("Cannot set property '%s' on %s.", name->chars, value_type_name(obj));
        map_set(vm, AS_MAP(obj), OBJ_VAL(name), val);
        vm->stackTop -= 2;
        PUSH(val);
        break;
      }
      case OP_EQUAL: {
        Value b = PEEK(0), a = PEEK(1);
        vm->stackTop -= 2;
        PUSH(BOOL_VAL(deep_equal(a, b, 0)));
        break;
      }
      case OP_NOT_EQUAL: {
        Value b = PEEK(0), a = PEEK(1);
        vm->stackTop -= 2;
        PUSH(BOOL_VAL(!deep_equal(a, b, 0)));
        break;
      }
      case OP_LESS: CMP_OP(<, "<"); break;
      case OP_LESS_EQUAL: CMP_OP(<=, "<="); break;
      case OP_GREATER: CMP_OP(>, ">"); break;
      case OP_GREATER_EQUAL: CMP_OP(>=, ">="); break;
      case OP_ADD: {
        Value b = PEEK(0), a = PEEK(1);
        if (IS_NUM(a) && IS_NUM(b)) {
          vm->stackTop -= 2;
          PUSH(NUM_VAL(AS_NUM(a) + AS_NUM(b)));
        } else {
          Value r;
          CHECK(add_slow(vm, a, b, &r));
          vm->stackTop -= 2;
          PUSH(r);
        }
        break;
      }
      case OP_SUB: NUM_BINOP("-", NUM_VAL(x - y)); break;
      case OP_MUL: {
        Value b = PEEK(0), a = PEEK(1);
        if (IS_NUM(a) && IS_NUM(b)) {
          vm->stackTop -= 2;
          PUSH(NUM_VAL(AS_NUM(a) * AS_NUM(b)));
        } else {
          Value r;
          CHECK(mul_slow(vm, a, b, &r));
          vm->stackTop -= 2;
          PUSH(r);
        }
        break;
      }
      case OP_DIV: NUM_BINOP("/", NUM_VAL(x / y)); break;
      case OP_MOD: NUM_BINOP("%", NUM_VAL(fmod(x, y))); break;
      case OP_POW: NUM_BINOP("**", NUM_VAL(pow(x, y))); break;
      case OP_NEG: {
        if (!IS_NUM(PEEK(0))) ERR("Operand of unary '-' must be a number (got %s).", value_type_name(PEEK(0)));
        vm->stackTop[-1] = NUM_VAL(-AS_NUM(PEEK(0)));
        break;
      }
      case OP_NOT: vm->stackTop[-1] = BOOL_VAL(!value_truthy(PEEK(0))); break;
      case OP_BNOT: {
        if (!IS_NUM(PEEK(0))) ERR("Operand of '~' must be a number (got %s).", value_type_name(PEEK(0)));
        vm->stackTop[-1] = NUM_VAL((double)(~to_i64(AS_NUM(PEEK(0)))));
        break;
      }
      case OP_BAND: INT_BINOP("&", x & y); break;
      case OP_BOR: INT_BINOP("|", x | y); break;
      case OP_BXOR: INT_BINOP("^", x ^ y); break;
      case OP_SHL: INT_BINOP("<<", (int64_t)((uint64_t)x << (y & 63))); break;
      case OP_SHR: INT_BINOP(">>", x >> (y & 63)); break;
      case OP_JUMP: {
        uint16_t off = READ_U16();
        ip += off;
        break;
      }
      case OP_JUMP_IF_FALSE: {
        uint16_t off = READ_U16();
        if (!value_truthy(PEEK(0))) ip += off;
        break;
      }
      case OP_JUMP_IF_TRUE: {
        uint16_t off = READ_U16();
        if (value_truthy(PEEK(0))) ip += off;
        break;
      }
      case OP_LOOP: {
        uint16_t off = READ_U16();
        ip -= off;
        SAFEPOINT();
        break;
      }
      case OP_CALL: {
        int argc = READ_BYTE();
        SAVE();
        SAFEPOINT();
        CHECK(call_value(vm, PEEK(argc), argc));
        LOAD();
        break;
      }
      case OP_INVOKE: {
        ObjString* name = READ_STR();
        int argc = READ_BYTE();
        SAVE();
        SAFEPOINT();
        Value recv = PEEK(argc);
        if (IS_MAP(recv)) {
          Value f;
          if (map_get(AS_MAP(recv), OBJ_VAL(name), &f) && IS_CALLABLE(f)) {
            vm->stackTop[-argc - 1] = f;
            CHECK(call_value(vm, f, argc));
            LOAD();
            break;
          }
        }
        ObjMap* mt = methods_for(vm, recv, false);
        Value m;
        if (!mt || !map_get(mt, OBJ_VAL(name), &m) || !IS_NATIVE(m)) {
          if (IS_MAP(recv)) ERR("Map has no callable field or method '%s'.", name->chars);
          ERR("%s has no method '%s'.", value_type_name(recv), name->chars);
        }
        ObjNative* n = AS_NATIVE(m);
        CHECK(check_native_args(vm, n, argc + 1, 1));
        if (vm->stackTop + 64 >= vm->stack + STACK_MAX) ERR("Stack overflow.");
        Value ret = NIL_VAL;
        CHECK(n->fn(vm, argc + 1, vm->stackTop - argc - 1, &ret, n->ud));
        vm->stackTop -= argc + 1;
        PUSH(ret);
        LOAD();
        break;
      }
      case OP_RETURN: {
        Value result = POP();
        close_upvalues(vm, frame->slots);
        vm->frameCount--;
        vm->stackTop = frame->slots;
        PUSH(result);
        if (vm->frameCount == baseFrames) return true;
        LOAD();
        break;
      }
      case OP_CLOSURE: {
        ObjFunction* fn = AS_FUNCTION(READ_CONST());
        ObjClosure* c = closure_new(vm, fn);
        PUSH(OBJ_VAL(c));
        for (int i = 0; i < c->upvalueCount; i++) {
          uint8_t isLocal = READ_BYTE();
          uint8_t index = READ_BYTE();
          if (isLocal) c->upvalues[i] = capture_upvalue(vm, frame->slots + index);
          else c->upvalues[i] = frame->closure->upvalues[index];
        }
        break;
      }
      case OP_BUILD_LIST: {
        int n = READ_U16();
        ObjList* l = list_new(vm);
        if (n > 0) {
          l->items = MEM_ALLOC(vm, Value, n);
          l->cap = n;
          memcpy(l->items, vm->stackTop - n, sizeof(Value) * (size_t)n);
          l->count = n;
        }
        vm->stackTop -= n;
        PUSH(OBJ_VAL(l));
        break;
      }
      case OP_BUILD_MAP: {
        int n = READ_U16();
        ObjMap* m = map_new(vm);
        for (int i = 0; i < n; i++) {
          Value k = vm->stackTop[-2 * n + 2 * i];
          Value v = vm->stackTop[-2 * n + 2 * i + 1];
          CHECK(valid_key(vm, k));
          map_set(vm, m, k, v);
        }
        vm->stackTop -= 2 * n;
        PUSH(OBJ_VAL(m));
        break;
      }
      case OP_ITER_INIT: CHECK(iter_init(vm, &vm->stackTop[-1])); break;
      case OP_FOR_NEXT: {
        uint16_t off = READ_U16();
        Value it = PEEK(1);
        double idx = AS_NUM(PEEK(0));
        Value elem;
        bool has = false;
        switch (AS_OBJ(it)->type) {
          case OBJ_LIST: {
            ObjList* l = AS_LIST(it);
            if (idx < l->count) {
              elem = l->items[(int)idx];
              idx += 1;
              has = true;
            }
            break;
          }
          case OBJ_RANGE: {
            ObjRange* r = AS_RANGE(it);
            if (idx < (double)range_len(r)) {
              elem = NUM_VAL(r->start + r->step * idx);
              idx += 1;
              has = true;
            }
            break;
          }
          case OBJ_BYTES: {
            ObjBytes* b = AS_BYTES(it);
            if (idx < b->len) {
              elem = NUM_VAL(b->data[(int)idx]);
              idx += 1;
              has = true;
            }
            break;
          }
          case OBJ_STRING: {
            ObjString* s = AS_STRING(it);
            if (idx < s->len) {
              int n = utf8_len_at(s->chars + (int)idx, s->len - (int)idx);
              elem = OBJ_VAL(str_copy(vm, s->chars + (int)idx, (size_t)n));
              idx += n;
              has = true;
            }
            break;
          }
          default: break;
        }
        if (has) {
          vm->stackTop[-1] = NUM_VAL(idx);
          PUSH(elem);
        } else {
          ip += off;
        }
        break;
      }
      default: ERR("Invalid opcode %d.", instruction);
    }
  }

fail : {
  int n = vm->frameCount - baseFrames;
  for (int k = 0; k < n; k++) {
    if (n > 24 && k >= 12 && k < n - 6) {
      if (k == 12) buf_addf(&vm->trace, "  ... %d more frames\n", n - 18);
      continue;
    }
    CallFrame* f = &vm->frames[vm->frameCount - 1 - k];
    ObjFunction* fn = f->closure->fn;
    int off = (int)(f->ip - fn->chunk.code) - 1;
    buf_addf(&vm->trace, "  [%s:%d] in %s\n", fn->file ? fn->file->chars : "?", chunk_line(&fn->chunk, off),
             fn->name ? fn->name->chars : "<script>");
  }
  return false;
}
#undef READ_BYTE
#undef READ_U16
#undef READ_CONST
#undef READ_STR
#undef PUSH
#undef POP
#undef PEEK
#undef SAVE
#undef LOAD
#undef ERR
#undef CHECK
#undef SAFEPOINT
#undef NUM_BINOP
#undef INT_BINOP
#undef CMP_OP
}
