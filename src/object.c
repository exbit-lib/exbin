/* EXBIN: heap objects, strings, containers, printing and the garbage
 * collector (mark & sweep, run only at interpreter safepoints). */
#include "vm.h"

/* ------------------------------------------------------------------ Buf -- */
void buf_init(Buf* b) {
  b->d = NULL;
  b->len = b->cap = 0;
}
void buf_free(Buf* b) {
  free(b->d);
  buf_init(b);
}
static void buf_reserve(Buf* b, size_t extra) {
  if (b->len + extra + 1 <= b->cap) return;
  size_t nc = b->cap ? b->cap : 64;
  while (nc < b->len + extra + 1) nc *= 2;
  char* nd = (char*)realloc(b->d, nc);
  if (!nd) {
    fprintf(stderr, "exbin: out of memory\n");
    exit(1);
  }
  b->d = nd;
  b->cap = nc;
}
void buf_add(Buf* b, const char* s, size_t n) {
  buf_reserve(b, n);
  if (n) memcpy(b->d + b->len, s, n);
  b->len += n;
  b->d[b->len] = 0;
}
void buf_addc(Buf* b, char c) { buf_add(b, &c, 1); }
void buf_adds(Buf* b, const char* s) { buf_add(b, s, strlen(s)); }
void buf_addf(Buf* b, const char* fmt, ...) {
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n < 0) {
    va_end(ap2);
    return;
  }
  buf_reserve(b, (size_t)n);
  vsnprintf(b->d + b->len, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  b->len += (size_t)n;
}
const char* buf_str(Buf* b) {
  buf_reserve(b, 0);
  b->d[b->len] = 0;
  return b->d;
}
char* buf_take(Buf* b) {
  buf_reserve(b, 0);
  b->d[b->len] = 0;
  char* r = b->d;
  buf_init(b);
  return r;
}

/* --------------------------------------------------------------- memory -- */
void* ex_realloc(VM* vm, void* p, size_t oldSize, size_t newSize) {
  vm->bytesAllocated += newSize;
  vm->bytesAllocated -= oldSize;
  if (newSize == 0) {
    free(p);
    return NULL;
  }
  void* r = realloc(p, newSize);
  if (!r) {
    fprintf(stderr, "exbin: out of memory\n");
    exit(1);
  }
  return r;
}

static Obj* alloc_obj(VM* vm, size_t size, ObjType type) {
  Obj* o = (Obj*)ex_realloc(vm, NULL, 0, size);
  o->type = type;
  o->marked = false;
  o->next = vm->objects;
  vm->objects = o;
  return o;
}
#define NEW_OBJ(vm, T, type) ((T*)alloc_obj((vm), sizeof(T), (type)))

/* -------------------------------------------------------------- strings -- */
uint32_t hash_bytes(const char* s, size_t len) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; i++) {
    h ^= (uint8_t)s[i];
    h *= 16777619u;
  }
  return h;
}

ObjString* str_copy(VM* vm, const char* s, size_t len) {
  ObjString* o = (ObjString*)alloc_obj(vm, sizeof(ObjString) + len + 1, OBJ_STRING);
  o->len = (int)len;
  if (len) memcpy(o->chars, s, len);
  o->chars[len] = 0;
  o->hash = hash_bytes(o->chars, len);
  return o;
}
ObjString* str_from(VM* vm, const char* cstr) { return str_copy(vm, cstr, strlen(cstr)); }

ObjString* str_concat(VM* vm, ObjString* a, ObjString* b) {
  size_t len = (size_t)a->len + (size_t)b->len;
  ObjString* o = (ObjString*)alloc_obj(vm, sizeof(ObjString) + len + 1, OBJ_STRING);
  o->len = (int)len;
  memcpy(o->chars, a->chars, (size_t)a->len);
  memcpy(o->chars + a->len, b->chars, (size_t)b->len);
  o->chars[len] = 0;
  o->hash = hash_bytes(o->chars, len);
  return o;
}

