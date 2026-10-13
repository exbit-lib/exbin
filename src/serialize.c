/* EXBIN: .exbc bytecode container — write, read (hardened) and verify.
 *
 * Layout
 *   "EXBC" u8 version, 3 reserved bytes
 *   function := hasName[u8] (str)? hasFile[u8] (str)?
 *               varint arity, varint upvalues, varint codeLen, code bytes,
 *               varint nRuns, (varint runLen, varint line)*,
 *               varint nConsts, const*
 *   const    := 0 nil | 1 false | 2 true | 3 f64-LE | 4 str | 5 function
 *   str      := varint len, bytes
 */
#include "vm.h"

static const char MAGIC[4] = {'E', 'X', 'B', 'C'};

bool bytecode_check_magic(const char* data, size_t len) { return len >= 4 && memcmp(data, MAGIC, 4) == 0; }

/* ---------------------------------------------------------------- writer -- */
static void w_varint(Buf* b, uint64_t v) {
  while (v >= 0x80) {
    buf_addc(b, (char)((v & 0x7f) | 0x80));
    v >>= 7;
  }
  buf_addc(b, (char)v);
}
static void w_str(Buf* b, const char* s, size_t n) {
  w_varint(b, n);
  buf_add(b, s, n);
}

static bool w_function(VM* vm, ObjFunction* fn, Buf* b, int depth) {
  if (depth > 100) return vm_error(vm, "Function nesting too deep to serialize.");
  buf_addc(b, fn->name ? 1 : 0);
  if (fn->name) w_str(b, fn->name->chars, (size_t)fn->name->len);
  buf_addc(b, fn->file ? 1 : 0);
  if (fn->file) w_str(b, fn->file->chars, (size_t)fn->file->len);
  w_varint(b, (uint64_t)fn->arity);
  w_varint(b, (uint64_t)fn->upvalueCount);
  const Chunk* c = &fn->chunk;
  w_varint(b, (uint64_t)c->count);
  buf_add(b, (const char*)c->code, (size_t)c->count);
  /* line table, run-length encoded */
  int runs = 0;
  for (int i = 0; i < c->count; i++)
    if (i == 0 || c->lines[i] != c->lines[i - 1]) runs++;
  w_varint(b, (uint64_t)runs);
  for (int i = 0; i < c->count;) {
    int j = i;
    while (j < c->count && c->lines[j] == c->lines[i]) j++;
    w_varint(b, (uint64_t)(j - i));
    w_varint(b, (uint64_t)(c->lines[i] < 0 ? 0 : c->lines[i]));
    i = j;
  }
  w_varint(b, (uint64_t)c->consts.count);
  for (int i = 0; i < c->consts.count; i++) {
    Value v = c->consts.v[i];
    switch (v.type) {
      case EX_NIL: buf_addc(b, 0); break;
      case EX_BOOL: buf_addc(b, v.as.b ? 2 : 1); break;
      case EX_NUM: {
        uint64_t bits;
        memcpy(&bits, &v.as.n, 8);
        buf_addc(b, 3);
        for (int k = 0; k < 8; k++) buf_addc(b, (char)((bits >> (8 * k)) & 0xff));
        break;
      }
      default:
        if (IS_STRING(v)) {
          buf_addc(b, 4);
          w_str(b, AS_CSTRING(v), (size_t)AS_STRING(v)->len);
        } else if (IS_FUNCTION(v)) {
          buf_addc(b, 5);
          if (!w_function(vm, AS_FUNCTION(v), b, depth + 1)) return false;
        } else {
          return vm_error(vm, "Constant of type %s cannot be serialized.", value_type_name(v));
        }
    }
  }
  return true;
}

bool bytecode_write(VM* vm, ObjFunction* fn, Buf* out) {
  char err[200];
  if (!chunk_verify(fn, err, sizeof err)) return vm_error(vm, "Invalid bytecode: %s", err);
  buf_add(out, MAGIC, 4);
  buf_addc(out, EXBIN_BYTECODE_VERSION);
  buf_addc(out, 0);
  buf_addc(out, 0);
  buf_addc(out, 0);
  return w_function(vm, fn, out, 0);
}

/* ---------------------------------------------------------------- reader -- */
typedef struct {
  const uint8_t* d;
  size_t len, pos;
  const char* err;
} Reader;

#define RFAIL(r, msg)     \
  do {                    \
    if (!(r)->err) (r)->err = (msg); \
    return 0;             \
  } while (0)

static int r_u8(Reader* r) {
  if (r->pos >= r->len) RFAIL(r, "unexpected end of data");
  return r->d[r->pos++];
}
static uint64_t r_varint(Reader* r) {
  uint64_t v = 0;
  for (int shift = 0; shift < 70; shift += 7) {
    if (r->pos >= r->len) RFAIL(r, "unexpected end of data");
    uint8_t b = r->d[r->pos++];
    if (shift == 63 && (b & 0x7e)) RFAIL(r, "varint overflow");
    v |= (uint64_t)(b & 0x7f) << shift;
    if (!(b & 0x80)) return v;
  }
  RFAIL(r, "varint too long");
}
static const uint8_t* r_bytes(Reader* r, uint64_t n) {
  if (n > r->len - r->pos) {
    if (!r->err) r->err = "unexpected end of data";
    return NULL;
  }
  const uint8_t* p = r->d + r->pos;
  r->pos += (size_t)n;
  return p;
}

#define MAX_CODE (1u << 24)
#define MAX_STR (1u << 28)

