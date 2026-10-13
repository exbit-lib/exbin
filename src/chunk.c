/* EXBIN: bytecode chunks, opcode tables, verifier and disassembler. */
#include <ctype.h>

#include "vm.h"

static const char* OP_NAMES[] = {
#define X(n, k) #n,
    OPCODE_LIST(X)
#undef X
};
static const OpKind OP_KINDS[] = {
#define X(n, k) k,
    OPCODE_LIST(X)
#undef X
};

const char* op_name(int op) { return (op >= 0 && op < OP__COUNT) ? OP_NAMES[op] : "???"; }
OpKind op_kind(int op) { return OP_KINDS[op]; }

int op_from_name(const char* name) {
  for (int i = 0; i < OP__COUNT; i++) {
    const char *a = OP_NAMES[i], *b = name;
    while (*a && *b && toupper((unsigned char)*a) == toupper((unsigned char)*b)) a++, b++;
    if (!*a && !*b) return i;
  }
  return -1;
}

void chunk_init(Chunk* c) {
  c->code = NULL;
  c->lines = NULL;
  c->count = c->cap = 0;
  c->consts.v = NULL;
  c->consts.count = c->consts.cap = 0;
}

void chunk_free(VM* vm, Chunk* c) {
  if (c->code) MEM_FREE(vm, uint8_t, c->code, c->cap);
  if (c->lines) MEM_FREE(vm, int, c->lines, c->cap);
  if (c->consts.v) MEM_FREE(vm, Value, c->consts.v, c->consts.cap);
  chunk_init(c);
}

void chunk_write(VM* vm, Chunk* c, uint8_t byte, int line) {
  if (c->count + 1 > c->cap) {
    int nc = c->cap < 64 ? 64 : c->cap * 2;
    c->code = MEM_GROW(vm, uint8_t, c->code, c->cap, nc);
    c->lines = MEM_GROW(vm, int, c->lines, c->cap, nc);
    c->cap = nc;
  }
  c->code[c->count] = byte;
  c->lines[c->count] = line;
  c->count++;
}

int chunk_add_const(VM* vm, Chunk* c, Value v) {
  ValueArray* a = &c->consts;
  int from = a->count > 256 ? a->count - 256 : 0;
  if (IS_STRING(v) || IS_NUM(v)) {
    for (int i = from; i < a->count; i++) {
      Value o = a->v[i];
      if (o.type != v.type) continue;
      if (IS_NUM(v)) {
        if (memcmp(&o.as.n, &v.as.n, sizeof(double)) == 0) return i;
      } else if (values_equal(o, v)) {
        return i;
      }
    }
  }
  if (a->count + 1 > a->cap) {
    int nc = a->cap < 8 ? 8 : a->cap * 2;
    a->v = MEM_GROW(vm, Value, a->v, a->cap, nc);
    a->cap = nc;
  }
  a->v[a->count] = v;
  return a->count++;
}

int chunk_line(const Chunk* c, int offset) {
  if (!c->lines || c->count == 0) return 0;
  if (offset < 0) offset = 0;
  if (offset >= c->count) offset = c->count - 1;
  return c->lines[offset];
}

static int rd16(const uint8_t* p) { return (p[0] << 8) | p[1]; }

int op_length(const Chunk* c, int offset) {
  if (offset < 0 || offset >= c->count) return -1;
  int op = c->code[offset];
  if (op >= OP__COUNT) return -1;
  switch (OP_KINDS[op]) {
    case OPK_NONE: return 1;
    case OPK_U8: return 2;
    case OPK_U16:
    case OPK_CONST:
    case OPK_NAME:
    case OPK_JUMP:
    case OPK_LOOP: return 3;
    case OPK_INVOKE: return 4;
    case OPK_CLOSURE: {
      if (offset + 3 > c->count) return -1;
      int idx = rd16(&c->code[offset + 1]);
      if (idx >= c->consts.count || !IS_FUNCTION(c->consts.v[idx])) return -1;
      return 3 + 2 * AS_FUNCTION(c->consts.v[idx])->upvalueCount;
    }
  }
  return -1;
}