/* ---------------------------------------------------------------- lists -- */
ObjList* list_new(VM* vm) {
  ObjList* l = NEW_OBJ(vm, ObjList, OBJ_LIST);
  l->count = l->cap = 0;
  l->items = NULL;
  return l;
}
void list_push(VM* vm, ObjList* l, Value v) {
  if (l->count + 1 > l->cap) {
    int nc = l->cap < 8 ? 8 : l->cap * 2;
    l->items = MEM_GROW(vm, Value, l->items, l->cap, nc);
    l->cap = nc;
  }
  l->items[l->count++] = v;
}
void list_insert(VM* vm, ObjList* l, int idx, Value v) {
  list_push(vm, l, NIL_VAL);
  memmove(&l->items[idx + 1], &l->items[idx], sizeof(Value) * (size_t)(l->count - 1 - idx));
  l->items[idx] = v;
}
Value list_remove(ObjList* l, int idx) {
  Value v = l->items[idx];
  memmove(&l->items[idx], &l->items[idx + 1], sizeof(Value) * (size_t)(l->count - 1 - idx));
  l->count--;
  return v;
}

/* ----------------------------------------------------------------- maps -- */
ObjMap* map_new(VM* vm) {
  ObjMap* m = NEW_OBJ(vm, ObjMap, OBJ_MAP);
  m->count = m->used = m->cap = 0;
  m->entries = NULL;
  m->index = NULL;
  m->icap = 0;
  return m;
}

uint32_t hash_value(Value v) {
  switch (v.type) {
    case EX_NIL: return 7;
    case EX_BOOL: return v.as.b ? 3 : 5;
    case EX_NUM: {
      double d = v.as.n;
      if (d == 0) d = 0; /* -0 == 0 */
      uint64_t bits;
      memcpy(&bits, &d, sizeof bits);
      bits ^= bits >> 33;
      bits *= 0xff51afd7ed558ccdULL;
      bits ^= bits >> 33;
      return (uint32_t)bits;
    }
    default:
      if (AS_OBJ(v)->type == OBJ_STRING) return AS_STRING(v)->hash;
      if (AS_OBJ(v)->type == OBJ_BYTES) return hash_bytes((const char*)AS_BYTES(v)->data, (size_t)AS_BYTES(v)->len);
      return (uint32_t)(((uintptr_t)AS_OBJ(v)) >> 4) * 2654435761u;
  }
}

bool values_equal(Value a, Value b) {
  if (a.type != b.type) return false;
  switch (a.type) {
    case EX_NIL: return true;
    case EX_BOOL: return a.as.b == b.as.b;
    case EX_NUM: return a.as.n == b.as.n;
    default: {
      if (AS_OBJ(a) == AS_OBJ(b)) return true;
      if (AS_OBJ(a)->type != AS_OBJ(b)->type) return false;
      if (AS_OBJ(a)->type == OBJ_STRING) {
        ObjString *x = AS_STRING(a), *y = AS_STRING(b);
        return x->len == y->len && x->hash == y->hash && memcmp(x->chars, y->chars, (size_t)x->len) == 0;
      }
      if (AS_OBJ(a)->type == OBJ_BYTES) {
        ObjBytes *x = AS_BYTES(a), *y = AS_BYTES(b);
        return x->len == y->len && memcmp(x->data, y->data, (size_t)x->len) == 0;
      }
      return false;
    }
  }
}

static int map_lookup(ObjMap* m, Value key) {
  if (m->icap == 0) return -1;
  uint32_t mask = (uint32_t)m->icap - 1;
  uint32_t i = hash_value(key) & mask;
  for (;;) {
    int e = m->index[i];
    if (e == -1) return -1;
    if (e >= 0 && values_equal(m->entries[e].key, key)) return e;
    i = (i + 1) & mask;
  }
}