static ObjFunction* r_function(VM* vm, Reader* r, int depth) {
  if (depth > 100) {
    r->err = "functions nested too deeply";
    return NULL;
  }
  ObjFunction* fn = fn_new(vm);
  /* from here on, fn is only reachable from C, but no GC runs while reading */
  if (r_u8(r) == 1) {
    uint64_t n = r_varint(r);
    if (n > MAX_STR) {
      r->err = "string too long";
      return NULL;
    }
    const uint8_t* p = r_bytes(r, n);
    if (!p && n) return NULL;
    fn->name = str_copy(vm, (const char*)p, (size_t)n);
  }
  if (r_u8(r) == 1) {
    uint64_t n = r_varint(r);
    if (n > MAX_STR) {
      r->err = "string too long";
      return NULL;
    }
    const uint8_t* p = r_bytes(r, n);
    if (!p && n) return NULL;
    fn->file = str_copy(vm, (const char*)p, (size_t)n);
  }
  uint64_t arity = r_varint(r), ups = r_varint(r), codeLen = r_varint(r);
  if (r->err) return NULL;
  if (arity > 255 || ups > 255 || codeLen == 0 || codeLen > MAX_CODE) {
    r->err = "bad function header";
    return NULL;
  }
  fn->arity = (int)arity;
  fn->upvalueCount = (int)ups;
  const uint8_t* code = r_bytes(r, codeLen);
  if (!code) return NULL;
  uint8_t* codeCopy = (uint8_t*)malloc((size_t)codeLen);
  memcpy(codeCopy, code, (size_t)codeLen);
  uint64_t runs = r_varint(r);
  if (r->err || runs == 0 || runs > codeLen) {
    free(codeCopy);
    if (!r->err) r->err = "bad line table";
    return NULL;
  }
  uint64_t pos = 0;
  for (uint64_t i = 0; i < runs; i++) {
    uint64_t n = r_varint(r), line = r_varint(r);
    if (r->err || n == 0 || n > codeLen - pos || line > 0x7fffffff) {
      free(codeCopy);
      if (!r->err) r->err = "bad line table";
      return NULL;
    }
    for (uint64_t k = 0; k < n; k++) chunk_write(vm, &fn->chunk, codeCopy[pos + k], (int)line);
    pos += n;
  }
  free(codeCopy);
  if (pos != codeLen) {
    r->err = "line table does not cover the code";
    return NULL;
  }
  uint64_t nc = r_varint(r);
  if (r->err || nc > 65536) {
    if (!r->err) r->err = "too many constants";
    return NULL;
  }
  for (uint64_t i = 0; i < nc; i++) {
    int tag = r_u8(r);
    if (r->err) return NULL;
    Value v = NIL_VAL;
    switch (tag) {
      case 0: break;
      case 1: v = BOOL_VAL(false); break;
      case 2: v = BOOL_VAL(true); break;
      case 3: {
        const uint8_t* p = r_bytes(r, 8);
        if (!p) return NULL;
        uint64_t bits = 0;
        for (int k = 0; k < 8; k++) bits |= (uint64_t)p[k] << (8 * k);
        double d;
        memcpy(&d, &bits, 8);
        v = NUM_VAL(d);
        break;
      }
      case 4: {
        uint64_t n = r_varint(r);
        if (r->err || n > MAX_STR) {
          if (!r->err) r->err = "string too long";
          return NULL;
        }
        const uint8_t* p = r_bytes(r, n);
        if (!p && n) return NULL;
        v = OBJ_VAL(str_copy(vm, (const char*)p, (size_t)n));
        break;
      }
      case 5: {
        ObjFunction* sub = r_function(vm, r, depth + 1);
        if (!sub) return NULL;
        v = OBJ_VAL(sub);
        break;
      }
      default: r->err = "unknown constant tag"; return NULL;
    }
    /* bypass de-duplication so indices stay identical to the file */
    ValueArray* a = &fn->chunk.consts;
    if (a->count + 1 > a->cap) {
      int ncap = a->cap < 8 ? 8 : a->cap * 2;
      a->v = MEM_GROW(vm, Value, a->v, a->cap, ncap);
      a->cap = ncap;
    }
    a->v[a->count++] = v;
  }
  return fn;
}

ObjFunction* bytecode_read(VM* vm, const uint8_t* data, size_t len) {
  Reader r = {data, len, 0, NULL};
  if (len < 8 || memcmp(data, MAGIC, 4) != 0) {
    vm_set_errmsg(vm, "Not an EXBIN bytecode file (bad magic).");
    return NULL;
  }
  if (data[4] != EXBIN_BYTECODE_VERSION) {
    char msg[96];
    snprintf(msg, sizeof msg, "Unsupported bytecode version %d (this VM runs version %d).", data[4],
             EXBIN_BYTECODE_VERSION);
    vm_set_errmsg(vm, msg);
    return NULL;
  }
  r.pos = 8;
  ObjFunction* fn = r_function(vm, &r, 0);
  if (!fn || r.err) {
    char msg[160];
    snprintf(msg, sizeof msg, "Corrupt bytecode: %s.", r.err ? r.err : "unknown error");
    vm_set_errmsg(vm, msg);
    return NULL;
  }
  if (r.pos != r.len) {
    vm_set_errmsg(vm, "Corrupt bytecode: trailing data.");
    return NULL;
  }
  char err[200];
  if (!chunk_verify(fn, err, sizeof err)) {
    char msg[260];
    snprintf(msg, sizeof msg, "Bytecode verification failed: %s.", err);
    vm_set_errmsg(vm, msg);
    return NULL;
  }
  return fn;
}