/* Abstract interpretation of stack depth over the control-flow graph. */
static bool verify_stack(ObjFunction* fn, char* err, size_t errsz) {
  const Chunk* c = &fn->chunk;
  if (c->count <= 0 || c->count > (1 << 26)) {
    snprintf(err, errsz, "bad code size");
    return false;
  }
  int* depth = (int*)malloc(sizeof(int) * (size_t)c->count);
  int* work = (int*)malloc(sizeof(int) * (size_t)c->count * 2 + 16);
  if (!depth || !work) {
    free(depth);
    free(work);
    snprintf(err, errsz, "out of memory");
    return false;
  }
  for (int i = 0; i < c->count; i++) depth[i] = -1;
  int wn = 0, maxd = 0;
  depth[0] = 1 + fn->arity;
  work[wn++] = 0;
  maxd = depth[0];
#define VFAIL(...)                       \
  do {                                   \
    snprintf(err, errsz, __VA_ARGS__);   \
    free(depth);                         \
    free(work);                          \
    return false;                        \
  } while (0)
#define SUCC(target, sd)                                                      \
  do {                                                                        \
    int _t = (target), _d = (sd);                                             \
    if (_t >= c->count) VFAIL("control flow falls off the end at %d", off);   \
    if (_d > maxd) maxd = _d;                                                 \
    if (depth[_t] == -1) {                                                    \
      depth[_t] = _d;                                                         \
      work[wn++] = _t;                                                        \
    } else if (depth[_t] != _d) {                                             \
      VFAIL("inconsistent stack depth (%d vs %d) at %d", depth[_t], _d, _t);  \
    }                                                                         \
  } while (0)
  while (wn > 0) {
    int off = work[--wn];
    int d = depth[off];
    int op = c->code[off];
    int len = op_length(c, off);
    const uint8_t* a = &c->code[off + 1];
    int need = 0, delta = 0;
    switch (op) {
      case OP_CONSTANT: case OP_NIL: case OP_TRUE: case OP_FALSE: delta = 1; break;
      case OP_POP: need = 1; delta = -1; break;
      case OP_DUP: need = 1; delta = 1; break;
      case OP_DUP2: need = 2; delta = 2; break;
      case OP_GET_LOCAL:
        if (a[0] >= d) VFAIL("local slot %d out of range at %d", a[0], off);
        delta = 1;
        break;
      case OP_SET_LOCAL:
        if (a[0] >= d) VFAIL("local slot %d out of range at %d", a[0], off);
        need = 1;
        break;
      case OP_GET_GLOBAL: case OP_GET_UPVALUE: case OP_CLOSURE: delta = 1; break;
      case OP_DEFINE_GLOBAL: case OP_CLOSE_UPVALUE: need = 1; delta = -1; break;
      case OP_SET_GLOBAL: case OP_SET_UPVALUE: need = 1; break;
      case OP_GET_INDEX: need = 2; delta = -1; break;
      case OP_SET_INDEX: need = 3; delta = -2; break;
      case OP_GET_FIELD: need = 1; break;
      case OP_SET_FIELD: need = 2; delta = -1; break;
      case OP_EQUAL: case OP_NOT_EQUAL: case OP_LESS: case OP_LESS_EQUAL:
      case OP_GREATER: case OP_GREATER_EQUAL: case OP_ADD: case OP_SUB:
      case OP_MUL: case OP_DIV: case OP_MOD: case OP_POW: case OP_BAND:
      case OP_BOR: case OP_BXOR: case OP_SHL: case OP_SHR:
        need = 2;
        delta = -1;
        break;
      case OP_NEG: case OP_NOT: case OP_BNOT: case OP_ITER_INIT: need = 1; break;
      case OP_JUMP: case OP_LOOP: break;
      case OP_JUMP_IF_FALSE: case OP_JUMP_IF_TRUE: need = 1; break;
      case OP_FOR_NEXT: need = 2; break;
      case OP_CALL: need = a[0] + 1; delta = -a[0]; break;
      case OP_INVOKE: need = a[2] + 1; delta = -a[2]; break;
      case OP_RETURN: need = 1; break;
      case OP_BUILD_LIST: {
        int n = rd16(a);
        need = n;
        delta = 1 - n;
        break;
      }
      case OP_BUILD_MAP: {
        int n = rd16(a);
        need = 2 * n;
        delta = 1 - 2 * n;
        break;
      }
      default: VFAIL("unknown opcode %d at %d", op, off);
    }
    if (d < need) VFAIL("stack underflow at %d (%s needs %d, has %d)", off, op_name(op), need, d);
    if (op == OP_CLOSURE) {
      /* the closure is pushed before capturing, so its own slot (index d) is valid:
       * that is how a local function refers to itself */
      ObjFunction* f = AS_FUNCTION(c->consts.v[rd16(a)]);
      for (int i = 0; i < f->upvalueCount; i++) {
        if (a[2 + 2 * i] == 1 && a[3 + 2 * i] > d) VFAIL("captured local %d out of range at %d", a[3 + 2 * i], off);
      }
    }
    int nd = d + delta;
    switch (op) {
      case OP_RETURN: break;
      case OP_JUMP: SUCC(off + len + rd16(a), nd); break;
      case OP_LOOP: SUCC(off + len - rd16(a), nd); break;
      case OP_JUMP_IF_FALSE: case OP_JUMP_IF_TRUE:
        SUCC(off + len + rd16(a), nd);
        SUCC(off + len, nd);
        break;
      case OP_FOR_NEXT:
        SUCC(off + len + rd16(a), d);
        SUCC(off + len, d + 1);
        break;
      default: SUCC(off + len, nd); break;
    }
  }
#undef SUCC
#undef VFAIL
  fn->maxStack = maxd + 2;
  free(depth);
  free(work);
  return true;
}