static void map_rebuild(VM* vm, ObjMap* m, int newCap) {
  MapEntry* ne = MEM_ALLOC(vm, MapEntry, newCap);
  int icap = newCap * 2;
  int* ni = MEM_ALLOC(vm, int, icap);
  for (int i = 0; i < icap; i++) ni[i] = -1;
  int j = 0;
  for (int i = 0; i < m->used; i++) {
    if (IS_NIL(m->entries[i].key)) continue;
    ne[j] = m->entries[i];
    uint32_t mask = (uint32_t)icap - 1;
    uint32_t h = hash_value(ne[j].key) & mask;
    while (ni[h] >= 0) h = (h + 1) & mask;
    ni[h] = j;
    j++;
  }
  if (m->entries) MEM_FREE(vm, MapEntry, m->entries, m->cap);
  if (m->index) MEM_FREE(vm, int, m->index, m->icap);
  m->entries = ne;
  m->index = ni;
  m->cap = newCap;
  m->icap = icap;
  m->used = j;
}

bool map_get(ObjMap* m, Value key, Value* out) {
  int e = map_lookup(m, key);
  if (e < 0) return false;
  if (out) *out = m->entries[e].value;
  return true;
}

bool map_set(VM* vm, ObjMap* m, Value key, Value v) {
  int e = map_lookup(m, key);
  if (e >= 0) {
    m->entries[e].value = v;
    return false;
  }
  if (m->used + 1 > m->cap) {
    int nc = m->cap < 8 ? 8 : m->cap;
    if (m->count + 1 > m->cap / 2) nc = m->cap < 8 ? 8 : m->cap * 2;
    map_rebuild(vm, m, nc);
  }
  int idx = m->used++;
  m->entries[idx].key = key;
  m->entries[idx].value = v;
  uint32_t mask = (uint32_t)m->icap - 1;
  uint32_t h = hash_value(key) & mask;
  while (m->index[h] >= 0) h = (h + 1) & mask;
  m->index[h] = idx;
  m->count++;
  return true;
}

bool map_delete(ObjMap* m, Value key) {
  if (m->icap == 0) return false;
  uint32_t mask = (uint32_t)m->icap - 1;
  uint32_t i = hash_value(key) & mask;
  for (;;) {
    int e = m->index[i];
    if (e == -1) return false;
    if (e >= 0 && values_equal(m->entries[e].key, key)) {
      m->index[i] = -2;
      m->entries[e].key = NIL_VAL;
      m->entries[e].value = NIL_VAL;
      m->count--;
      return true;
    }
    i = (i + 1) & mask;
  }
}

bool map_get_str(VM* vm, ObjMap* m, const char* key, Value* out) {
  /* temporary string; no GC can run here (safepoint-only collector) */
  ObjString* s = str_from(vm, key);
  return map_get(m, OBJ_VAL(s), out);
}
void map_set_str(VM* vm, ObjMap* m, const char* key, Value v) {
  map_set(vm, m, OBJ_VAL(str_from(vm, key)), v);
}

/* ---------------------------------------------------------------- bytes -- */
ObjBytes* bytes_new(VM* vm, int len) {
  ObjBytes* b = NEW_OBJ(vm, ObjBytes, OBJ_BYTES);
  b->len = len;
  b->cap = len;
  b->data = NULL;
  if (len > 0) {
    b->data = MEM_ALLOC(vm, uint8_t, len);
    memset(b->data, 0, (size_t)len);
  }
  return b;
}
void bytes_push(VM* vm, ObjBytes* b, uint8_t byte) {
  if (b->len + 1 > b->cap) {
    int nc = b->cap < 16 ? 16 : b->cap * 2;
    b->data = MEM_GROW(vm, uint8_t, b->data, b->cap, nc);
    b->cap = nc;
  }
  b->data[b->len++] = byte;
}