bool chunk_verify(ObjFunction* fn, char* err, size_t errsz) {
  const Chunk* c = &fn->chunk;
#define FAIL(...)                                  \
  do {                                             \
    snprintf(err, errsz, __VA_ARGS__);             \
    return false;                                  \
  } while (0)
  if (fn->arity < 0 || fn->arity > 255) FAIL("bad arity");
  if (fn->upvalueCount < 0 || fn->upvalueCount > 255) FAIL("bad upvalue count");
  if (c->count <= 0 || c->count > (1 << 26)) FAIL("bad code size");
  if (c->count > 0 && !c->lines) FAIL("missing line table");

  char* boundary = (char*)calloc((size_t)c->count + 1, 1);
  if (!boundary) FAIL("out of memory");
  /* pass 1: decode */
  int last = -1;
  for (int off = 0; off < c->count;) {
    int len = op_length(c, off);
    if (len < 0) {
      free(boundary);
      FAIL("malformed instruction at %d", off);
    }
    if (off + len > c->count) {
      free(boundary);
      FAIL("truncated instruction at %d", off);
    }
    boundary[off] = 1;
    last = off;
    off += len;
  }
  if (c->code[last] != OP_RETURN) {
    free(boundary);
    FAIL("code does not end with RETURN");
  }
  /* pass 2: operands */
  for (int off = 0; off < c->count;) {
    int op = c->code[off];
    int len = op_length(c, off);
    const uint8_t* a = &c->code[off + 1];
    switch (OP_KINDS[op]) {
      case OPK_CONST:
      case OPK_NAME:
      case OPK_INVOKE: {
        int idx = rd16(a);
        if (idx >= c->consts.count) {
          free(boundary);
          FAIL("constant index out of range at %d", off);
        }
        if (OP_KINDS[op] != OPK_CONST && !IS_STRING(c->consts.v[idx])) {
          free(boundary);
          FAIL("name constant is not a string at %d", off);
        }
        break;
      }
      case OPK_JUMP: {
        int t = off + len + rd16(a);
        if (t >= c->count || !boundary[t]) {
          free(boundary);
          FAIL("bad jump target at %d", off);
        }
        break;
      }
      case OPK_LOOP: {
        int t = off + len - rd16(a);
        if (t < 0 || !boundary[t]) {
          free(boundary);
          FAIL("bad loop target at %d", off);
        }
        break;
      }
      case OPK_U8:
        if ((op == OP_GET_UPVALUE || op == OP_SET_UPVALUE) && a[0] >= fn->upvalueCount) {
          free(boundary);
          FAIL("upvalue index out of range at %d", off);
        }
        break;
      case OPK_CLOSURE: {
        int idx = rd16(a);
        ObjFunction* f = AS_FUNCTION(c->consts.v[idx]);
        for (int i = 0; i < f->upvalueCount; i++) {
          int isLocal = a[2 + 2 * i];
          int index = a[3 + 2 * i];
          if (isLocal > 1 || (!isLocal && index >= fn->upvalueCount)) {
            free(boundary);
            FAIL("bad upvalue descriptor at %d", off);
          }
        }
        break;
      }
      default: break;
    }
    off += len;
  }
  free(boundary);
  if (!verify_stack(fn, err, errsz)) return false;
  /* nested functions */
  for (int i = 0; i < c->consts.count; i++) {
    if (IS_FUNCTION(c->consts.v[i]) && !chunk_verify(AS_FUNCTION(c->consts.v[i]), err, errsz)) return false;
  }
  return true;
#undef FAIL
}