/* --------------------------------------------------------------- ranges -- */
ObjRange* range_new(VM* vm, double start, double stop, double step) {
  ObjRange* r = NEW_OBJ(vm, ObjRange, OBJ_RANGE);
  r->start = start;
  r->stop = stop;
  r->step = step;
  return r;
}
int64_t range_len(const ObjRange* r) {
  if (r->step == 0) return 0;
  double n = (r->stop - r->start) / r->step;
  if (!(n > 0)) return 0;
  return (int64_t)ceil(n);
}

/* ------------------------------------------------- functions & friends --- */
ObjFunction* fn_new(VM* vm) {
  ObjFunction* f = NEW_OBJ(vm, ObjFunction, OBJ_FUNCTION);
  f->arity = 0;
  f->upvalueCount = 0;
  f->maxStack = 0;
  f->name = NULL;
  f->file = NULL;
  chunk_init(&f->chunk);
  return f;
}
ObjClosure* closure_new(VM* vm, ObjFunction* fn) {
  ObjUpvalue** ups = NULL;
  if (fn->upvalueCount > 0) {
    ups = MEM_ALLOC(vm, ObjUpvalue*, fn->upvalueCount);
    for (int i = 0; i < fn->upvalueCount; i++) ups[i] = NULL;
  }
  ObjClosure* c = NEW_OBJ(vm, ObjClosure, OBJ_CLOSURE);
  c->fn = fn;
  c->upvalues = ups;
  c->upvalueCount = fn->upvalueCount;
  return c;
}
ObjUpvalue* upvalue_new(VM* vm, Value* slot) {
  ObjUpvalue* u = NEW_OBJ(vm, ObjUpvalue, OBJ_UPVALUE);
  u->location = slot;
  u->closed = NIL_VAL;
  u->next = NULL;
  return u;
}
ObjNative* native_new(VM* vm, const char* name, ExNative fn, void* ud, int minArgs,
                      int maxArgs, void (*udFree)(void*)) {
  ObjNative* n = NEW_OBJ(vm, ObjNative, OBJ_NATIVE);
  n->fn = fn;
  n->ud = ud;
  n->udFree = udFree;
  n->minArgs = minArgs;
  n->maxArgs = maxArgs;
  size_t l = strlen(name);
  n->name = (char*)malloc(l + 1);
  memcpy(n->name, name, l + 1);
  return n;
}
ObjHandle* handle_new(VM* vm, const char* kind, void* data, void (*mark)(VM*, void*),
                      void (*destroy)(void*)) {
  ObjHandle* h = NEW_OBJ(vm, ObjHandle, OBJ_HANDLE);
  h->kind = kind;
  h->data = data;
  h->mark = mark;
  h->destroy = destroy;
  return h;
}

/* ------------------------------------------------------------ printing --- */
bool value_truthy(Value v) {
  if (IS_NIL(v)) return false;
  if (IS_BOOL(v)) return AS_BOOL(v) != 0;
  return true;
}

const char* value_type_name(Value v) {
  switch (v.type) {
    case EX_NIL: return "nil";
    case EX_BOOL: return "bool";
    case EX_NUM: return "number";
    default:
      switch (AS_OBJ(v)->type) {
        case OBJ_STRING: return "string";
        case OBJ_LIST: return "list";
        case OBJ_MAP: return "map";
        case OBJ_BYTES: return "bytes";
        case OBJ_RANGE: return "range";
        case OBJ_FUNCTION:
        case OBJ_CLOSURE: return "function";
        case OBJ_NATIVE: return "native";
        case OBJ_HANDLE: return AS_HANDLE(v)->kind;
        default: return "object";
      }
  }
}

void fmt_number(Buf* b, double n) {
  if (isnan(n)) {
    buf_adds(b, "nan");
  } else if (isinf(n)) {
    buf_adds(b, n < 0 ? "-inf" : "inf");
  } else if (n == 0) {
    buf_adds(b, "0");
  } else if (n == floor(n) && fabs(n) < 1e15) {
    buf_addf(b, "%.0f", n);
  } else {
    buf_addf(b, "%.14g", n);
  }
}

static bool is_ident_like(const ObjString* s) {
  if (s->len == 0) return false;
  for (int i = 0; i < s->len; i++) {
    unsigned char c = (unsigned char)s->chars[i];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80 ||
              (i > 0 && c >= '0' && c <= '9');
    if (!ok) return false;
  }
  return true;
}

static void repr_string(Buf* b, const ObjString* s) {
  buf_addc(b, '"');
  for (int i = 0; i < s->len; i++) {
    unsigned char c = (unsigned char)s->chars[i];
    switch (c) {
      case '\n': buf_adds(b, "\\n"); break;
      case '\r': buf_adds(b, "\\r"); break;
      case '\t': buf_adds(b, "\\t"); break;
      case '\\': buf_adds(b, "\\\\"); break;
      case '"': buf_adds(b, "\\\""); break;
      default:
        if (c < 0x20 || c == 0x7f) buf_addf(b, "\\x%02x", c);
        else buf_addc(b, (char)c);
    }
  }
  buf_addc(b, '"');
}

void value_repr_buf(VM* vm, Buf* b, Value v, int depth) {
  switch (v.type) {
    case EX_NIL: buf_adds(b, "nil"); return;
    case EX_BOOL: buf_adds(b, v.as.b ? "true" : "false"); return;
    case EX_NUM: fmt_number(b, v.as.n); return;
    default: break;
  }
  switch (AS_OBJ(v)->type) {
    case OBJ_STRING: repr_string(b, AS_STRING(v)); return;
    case OBJ_LIST: {
      ObjList* l = AS_LIST(v);
      if (depth > 12) {
        buf_adds(b, "[...]");
        return;
      }
      buf_addc(b, '[');
      for (int i = 0; i < l->count; i++) {
        if (i) buf_adds(b, ", ");
        if (i >= 1000) {
          buf_addf(b, "... (%d more)", l->count - i);
          break;
        }
        value_repr_buf(vm, b, l->items[i], depth + 1);
      }
      buf_addc(b, ']');
      return;
    }
    case OBJ_MAP: {
      ObjMap* m = AS_MAP(v);
      if (depth > 12) {
        buf_adds(b, "{...}");
        return;
      }
      buf_addc(b, '{');
      bool first = true;
      int shown = 0;
      for (int i = 0; i < m->used; i++) {
        if (IS_NIL(m->entries[i].key)) continue;
        if (!first) buf_adds(b, ", ");
        first = false;
        if (shown++ >= 1000) {
          buf_adds(b, "...");
          break;
        }
        Value k = m->entries[i].key;
        if (IS_STRING(k) && is_ident_like(AS_STRING(k))) buf_add(b, AS_CSTRING(k), (size_t)AS_STRING(k)->len);
        else if (IS_NUM(k) || IS_STRING(k)) value_repr_buf(vm, b, k, depth + 1);
        else {
          buf_addc(b, '[');
          value_repr_buf(vm, b, k, depth + 1);
          buf_addc(b, ']');
        }
        buf_adds(b, ": ");
        value_repr_buf(vm, b, m->entries[i].value, depth + 1);
      }
      buf_addc(b, '}');
      return;
    }
    case OBJ_BYTES: {
      ObjBytes* by = AS_BYTES(v);
      buf_addf(b, "<bytes %d:", by->len);
      for (int i = 0; i < by->len && i < 32; i++) buf_addf(b, " %02x", by->data[i]);
      if (by->len > 32) buf_adds(b, " ...");
      buf_addc(b, '>');
      return;
    }
    case OBJ_RANGE: {
      ObjRange* r = AS_RANGE(v);
      buf_adds(b, "range(");
      fmt_number(b, r->start);
      buf_adds(b, ", ");
      fmt_number(b, r->stop);
      if (r->step != 1) {
        buf_adds(b, ", ");
        fmt_number(b, r->step);
      }
      buf_addc(b, ')');
      return;
    }
    case OBJ_FUNCTION:
    case OBJ_CLOSURE: {
      ObjFunction* f = IS_FUNCTION(v) ? AS_FUNCTION(v) : AS_CLOSURE(v)->fn;
      if (f->name) buf_addf(b, "<fn %s>", f->name->chars);
      else buf_adds(b, "<script>");
      return;
    }
    case OBJ_NATIVE: buf_addf(b, "<native %s>", AS_NATIVE(v)->name); return;
    case OBJ_HANDLE: buf_addf(b, "<%s>", AS_HANDLE(v)->kind); return;
    default: buf_adds(b, "<object>"); return;
  }
}