static void dis_fn(const ObjFunction* fn, Buf* out, int depth) {
  const Chunk* c = &fn->chunk;
  buf_addf(out, "== %s (arity %d, upvalues %d, %d bytes, %d consts) ==\n",
           fn->name ? fn->name->chars : "<script>", fn->arity, fn->upvalueCount, c->count,
           c->consts.count);
  int prevLine = -1;
  for (int off = 0; off < c->count;) {
    int len = op_length(c, off);
    if (len < 0) {
      buf_addf(out, "%04d  <malformed>\n", off);
      break;
    }
    int op = c->code[off];
    int line = c->lines ? c->lines[off] : 0;
    if (line == prevLine) buf_addf(out, "%04d    | ", off);
    else buf_addf(out, "%04d %4d ", off, line);
    prevLine = line;
    buf_addf(out, "%-14s", OP_NAMES[op]);
    const uint8_t* a = &c->code[off + 1];
    switch (OP_KINDS[op]) {
      case OPK_NONE: break;
      case OPK_U8: buf_addf(out, " %d", a[0]); break;
      case OPK_U16: buf_addf(out, " %d", rd16(a)); break;
      case OPK_CONST:
      case OPK_NAME: {
        int idx = rd16(a);
        buf_addf(out, " %d  ", idx);
        if (idx < c->consts.count) value_repr_buf(NULL, out, c->consts.v[idx], 0);
        break;
      }
      case OPK_JUMP: buf_addf(out, " -> %04d", off + len + rd16(a)); break;
      case OPK_LOOP: buf_addf(out, " -> %04d", off + len - rd16(a)); break;
      case OPK_INVOKE: {
        int idx = rd16(a);
        buf_addf(out, " %d  ", idx);
        if (idx < c->consts.count) value_repr_buf(NULL, out, c->consts.v[idx], 0);
        buf_addf(out, " (%d args)", a[2]);
        break;
      }
      case OPK_CLOSURE: {
        int idx = rd16(a);
        buf_addf(out, " %d  ", idx);
        if (idx < c->consts.count) value_repr_buf(NULL, out, c->consts.v[idx], 0);
        if (idx < c->consts.count && IS_FUNCTION(c->consts.v[idx])) {
          ObjFunction* f = AS_FUNCTION(c->consts.v[idx]);
          for (int i = 0; i < f->upvalueCount; i++)
            buf_addf(out, "  [%s %d]", a[2 + 2 * i] ? "local" : "upvalue", a[3 + 2 * i]);
        }
        break;
      }
    }
    buf_addc(out, '\n');
    off += len;
  }
  if (depth < 32) {
    for (int i = 0; i < c->consts.count; i++) {
      if (IS_FUNCTION(c->consts.v[i])) {
        buf_addc(out, '\n');
        dis_fn(AS_FUNCTION(c->consts.v[i]), out, depth + 1);
      }
    }
  }
}

void chunk_disassemble(const ObjFunction* fn, Buf* out) { dis_fn(fn, out, 0); }