void value_tostr_buf(VM* vm, Buf* b, Value v) {
  if (IS_STRING(v)) buf_add(b, AS_CSTRING(v), (size_t)AS_STRING(v)->len);
  else value_repr_buf(vm, b, v, 0);
}

ObjString* value_tostring(VM* vm, Value v) {
  if (IS_STRING(v)) return AS_STRING(v);
  Buf b;
  buf_init(&b);
  value_tostr_buf(vm, &b, v);
  ObjString* s = str_copy(vm, b.d ? b.d : "", b.len);
  buf_free(&b);
  return s;
}
ObjString* value_repr(VM* vm, Value v) {
  Buf b;
  buf_init(&b);
  value_repr_buf(vm, &b, v, 0);
  ObjString* s = str_copy(vm, b.d ? b.d : "", b.len);
  buf_free(&b);
  return s;
}

/* ------------------------------------------------------------------- gc -- */
void gc_mark_obj(VM* vm, Obj* o) {
  if (o == NULL || o->marked) return;
  o->marked = true;
  if (vm->grayCount + 1 > vm->grayCap) {
    vm->grayCap = vm->grayCap < 64 ? 64 : vm->grayCap * 2;
    vm->gray = (Obj**)realloc(vm->gray, sizeof(Obj*) * (size_t)vm->grayCap);
    if (!vm->gray) {
      fprintf(stderr, "exbin: out of memory\n");
      exit(1);
    }
  }
  vm->gray[vm->grayCount++] = o;
}
void gc_mark_value(VM* vm, Value v) {
  if (IS_OBJ(v)) gc_mark_obj(vm, AS_OBJ(v));
}

static void blacken(VM* vm, Obj* o) {
  switch (o->type) {
    case OBJ_STRING:
    case OBJ_RANGE:
    case OBJ_BYTES:
    case OBJ_NATIVE: break;
    case OBJ_LIST: {
      ObjList* l = (ObjList*)o;
      for (int i = 0; i < l->count; i++) gc_mark_value(vm, l->items[i]);
      break;
    }
    case OBJ_MAP: {
      ObjMap* m = (ObjMap*)o;
      for (int i = 0; i < m->used; i++) {
        if (IS_NIL(m->entries[i].key)) continue;
        gc_mark_value(vm, m->entries[i].key);
        gc_mark_value(vm, m->entries[i].value);
      }
      break;
    }
    case OBJ_FUNCTION: {
      ObjFunction* f = (ObjFunction*)o;
      gc_mark_obj(vm, (Obj*)f->name);
      gc_mark_obj(vm, (Obj*)f->file);
      for (int i = 0; i < f->chunk.consts.count; i++) gc_mark_value(vm, f->chunk.consts.v[i]);
      break;
    }
    case OBJ_CLOSURE: {
      ObjClosure* c = (ObjClosure*)o;
      gc_mark_obj(vm, (Obj*)c->fn);
      for (int i = 0; i < c->upvalueCount; i++) gc_mark_obj(vm, (Obj*)c->upvalues[i]);
      break;
    }
    case OBJ_UPVALUE: gc_mark_value(vm, ((ObjUpvalue*)o)->closed); break;
    case OBJ_HANDLE: {
      ObjHandle* h = (ObjHandle*)o;
      if (h->mark) h->mark(vm, h->data);
      break;
    }
  }
}

static void free_object(VM* vm, Obj* o) {
  switch (o->type) {
    case OBJ_STRING: ex_realloc(vm, o, sizeof(ObjString) + (size_t)((ObjString*)o)->len + 1, 0); break;
    case OBJ_LIST: {
      ObjList* l = (ObjList*)o;
      if (l->items) MEM_FREE(vm, Value, l->items, l->cap);
      MEM_FREE(vm, ObjList, o, 1);
      break;
    }
    case OBJ_MAP: {
      ObjMap* m = (ObjMap*)o;
      if (m->entries) MEM_FREE(vm, MapEntry, m->entries, m->cap);
      if (m->index) MEM_FREE(vm, int, m->index, m->icap);
      MEM_FREE(vm, ObjMap, o, 1);
      break;
    }
    case OBJ_BYTES: {
      ObjBytes* b = (ObjBytes*)o;
      if (b->data) MEM_FREE(vm, uint8_t, b->data, b->cap);
      MEM_FREE(vm, ObjBytes, o, 1);
      break;
    }
    case OBJ_RANGE: MEM_FREE(vm, ObjRange, o, 1); break;
    case OBJ_FUNCTION: {
      ObjFunction* f = (ObjFunction*)o;
      chunk_free(vm, &f->chunk);
      MEM_FREE(vm, ObjFunction, o, 1);
      break;
    }
    case OBJ_CLOSURE: {
      ObjClosure* c = (ObjClosure*)o;
      if (c->upvalues) MEM_FREE(vm, ObjUpvalue*, c->upvalues, c->upvalueCount);
      MEM_FREE(vm, ObjClosure, o, 1);
      break;
    }
    case OBJ_UPVALUE: MEM_FREE(vm, ObjUpvalue, o, 1); break;
    case OBJ_NATIVE: {
      ObjNative* n = (ObjNative*)o;
      if (n->udFree && n->ud) n->udFree(n->ud);
      free(n->name);
      MEM_FREE(vm, ObjNative, o, 1);
      break;
    }
    case OBJ_HANDLE: {
      ObjHandle* h = (ObjHandle*)o;
      if (h->destroy && h->data) h->destroy(h->data);
      MEM_FREE(vm, ObjHandle, o, 1);
      break;
    }
  }
}

void gc_collect(VM* vm) {
  /* roots */
  for (Value* s = vm->stack; s < vm->stackTop; s++) gc_mark_value(vm, *s);
  for (int i = 0; i < vm->frameCount; i++) gc_mark_obj(vm, (Obj*)vm->frames[i].closure);
  for (ObjUpvalue* u = vm->openUpvalues; u; u = u->next) gc_mark_obj(vm, (Obj*)u);
  gc_mark_obj(vm, (Obj*)vm->globals);
  gc_mark_obj(vm, (Obj*)vm->modules);
  for (int i = 0; i < METH_COUNT; i++) gc_mark_obj(vm, (Obj*)vm->methods[i]);
  gc_mark_value(vm, vm->errval);
  /* trace */
  while (vm->grayCount > 0) blacken(vm, vm->gray[--vm->grayCount]);
  /* sweep */
  Obj** p = &vm->objects;
  while (*p) {
    if ((*p)->marked) {
      (*p)->marked = false;
      p = &(*p)->next;
    } else {
      Obj* dead = *p;
      *p = dead->next;
      free_object(vm, dead);
    }
  }
  size_t next = vm->bytesAllocated * 2;
  vm->nextGC = next < (1u << 20) ? (1u << 20) : next;
}

void gc_free_all(VM* vm) {
  Obj* o = vm->objects;
  while (o) {
    Obj* n = o->next;
    free_object(vm, o);
    o = n;
  }
  vm->objects = NULL;
  free(vm->gray);
  vm->gray = NULL;
  vm->grayCount = vm->grayCap = 0;
}
