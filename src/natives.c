/* EXBIN: standard library — core natives, strings, lists, maps, math, I/O,
 * error handling and require(). Every function registered with def_both() is
 * available both as `name(x, ...)` and as the method `x.name(...)`. */
#include <ctype.h>
#include <time.h>
#if defined(__unix__) || defined(__APPLE__)
#include <unistd.h>
#endif

#include "vm.h"

#define NATIVE(n) static int n(VM* vm, int argc, const Value* args, Value* ret, void* ud)
#define UNUSED_ALL (void)vm, (void)argc, (void)args, (void)ret, (void)ud

static int bad_arg(VM* vm, const char* fn, int i, const char* want, Value got) {
  vm_error(vm, "%s: argument %d must be %s, got %s.", fn, i + 1, want, value_type_name(got));
  return 0;
}
#define REQ_NUM(fn, i, var)                                   \
  if (!IS_NUM(args[i])) return bad_arg(vm, fn, i, "a number", args[i]); \
  double var = AS_NUM(args[i])
#define REQ_STR(fn, i, var)                                   \
  if (!IS_STRING(args[i])) return bad_arg(vm, fn, i, "a string", args[i]); \
  ObjString* var = AS_STRING(args[i])
#define REQ_LIST(fn, i, var)                                  \
  if (!IS_LIST(args[i])) return bad_arg(vm, fn, i, "a list", args[i]); \
  ObjList* var = AS_LIST(args[i])
#define REQ_MAP(fn, i, var)                                   \
  if (!IS_MAP(args[i])) return bad_arg(vm, fn, i, "a map", args[i]); \
  ObjMap* var = AS_MAP(args[i])
#define REQ_FN(fn, i)                                         \
  if (!IS_CALLABLE(args[i])) return bad_arg(vm, fn, i, "a function", args[i])
#define REQ_INT(fn, i, var)                                   \
  if (!IS_NUM(args[i]) || AS_NUM(args[i]) != floor(AS_NUM(args[i]))) \
    return bad_arg(vm, fn, i, "an integer", args[i]);         \
  long long var = (long long)AS_NUM(args[i])

#define RET(v) \
  do {         \
    *ret = (v);\
    return 1;  \
  } while (0)

/* ----------------------------------------------------------- registering -- */
void natives_def(VM* vm, const char* name, ExNative fn, int minArgs, int maxArgs) {
  vm_define_global(vm, name, OBJ_VAL(native_new(vm, name, fn, NULL, minArgs, maxArgs, NULL)));
}

void natives_def_method(VM* vm, const char* type, const char* name, ExNative fn, int minArgs, int maxArgs) {
  /* build a probe value to find the right table */
  ObjMap* table = NULL;
  if (strcmp(type, "number") == 0) table = methods_for(vm, NUM_VAL(0), true);
  else if (strcmp(type, "bool") == 0) table = methods_for(vm, BOOL_VAL(false), true);
  else if (strcmp(type, "nil") == 0) table = methods_for(vm, NIL_VAL, true);
  else if (strcmp(type, "string") == 0) table = methods_for(vm, OBJ_VAL(str_from(vm, "")), true);
  else if (strcmp(type, "list") == 0) table = methods_for(vm, OBJ_VAL(list_new(vm)), true);
  else if (strcmp(type, "map") == 0) table = methods_for(vm, OBJ_VAL(map_new(vm)), true);
  else if (strcmp(type, "bytes") == 0) table = methods_for(vm, OBJ_VAL(bytes_new(vm, 0)), true);
  else if (strcmp(type, "range") == 0) table = methods_for(vm, OBJ_VAL(range_new(vm, 0, 0, 1)), true);
  else if (strcmp(type, "function") == 0) table = methods_for(vm, OBJ_VAL(fn_new(vm)), true);
  else {
    /* handle kind: kinds are interned by the toolkit as static strings */
    static const char* kinds[] = {"regex", "lexer", "asm", NULL};
    const char* kind = NULL;
    for (int i = 0; kinds[i]; i++)
      if (strcmp(kinds[i], type) == 0) kind = kinds[i];
    if (!kind) return;
    ObjHandle* h = handle_new(vm, kind, NULL, NULL, NULL);
    table = methods_for(vm, OBJ_VAL(h), true);
  }
  int mn = minArgs + 1, mx = maxArgs < 0 ? -1 : maxArgs + 1;
  map_set_str(vm, table, name, OBJ_VAL(native_new(vm, name, fn, NULL, mn, mx, NULL)));
}

/* min/max count ALL arguments, including the first one */
static void def_both(VM* vm, const char* name, ExNative fn, int min, int max, const char* types) {
  natives_def(vm, name, fn, min, max);
  char buf[96];
  snprintf(buf, sizeof buf, "%s", types);
  for (char* t = strtok(buf, ","); t; t = strtok(NULL, ","))
    natives_def_method(vm, t, name, fn, min - 1, max < 0 ? -1 : max - 1);
}

/* ------------------------------------------------------------------ utf8 -- */
static void utf8_enc(Buf* b, unsigned long cp) {
  if (cp < 0x80) buf_addc(b, (char)cp);
  else if (cp < 0x800) {
    buf_addc(b, (char)(0xC0 | (cp >> 6)));
    buf_addc(b, (char)(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    buf_addc(b, (char)(0xE0 | (cp >> 12)));
    buf_addc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
    buf_addc(b, (char)(0x80 | (cp & 0x3F)));
  } else {
    buf_addc(b, (char)(0xF0 | (cp >> 18)));
    buf_addc(b, (char)(0x80 | ((cp >> 12) & 0x3F)));
    buf_addc(b, (char)(0x80 | ((cp >> 6) & 0x3F)));
    buf_addc(b, (char)(0x80 | (cp & 0x3F)));
  }
}
static unsigned long utf8_dec(const unsigned char* s, int remaining, int* len) {
  unsigned char c = s[0];
  int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
  if (n > remaining) n = 1;
  unsigned long cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
  for (int i = 1; i < n; i++) {
    if ((s[i] & 0xC0) != 0x80) {
      n = 1;
      cp = c;
      break;
    }
    cp = (cp << 6) | (s[i] & 0x3F);
  }
  *len = n;
  return cp;
}

static ObjString* mkstr(VM* vm, const char* s, size_t n) { return str_copy(vm, s, n); }

/* ----------------------------------------------------------------- basic -- */
static void print_values(VM* vm, int argc, const Value* args, bool nl) {
  Buf b;
  buf_init(&b);
  for (int i = 0; i < argc; i++) {
    if (i) buf_addc(&b, ' ');
    value_tostr_buf(vm, &b, args[i]);
  }
  if (nl) buf_addc(&b, '\n');
  vm_write(vm, b.d ? b.d : "", b.len);
  buf_free(&b);
}
NATIVE(n_print) {
  (void)ret, (void)ud;
  print_values(vm, argc, args, true);
  *ret = NIL_VAL;
  return 1;
}
NATIVE(n_write) {
  (void)ud;
  print_values(vm, argc, args, false);
  *ret = NIL_VAL;
  return 1;
}

static int value_len(Value v, long long* out) {
  if (!IS_OBJ(v)) return 0;
  switch (AS_OBJ(v)->type) {
    case OBJ_STRING: *out = AS_STRING(v)->len; return 1;
    case OBJ_LIST: *out = AS_LIST(v)->count; return 1;
    case OBJ_MAP: *out = AS_MAP(v)->count; return 1;
    case OBJ_BYTES: *out = AS_BYTES(v)->len; return 1;
    case OBJ_RANGE: *out = (long long)range_len(AS_RANGE(v)); return 1;
    default: return 0;
  }
}
NATIVE(n_len) {
  (void)argc, (void)ud;
  long long n;
  if (!value_len(args[0], &n)) return bad_arg(vm, "len", 0, "a string, list, map, bytes or range", args[0]);
  RET(NUM_VAL((double)n));
}
NATIVE(n_type) {
  UNUSED_ALL;
  RET(OBJ_VAL(str_from(vm, value_type_name(args[0]))));
}
NATIVE(n_str) {
  UNUSED_ALL;
  RET(OBJ_VAL(value_tostring(vm, args[0])));
}
NATIVE(n_repr) {
  UNUSED_ALL;
  RET(OBJ_VAL(value_repr(vm, args[0])));
}

static bool parse_number(const char* s, int len, double* out) {
  char tmp[128];
  if (len <= 0 || len >= (int)sizeof tmp) return false;
  int n = 0;
  for (int i = 0; i < len; i++)
    if (s[i] != '_') tmp[n++] = s[i];
  tmp[n] = 0;
  char* p = tmp;
  while (isspace((unsigned char)*p)) p++;
  if (!*p) return false;
  char* end;
  if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) *out = (double)strtoull(p + 2, &end, 16);
  else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) *out = (double)strtoull(p + 2, &end, 2);
  else *out = strtod(p, &end);
  while (isspace((unsigned char)*end)) end++;
  return *end == 0 && end != p;
}
NATIVE(n_num) {
  UNUSED_ALL;
  Value v = args[0];
  double d;
  if (IS_NUM(v)) RET(v);
  if (IS_BOOL(v)) RET(NUM_VAL(AS_BOOL(v)));
  if (IS_STRING(v) && parse_number(AS_CSTRING(v), AS_STRING(v)->len, &d)) RET(NUM_VAL(d));
  RET(NIL_VAL);
}
NATIVE(n_int) {
  UNUSED_ALL;
  Value v = args[0];
  double d;
  if (IS_NUM(v)) RET(NUM_VAL(trunc(AS_NUM(v))));
  if (IS_STRING(v) && parse_number(AS_CSTRING(v), AS_STRING(v)->len, &d)) RET(NUM_VAL(trunc(d)));
  RET(NIL_VAL);
}

/* ------------------------------------------------------------ containers -- */
NATIVE(n_push) {
  (void)ud;
  if (IS_LIST(args[0])) {
    for (int i = 1; i < argc; i++) list_push(vm, AS_LIST(args[0]), args[i]);
  } else if (IS_BYTES(args[0])) {
    for (int i = 1; i < argc; i++) {
      if (!IS_NUM(args[i]) || AS_NUM(args[i]) < 0 || AS_NUM(args[i]) > 255 || AS_NUM(args[i]) != floor(AS_NUM(args[i])))
        return vm_error(vm, "push: bytes elements must be integers 0..255.");
      bytes_push(vm, AS_BYTES(args[0]), (uint8_t)AS_NUM(args[i]));
    }
  } else {
    return bad_arg(vm, "push", 0, "a list or bytes", args[0]);
  }
  RET(args[0]);
}
NATIVE(n_pop) {
  (void)ud;
  if (IS_LIST(args[0])) {
    ObjList* l = AS_LIST(args[0]);
    if (l->count == 0) RET(NIL_VAL);
    int idx = l->count - 1;
    if (argc > 1) {
      REQ_INT("pop", 1, i);
      if (i < 0) i += l->count;
      if (i < 0 || i >= l->count) return vm_error(vm, "pop: index out of range.");
      idx = (int)i;
    }
    RET(list_remove(l, idx));
  }
  if (IS_BYTES(args[0])) {
    ObjBytes* b = AS_BYTES(args[0]);
    if (b->len == 0) RET(NIL_VAL);
    RET(NUM_VAL(b->data[--b->len]));
  }
  return bad_arg(vm, "pop", 0, "a list or bytes", args[0]);
}
NATIVE(n_insert) {
  UNUSED_ALL;
  REQ_LIST("insert", 0, l);
  REQ_INT("insert", 1, i);
  if (i < 0) i += l->count;
  if (i < 0 || i > l->count) return vm_error(vm, "insert: index out of range.");
  list_insert(vm, l, (int)i, args[2]);
  RET(args[0]);
}
NATIVE(n_remove) {
  UNUSED_ALL;
  REQ_LIST("remove", 0, l);
  REQ_INT("remove", 1, i);
  if (i < 0) i += l->count;
  if (i < 0 || i >= l->count) return vm_error(vm, "remove: index out of range.");
  RET(list_remove(l, (int)i));
}
NATIVE(n_delete) {
  UNUSED_ALL;
  REQ_MAP("delete", 0, m);
  RET(BOOL_VAL(map_delete(m, args[1])));
}
NATIVE(n_keys) {
  UNUSED_ALL;
  REQ_MAP("keys", 0, m);
  ObjList* l = list_new(vm);
  for (int i = 0; i < m->used; i++)
    if (!IS_NIL(m->entries[i].key)) list_push(vm, l, m->entries[i].key);
  RET(OBJ_VAL(l));
}
NATIVE(n_values) {
  UNUSED_ALL;
  REQ_MAP("values", 0, m);
  ObjList* l = list_new(vm);
  for (int i = 0; i < m->used; i++)
    if (!IS_NIL(m->entries[i].key)) list_push(vm, l, m->entries[i].value);
  RET(OBJ_VAL(l));
}
NATIVE(n_items) {
  UNUSED_ALL;
  REQ_MAP("items", 0, m);
  ObjList* l = list_new(vm);
  for (int i = 0; i < m->used; i++) {
    if (IS_NIL(m->entries[i].key)) continue;
    ObjList* pair = list_new(vm);
    list_push(vm, pair, m->entries[i].key);
    list_push(vm, pair, m->entries[i].value);
    list_push(vm, l, OBJ_VAL(pair));
  }
  RET(OBJ_VAL(l));
}

static int find_sub(const char* h, int hl, const char* n, int nl, int from) {
  if (nl == 0) return from <= hl ? from : -1;
  for (int i = from; i + nl <= hl; i++)
    if (h[i] == n[0] && memcmp(h + i, n, (size_t)nl) == 0) return i;
  return -1;
}

NATIVE(n_has) {
  UNUSED_ALL;
  Value c = args[0], v = args[1];
  if (IS_MAP(c)) RET(BOOL_VAL(map_get(AS_MAP(c), v, NULL)));
  if (IS_LIST(c)) {
    ObjList* l = AS_LIST(c);
    for (int i = 0; i < l->count; i++)
      if (deep_equal(l->items[i], v, 0)) RET(BOOL_VAL(true));
    RET(BOOL_VAL(false));
  }
  if (IS_STRING(c)) {
    if (!IS_STRING(v)) return bad_arg(vm, "has", 1, "a string", v);
    RET(BOOL_VAL(find_sub(AS_CSTRING(c), AS_STRING(c)->len, AS_CSTRING(v), AS_STRING(v)->len, 0) >= 0));
  }
  if (IS_BYTES(c)) {
    if (!IS_NUM(v)) RET(BOOL_VAL(false));
    ObjBytes* b = AS_BYTES(c);
    for (int i = 0; i < b->len; i++)
      if (b->data[i] == (uint8_t)AS_NUM(v) && AS_NUM(v) >= 0 && AS_NUM(v) < 256) RET(BOOL_VAL(true));
    RET(BOOL_VAL(false));
  }
  return bad_arg(vm, "has", 0, "a map, list, string or bytes", c);
}

NATIVE(n_find) { /* find(string|list, needle, start=0) -> index or -1 */
  UNUSED_ALL;
  long long from = 0;
  if (argc > 2) {
    REQ_INT("find", 2, f);
    from = f;
  }
  if (IS_STRING(args[0])) {
    REQ_STR("find", 1, n);
    ObjString* h = AS_STRING(args[0]);
    if (from < 0) from += h->len;
    if (from < 0) from = 0;
    if (from > h->len) RET(NUM_VAL(-1));
    RET(NUM_VAL(find_sub(h->chars, h->len, n->chars, n->len, (int)from)));
  }
  if (IS_LIST(args[0])) {
    ObjList* l = AS_LIST(args[0]);
    if (from < 0) from += l->count;
    if (from < 0) from = 0;
    for (int i = (int)from; i < l->count; i++)
      if (deep_equal(l->items[i], args[1], 0)) RET(NUM_VAL(i));
    RET(NUM_VAL(-1));
  }
  return bad_arg(vm, "find", 0, "a string or list", args[0]);
}

NATIVE(n_copy) {
  UNUSED_ALL;
  Value v = args[0];
  if (IS_LIST(v)) {
    ObjList* l = AS_LIST(v);
    ObjList* r = list_new(vm);
    for (int i = 0; i < l->count; i++) list_push(vm, r, l->items[i]);
    RET(OBJ_VAL(r));
  }
  if (IS_MAP(v)) {
    ObjMap* m = AS_MAP(v);
    ObjMap* r = map_new(vm);
    for (int i = 0; i < m->used; i++)
      if (!IS_NIL(m->entries[i].key)) map_set(vm, r, m->entries[i].key, m->entries[i].value);
    RET(OBJ_VAL(r));
  }
  if (IS_BYTES(v)) {
    ObjBytes* b = AS_BYTES(v);
    ObjBytes* r = bytes_new(vm, b->len);
    if (b->len) memcpy(r->data, b->data, (size_t)b->len);
    RET(OBJ_VAL(r));
  }
  RET(v);
}

NATIVE(n_slice) {
  UNUSED_ALL;
  long long n;
  if (!value_len(args[0], &n) || IS_MAP(args[0]) || IS_RANGE(args[0])) return bad_arg(vm, "slice", 0, "a string, list or bytes", args[0]);
  REQ_INT("slice", 1, a);
  long long b = n;
  if (argc > 2 && !IS_NIL(args[2])) {
    REQ_INT("slice", 2, e);
    b = e;
  }
  if (a < 0) a += n;
  if (b < 0) b += n;
  if (a < 0) a = 0;
  if (b > n) b = n;
  if (a > b) a = b;
  Value v = args[0];
  if (IS_STRING(v)) RET(OBJ_VAL(mkstr(vm, AS_CSTRING(v) + a, (size_t)(b - a))));
  if (IS_LIST(v)) {
    ObjList* r = list_new(vm);
    for (long long i = a; i < b; i++) list_push(vm, r, AS_LIST(v)->items[i]);
    RET(OBJ_VAL(r));
  }
  ObjBytes* r = bytes_new(vm, (int)(b - a));
  if (b > a) memcpy(r->data, AS_BYTES(v)->data + a, (size_t)(b - a));
  RET(OBJ_VAL(r));
}

NATIVE(n_join) {
  UNUSED_ALL;
  REQ_LIST("join", 0, l);
  ObjString* sep = NULL;
  if (argc > 1) {
    REQ_STR("join", 1, s);
    sep = s;
  }
  Buf b;
  buf_init(&b);
  for (int i = 0; i < l->count; i++) {
    if (i && sep) buf_add(&b, sep->chars, (size_t)sep->len);
    value_tostr_buf(vm, &b, l->items[i]);
  }
  ObjString* r = mkstr(vm, b.d ? b.d : "", b.len);
  buf_free(&b);
  RET(OBJ_VAL(r));
}

static ObjList* split_chars(VM* vm, ObjString* s) {
  ObjList* l = list_new(vm);
  for (int i = 0; i < s->len;) {
    int n;
    utf8_dec((const unsigned char*)s->chars + i, s->len - i, &n);
    list_push(vm, l, OBJ_VAL(mkstr(vm, s->chars + i, (size_t)n)));
    i += n;
  }
  return l;
}
NATIVE(n_chars) {
  UNUSED_ALL;
  REQ_STR("chars", 0, s);
  RET(OBJ_VAL(split_chars(vm, s)));
}
NATIVE(n_split) {
  UNUSED_ALL;
  REQ_STR("split", 0, s);
  if (argc < 2) RET(OBJ_VAL(split_chars(vm, s)));
  REQ_STR("split", 1, sep);
  if (sep->len == 0) RET(OBJ_VAL(split_chars(vm, s)));
  ObjList* l = list_new(vm);
  int start = 0;
  for (;;) {
    int i = find_sub(s->chars, s->len, sep->chars, sep->len, start);
    if (i < 0) {
      list_push(vm, l, OBJ_VAL(mkstr(vm, s->chars + start, (size_t)(s->len - start))));
      break;
    }
    list_push(vm, l, OBJ_VAL(mkstr(vm, s->chars + start, (size_t)(i - start))));
    start = i + sep->len;
  }
  RET(OBJ_VAL(l));
}
NATIVE(n_lines) {
  UNUSED_ALL;
  REQ_STR("lines", 0, s);
  ObjList* l = list_new(vm);
  int start = 0;
  for (int i = 0; i <= s->len; i++) {
    if (i == s->len || s->chars[i] == '\n') {
      if (i == s->len && start == i) break;
      int e = i;
      if (e > start && s->chars[e - 1] == '\r') e--;
      list_push(vm, l, OBJ_VAL(mkstr(vm, s->chars + start, (size_t)(e - start))));
      start = i + 1;
    }
  }
  RET(OBJ_VAL(l));
}
NATIVE(n_replace) {
  UNUSED_ALL;
  REQ_STR("replace", 0, s);
  REQ_STR("replace", 1, a);
  REQ_STR("replace", 2, b);
  long long limit = -1;
  if (argc > 3) {
    REQ_INT("replace", 3, lim);
    limit = lim;
  }
  if (a->len == 0) RET(args[0]);
  Buf out;
  buf_init(&out);
  int start = 0;
  while (limit != 0) {
    int i = find_sub(s->chars, s->len, a->chars, a->len, start);
    if (i < 0) break;
    buf_add(&out, s->chars + start, (size_t)(i - start));
    buf_add(&out, b->chars, (size_t)b->len);
    start = i + a->len;
    if (limit > 0) limit--;
  }
  buf_add(&out, s->chars + start, (size_t)(s->len - start));
  ObjString* r = mkstr(vm, out.d ? out.d : "", out.len);
  buf_free(&out);
  RET(OBJ_VAL(r));
}
NATIVE(n_upper) {
  UNUSED_ALL;
  REQ_STR("upper", 0, s);
  ObjString* r = mkstr(vm, s->chars, (size_t)s->len);
  for (int i = 0; i < r->len; i++) r->chars[i] = (char)toupper((unsigned char)r->chars[i]);
  r->hash = hash_bytes(r->chars, (size_t)r->len);
  RET(OBJ_VAL(r));
}
NATIVE(n_lower) {
  UNUSED_ALL;
  REQ_STR("lower", 0, s);
  ObjString* r = mkstr(vm, s->chars, (size_t)s->len);
  for (int i = 0; i < r->len; i++) r->chars[i] = (char)tolower((unsigned char)r->chars[i]);
  r->hash = hash_bytes(r->chars, (size_t)r->len);
  RET(OBJ_VAL(r));
}
static int trim_impl(VM* vm, Value v, bool left, bool right, Value* ret, const char* fn) {
  if (!IS_STRING(v)) return bad_arg(vm, fn, 0, "a string", v);
  ObjString* s = AS_STRING(v);
  int a = 0, b = s->len;
  if (left)
    while (a < b && isspace((unsigned char)s->chars[a])) a++;
  if (right)
    while (b > a && isspace((unsigned char)s->chars[b - 1])) b--;
  *ret = OBJ_VAL(mkstr(vm, s->chars + a, (size_t)(b - a)));
  return 1;
}
NATIVE(n_trim) {
  (void)argc, (void)ud;
  return trim_impl(vm, args[0], true, true, ret, "trim");
}
NATIVE(n_trim_start) {
  (void)argc, (void)ud;
  return trim_impl(vm, args[0], true, false, ret, "trim_start");
}
NATIVE(n_trim_end) {
  (void)argc, (void)ud;
  return trim_impl(vm, args[0], false, true, ret, "trim_end");
}
NATIVE(n_starts_with) {
  UNUSED_ALL;
  REQ_STR("starts_with", 0, s);
  REQ_STR("starts_with", 1, p);
  RET(BOOL_VAL(p->len <= s->len && memcmp(s->chars, p->chars, (size_t)p->len) == 0));
}
NATIVE(n_ends_with) {
  UNUSED_ALL;
  REQ_STR("ends_with", 0, s);
  REQ_STR("ends_with", 1, p);
  RET(BOOL_VAL(p->len <= s->len && memcmp(s->chars + s->len - p->len, p->chars, (size_t)p->len) == 0));
}
NATIVE(n_repeat) {
  UNUSED_ALL;
  REQ_STR("repeat", 0, s);
  REQ_INT("repeat", 1, n);
  if (n < 0 || (double)n * s->len > 1e9) return vm_error(vm, "repeat: bad count.");
  Buf b;
  buf_init(&b);
  for (long long i = 0; i < n; i++) buf_add(&b, s->chars, (size_t)s->len);
  ObjString* r = mkstr(vm, b.d ? b.d : "", b.len);
  buf_free(&b);
  RET(OBJ_VAL(r));
}
static int pad_impl(VM* vm, int argc, const Value* args, Value* ret, bool left, const char* fn) {
  if (!IS_STRING(args[0])) return bad_arg(vm, fn, 0, "a string", args[0]);
  if (!IS_NUM(args[1])) return bad_arg(vm, fn, 1, "a number", args[1]);
  ObjString* s = AS_STRING(args[0]);
  int width = (int)AS_NUM(args[1]);
  const char* pad = " ";
  int padlen = 1;
  if (argc > 2) {
    if (!IS_STRING(args[2]) || AS_STRING(args[2])->len == 0) return bad_arg(vm, fn, 2, "a non-empty string", args[2]);
    pad = AS_CSTRING(args[2]);
    padlen = AS_STRING(args[2])->len;
  }
  /* width counts UTF-8 characters */
  int chars = 0;
  for (int i = 0; i < s->len; i++)
    if (((unsigned char)s->chars[i] & 0xC0) != 0x80) chars++;
  Buf b;
  buf_init(&b);
  if (!left) buf_add(&b, s->chars, (size_t)s->len);
  for (int i = chars; i < width && i < 1000000; i++) buf_add(&b, pad, (size_t)padlen);
  if (left) buf_add(&b, s->chars, (size_t)s->len);
  *ret = OBJ_VAL(mkstr(vm, b.d ? b.d : "", b.len));
  buf_free(&b);
  return 1;
}
NATIVE(n_pad_left) {
  (void)ud;
  return pad_impl(vm, argc, args, ret, true, "pad_left");
}
NATIVE(n_pad_right) {
  (void)ud;
  return pad_impl(vm, argc, args, ret, false, "pad_right");
}
NATIVE(n_substr) { /* substr(s, start, len=rest) */
  UNUSED_ALL;
  REQ_STR("substr", 0, s);
  REQ_INT("substr", 1, a);
  if (a < 0) a += s->len;
  if (a < 0) a = 0;
  if (a > s->len) a = s->len;
  long long n = s->len - a;
  if (argc > 2) {
    REQ_INT("substr", 2, l);
    if (l < n) n = l < 0 ? 0 : l;
  }
  RET(OBJ_VAL(mkstr(vm, s->chars + a, (size_t)n)));
}
NATIVE(n_chr) {
  UNUSED_ALL;
  REQ_INT("chr", 0, n);
  if (n < 0 || n > 0x10FFFF) return vm_error(vm, "chr: code point out of range.");
  Buf b;
  buf_init(&b);
  utf8_enc(&b, (unsigned long)n);
  ObjString* r = mkstr(vm, b.d, b.len);
  buf_free(&b);
  RET(OBJ_VAL(r));
}
NATIVE(n_ord) {
  UNUSED_ALL;
  REQ_STR("ord", 0, s);
  if (s->len == 0) return vm_error(vm, "ord: empty string.");
  int n;
  RET(NUM_VAL((double)utf8_dec((const unsigned char*)s->chars, s->len, &n)));
}
NATIVE(n_utf8_len) {
  UNUSED_ALL;
  REQ_STR("utf8_len", 0, s);
  int c = 0;
  for (int i = 0; i < s->len; i++)
    if (((unsigned char)s->chars[i] & 0xC0) != 0x80) c++;
  RET(NUM_VAL(c));
}
NATIVE(n_reverse) {
  UNUSED_ALL;
  if (IS_LIST(args[0])) {
    ObjList* l = AS_LIST(args[0]);
    ObjList* r = list_new(vm);
    for (int i = l->count - 1; i >= 0; i--) list_push(vm, r, l->items[i]);
    RET(OBJ_VAL(r));
  }
  if (IS_STRING(args[0])) {
    ObjString* s = AS_STRING(args[0]);
    Buf b;
    buf_init(&b);
    int end = s->len;
    while (end > 0) {
      int st = end - 1;
      while (st > 0 && ((unsigned char)s->chars[st] & 0xC0) == 0x80) st--;
      buf_add(&b, s->chars + st, (size_t)(end - st));
      end = st;
    }
    ObjString* r = mkstr(vm, b.d ? b.d : "", b.len);
    buf_free(&b);
    RET(OBJ_VAL(r));
  }
  return bad_arg(vm, "reverse", 0, "a list or string", args[0]);
}

NATIVE(n_range) {
  UNUSED_ALL;
  REQ_NUM("range", 0, a);
  double start = 0, stop = a, step = 1;
  if (argc > 1) {
    REQ_NUM("range", 1, b);
    start = a;
    stop = b;
  }
  if (argc > 2) {
    REQ_NUM("range", 2, c);
    step = c;
  }
  if (step == 0) return vm_error(vm, "range: step cannot be zero.");
  RET(OBJ_VAL(range_new(vm, start, stop, step)));
}

NATIVE(n_list) { /* list(iterable) -> new list */
  UNUSED_ALL;
  Value v = args[0];
  ObjList* r = list_new(vm);
  Value keep = OBJ_VAL(r);
  (void)keep;
  if (IS_LIST(v)) {
    for (int i = 0; i < AS_LIST(v)->count; i++) list_push(vm, r, AS_LIST(v)->items[i]);
  } else if (IS_RANGE(v)) {
    int64_t n = range_len(AS_RANGE(v));
    if (n > 50000000) return vm_error(vm, "list: range too large.");
    for (int64_t i = 0; i < n; i++) list_push(vm, r, NUM_VAL(AS_RANGE(v)->start + AS_RANGE(v)->step * (double)i));
  } else if (IS_STRING(v)) {
    RET(OBJ_VAL(split_chars(vm, AS_STRING(v))));
  } else if (IS_BYTES(v)) {
    for (int i = 0; i < AS_BYTES(v)->len; i++) list_push(vm, r, NUM_VAL(AS_BYTES(v)->data[i]));
  } else if (IS_MAP(v)) {
    ObjMap* m = AS_MAP(v);
    for (int i = 0; i < m->used; i++)
      if (!IS_NIL(m->entries[i].key)) list_push(vm, r, m->entries[i].key);
  } else {
    return bad_arg(vm, "list", 0, "iterable", v);
  }
  RET(OBJ_VAL(r));
}

/* ---------------------------------------------- higher order & sorting ---- */
/* Natives that call back into the VM keep their result list rooted on the VM
 * stack, because the GC may run inside the callback. */
NATIVE(n_map) {
  UNUSED_ALL;
  REQ_LIST("map", 0, l);
  REQ_FN("map", 1);
  ObjList* res = list_new(vm);
  vm_push(vm, OBJ_VAL(res));
  for (int i = 0; i < l->count; i++) {
    Value r;
    Value a = l->items[i];
    if (!vm_call(vm, args[1], 1, &a, &r)) return 0;
    list_push(vm, res, r);
  }
  vm_pop(vm);
  RET(OBJ_VAL(res));
}
NATIVE(n_filter) {
  UNUSED_ALL;
  REQ_LIST("filter", 0, l);
  REQ_FN("filter", 1);
  ObjList* res = list_new(vm);
  vm_push(vm, OBJ_VAL(res));
  for (int i = 0; i < l->count; i++) {
    Value r, a = l->items[i];
    if (!vm_call(vm, args[1], 1, &a, &r)) return 0;
    if (value_truthy(r)) list_push(vm, res, a);
  }
  vm_pop(vm);
  RET(OBJ_VAL(res));
}
NATIVE(n_reduce) {
  UNUSED_ALL;
  REQ_LIST("reduce", 0, l);
  REQ_FN("reduce", 1);
  int i = 0;
  Value acc;
  if (argc > 2) acc = args[2];
  else {
    if (l->count == 0) return vm_error(vm, "reduce: empty list and no initial value.");
    acc = l->items[0];
    i = 1;
  }
  vm_push(vm, acc); /* keep the accumulator rooted */
  for (; i < l->count; i++) {
    Value pair[2] = {vm->stackTop[-1], l->items[i]};
    Value r;
    if (!vm_call(vm, args[1], 2, pair, &r)) return 0;
    vm->stackTop[-1] = r;
  }
  acc = vm_pop(vm);
  RET(acc);
}
NATIVE(n_each) {
  UNUSED_ALL;
  REQ_LIST("each", 0, l);
  REQ_FN("each", 1);
  for (int i = 0; i < l->count; i++) {
    Value r, a = l->items[i];
    if (!vm_call(vm, args[1], 1, &a, &r)) return 0;
  }
  RET(args[0]);
}
NATIVE(n_any) {
  UNUSED_ALL;
  REQ_LIST("any", 0, l);
  for (int i = 0; i < l->count; i++) {
    Value r, a = l->items[i];
    if (argc > 1) {
      if (!vm_call(vm, args[1], 1, &a, &r)) return 0;
    } else r = a;
    if (value_truthy(r)) RET(BOOL_VAL(true));
  }
  RET(BOOL_VAL(false));
}
NATIVE(n_all) {
  UNUSED_ALL;
  REQ_LIST("all", 0, l);
  for (int i = 0; i < l->count; i++) {
    Value r, a = l->items[i];
    if (argc > 1) {
      if (!vm_call(vm, args[1], 1, &a, &r)) return 0;
    } else r = a;
    if (!value_truthy(r)) RET(BOOL_VAL(false));
  }
  RET(BOOL_VAL(true));
}
NATIVE(n_sum) {
  UNUSED_ALL;
  REQ_LIST("sum", 0, l);
  double s = 0;
  for (int i = 0; i < l->count; i++) {
    if (!IS_NUM(l->items[i])) return vm_error(vm, "sum: element %d is not a number.", i);
    s += AS_NUM(l->items[i]);
  }
  RET(NUM_VAL(s));
}
NATIVE(n_zip) {
  UNUSED_ALL;
  REQ_LIST("zip", 0, a);
  REQ_LIST("zip", 1, b);
  ObjList* res = list_new(vm);
  int n = a->count < b->count ? a->count : b->count;
  for (int i = 0; i < n; i++) {
    ObjList* p = list_new(vm);
    list_push(vm, p, a->items[i]);
    list_push(vm, p, b->items[i]);
    list_push(vm, res, OBJ_VAL(p));
  }
  RET(OBJ_VAL(res));
}
NATIVE(n_enumerate) {
  UNUSED_ALL;
  REQ_LIST("enumerate", 0, a);
  ObjList* res = list_new(vm);
  for (int i = 0; i < a->count; i++) {
    ObjList* p = list_new(vm);
    list_push(vm, p, NUM_VAL(i));
    list_push(vm, p, a->items[i]);
    list_push(vm, res, OBJ_VAL(p));
  }
  RET(OBJ_VAL(res));
}

static int less_than(VM* vm, Value cmp, bool hasCmp, Value a, Value b, bool* out) {
  if (hasCmp) {
    Value pair[2] = {a, b}, r;
    if (!vm_call(vm, cmp, 2, pair, &r)) return 0;
    if (IS_NUM(r)) *out = AS_NUM(r) < 0;
    else *out = value_truthy(r);
    return 1;
  }
  if (IS_NUM(a) && IS_NUM(b)) {
    *out = AS_NUM(a) < AS_NUM(b);
    return 1;
  }
  if (IS_STRING(a) && IS_STRING(b)) {
    ObjString *x = AS_STRING(a), *y = AS_STRING(b);
    int n = x->len < y->len ? x->len : y->len;
    int c = memcmp(x->chars, y->chars, (size_t)n);
    *out = c < 0 || (c == 0 && x->len < y->len);
    return 1;
  }
  vm_error(vm, "sort: cannot compare %s and %s (pass a comparison function).", value_type_name(a), value_type_name(b));
  return 0;
}

NATIVE(n_sort) { /* in place, stable merge sort; elements stay rooted in the list */
  UNUSED_ALL;
  REQ_LIST("sort", 0, l);
  bool hasCmp = argc > 1 && !IS_NIL(args[1]);
  if (hasCmp) REQ_FN("sort", 1);
  Value cmp = hasCmp ? args[1] : NIL_VAL;
  int n = l->count;
  if (n < 2) RET(args[0]);
  Value* src = (Value*)malloc(sizeof(Value) * (size_t)n);
  Value* dst = (Value*)malloc(sizeof(Value) * (size_t)n);
  memcpy(src, l->items, sizeof(Value) * (size_t)n);
  /* l->items is never modified until the end, so every value stays reachable */
  for (int w = 1; w < n; w *= 2) {
    for (int lo = 0; lo < n; lo += 2 * w) {
      int mid = lo + w < n ? lo + w : n, hi = lo + 2 * w < n ? lo + 2 * w : n;
      int i = lo, j = mid, k = lo;
      while (i < mid && j < hi) {
        bool lt;
        if (!less_than(vm, cmp, hasCmp, src[j], src[i], &lt)) {
          free(src);
          free(dst);
          return 0;
        }
        dst[k++] = lt ? src[j++] : src[i++];
      }
      while (i < mid) dst[k++] = src[i++];
      while (j < hi) dst[k++] = src[j++];
    }
    Value* t = src;
    src = dst;
    dst = t;
  }
  if (l->count == n) memcpy(l->items, src, sizeof(Value) * (size_t)n);
  free(src);
  free(dst);
  RET(args[0]);
}

/* ------------------------------------------------------------------ math -- */
#define MATH1(name, expr)                    \
  NATIVE(n_##name) {                         \
    UNUSED_ALL;                              \
    REQ_NUM(#name, 0, x);                    \
    RET(NUM_VAL(expr));                      \
  }
MATH1(abs, fabs(x))
MATH1(floor, floor(x))
MATH1(ceil, ceil(x))
MATH1(round, round(x))
MATH1(sqrt, sqrt(x))
MATH1(sin, sin(x))
MATH1(cos, cos(x))
MATH1(tan, tan(x))
MATH1(asin, asin(x))
MATH1(acos, acos(x))
MATH1(atan, atan(x))
MATH1(log, log(x))
MATH1(log2, log2(x))
MATH1(log10, log10(x))
MATH1(exp, exp(x))
MATH1(sign, x > 0 ? 1 : x < 0 ? -1 : 0)
NATIVE(n_atan2) {
  UNUSED_ALL;
  REQ_NUM("atan2", 0, y);
  REQ_NUM("atan2", 1, x);
  RET(NUM_VAL(atan2(y, x)));
}
NATIVE(n_pow) {
  UNUSED_ALL;
  REQ_NUM("pow", 0, x);
  REQ_NUM("pow", 1, y);
  RET(NUM_VAL(pow(x, y)));
}
static int minmax(VM* vm, int argc, const Value* args, Value* ret, bool wantMax, const char* fn) {
  const Value* v = args;
  int n = argc;
  if (argc == 1 && IS_LIST(args[0])) {
    v = AS_LIST(args[0])->items;
    n = AS_LIST(args[0])->count;
    if (n == 0) return vm_error(vm, "%s: empty list.", fn);
  }
  double best = 0;
  for (int i = 0; i < n; i++) {
    if (!IS_NUM(v[i])) return bad_arg(vm, fn, i, "a number", v[i]);
    double d = AS_NUM(v[i]);
    if (i == 0 || (wantMax ? d > best : d < best)) best = d;
  }
  *ret = NUM_VAL(best);
  return 1;
}
NATIVE(n_min) {
  (void)ud;
  return minmax(vm, argc, args, ret, false, "min");
}
NATIVE(n_max) {
  (void)ud;
  return minmax(vm, argc, args, ret, true, "max");
}
NATIVE(n_clamp) {
  UNUSED_ALL;
  REQ_NUM("clamp", 0, x);
  REQ_NUM("clamp", 1, lo);
  REQ_NUM("clamp", 2, hi);
  RET(NUM_VAL(x < lo ? lo : x > hi ? hi : x));
}
static uint64_t rng_next(VM* vm) {
  uint64_t x = vm->rng ? vm->rng : 88172645463325252ULL;
  x ^= x >> 12;
  x ^= x << 25;
  x ^= x >> 27;
  vm->rng = x;
  return x * 2685821657736338717ULL;
}
NATIVE(n_random) {
  UNUSED_ALL;
  double u = (double)(rng_next(vm) >> 11) / 9007199254740992.0;
  if (argc == 0) RET(NUM_VAL(u));
  REQ_NUM("random", 0, a);
  if (argc == 1) RET(NUM_VAL(floor(u * a)));
  REQ_NUM("random", 1, b);
  RET(NUM_VAL(a + floor(u * (b - a + 1))));
}
NATIVE(n_seed) {
  UNUSED_ALL;
  REQ_NUM("seed", 0, s);
  vm->rng = (uint64_t)(int64_t)s * 2654435761ULL + 1;
  RET(NIL_VAL);
}
NATIVE(n_clock) {
  UNUSED_ALL;
  RET(NUM_VAL((double)clock() / CLOCKS_PER_SEC));
}
NATIVE(n_time) {
  UNUSED_ALL;
  RET(NUM_VAL((double)time(NULL)));
}
NATIVE(n_sleep) {
  UNUSED_ALL;
  REQ_NUM("sleep", 0, ms);
#if defined(__unix__) || defined(__APPLE__)
  if (ms > 0) usleep((useconds_t)(ms * 1000));
#endif
  RET(NIL_VAL);
}
static int radix(VM* vm, const Value* args, Value* ret, int base, const char* fn) {
  if (!IS_NUM(args[0])) return bad_arg(vm, fn, 0, "a number", args[0]);
  int64_t n = (int64_t)AS_NUM(args[0]);
  uint64_t u = n < 0 ? (uint64_t)(-n) : (uint64_t)n;
  char tmp[80];
  int i = 79;
  tmp[i] = 0;
  if (u == 0) tmp[--i] = '0';
  while (u) {
    tmp[--i] = "0123456789abcdef"[u % (uint64_t)base];
    u /= (uint64_t)base;
  }
  Buf b;
  buf_init(&b);
  if (n < 0) buf_addc(&b, '-');
  buf_adds(&b, base == 16 ? "0x" : "0b");
  buf_adds(&b, tmp + i);
  *ret = OBJ_VAL(mkstr(vm, b.d, b.len));
  buf_free(&b);
  return 1;
}
NATIVE(n_hex) {
  (void)argc, (void)ud;
  return radix(vm, args, ret, 16, "hex");
}
NATIVE(n_bin) {
  (void)argc, (void)ud;
  return radix(vm, args, ret, 2, "bin");
}

/* ---------------------------------------------------------------- format -- */
NATIVE(n_format) {
  UNUSED_ALL;
  REQ_STR("format", 0, f);
  Buf out;
  buf_init(&out);
  int ai = 1;
  for (int i = 0; i < f->len; i++) {
    char c = f->chars[i];
    if (c != '%') {
      buf_addc(&out, c);
      continue;
    }
    if (++i >= f->len) {
      buf_free(&out);
      return vm_error(vm, "format: dangling '%%'.");
    }
    if (f->chars[i] == '%') {
      buf_addc(&out, '%');
      continue;
    }
    char spec[32];
    int sl = 0;
    spec[sl++] = '%';
    while (i < f->len && strchr("-+ #0123456789.", f->chars[i]) && sl < 24) spec[sl++] = f->chars[i++];
    if (i >= f->len) {
      buf_free(&out);
      return vm_error(vm, "format: incomplete specifier.");
    }
    char conv = f->chars[i];
    if (ai >= argc) {
      buf_free(&out);
      return vm_error(vm, "format: not enough arguments.");
    }
    Value a = args[ai++];
    char tmp[512];
    switch (conv) {
      case 'd': case 'i': case 'x': case 'X': case 'o': case 'c': {
        if (!IS_NUM(a)) {
          buf_free(&out);
          return vm_error(vm, "format: %%%c needs a number, got %s.", conv, value_type_name(a));
        }
        if (conv == 'c') {
          utf8_enc(&out, (unsigned long)AS_NUM(a));
          break;
        }
        spec[sl++] = 'l';
        spec[sl++] = 'l';
        spec[sl++] = conv == 'i' ? 'd' : conv;
        spec[sl] = 0;
        snprintf(tmp, sizeof tmp, spec, (long long)AS_NUM(a));
        buf_adds(&out, tmp);
        break;
      }
      case 'f': case 'e': case 'g': case 'E': case 'G': {
        if (!IS_NUM(a)) {
          buf_free(&out);
          return vm_error(vm, "format: %%%c needs a number, got %s.", conv, value_type_name(a));
        }
        spec[sl++] = conv;
        spec[sl] = 0;
        snprintf(tmp, sizeof tmp, spec, AS_NUM(a));
        buf_adds(&out, tmp);
        break;
      }
      case 's': case 'r': {
        /* width/precision count UTF-8 characters, not bytes (matters for Arabic) */
        Buf t;
        buf_init(&t);
        if (conv == 's') value_tostr_buf(vm, &t, a);
        else value_repr_buf(vm, &t, a, 0);
        bool left = false;
        int width = 0, prec = -1;
        for (int k = 1; k < sl; k++) {
          if (spec[k] == '-') left = true;
          else if (spec[k] == '.') {
            prec = 0;
            for (k++; k < sl && spec[k] >= '0' && spec[k] <= '9'; k++) prec = prec * 10 + (spec[k] - '0');
            break;
          } else if (spec[k] >= '1' && spec[k] <= '9') {
            while (k < sl && spec[k] >= '0' && spec[k] <= '9') width = width * 10 + (spec[k++] - '0');
            k--;
          }
        }
        size_t bytes = t.len;
        int chars = 0;
        for (size_t k = 0; k < t.len; k++) {
          if (((unsigned char)t.d[k] & 0xC0) != 0x80) {
            if (prec >= 0 && chars == prec) {
              bytes = k;
              break;
            }
            chars++;
          }
        }
        if (width > 100000) width = 100000;
        if (!left) for (int k = chars; k < width; k++) buf_addc(&out, ' ');
        buf_add(&out, t.d ? t.d : "", bytes);
        if (left) for (int k = chars; k < width; k++) buf_addc(&out, ' ');
        buf_free(&t);
        break;
      }
      default:
        buf_free(&out);
        return vm_error(vm, "format: unknown specifier '%%%c'.", conv);
    }
  }
  ObjString* r = mkstr(vm, out.d ? out.d : "", out.len);
  buf_free(&out);
  RET(OBJ_VAL(r));
}

/* ---------------------------------------------------------------- bytes --- */
NATIVE(n_bytes) {
  UNUSED_ALL;
  Value v = args[0];
  if (IS_NUM(v)) {
    if (AS_NUM(v) < 0 || AS_NUM(v) > 1e9) return vm_error(vm, "bytes: bad size.");
    RET(OBJ_VAL(bytes_new(vm, (int)AS_NUM(v))));
  }
  if (IS_STRING(v)) {
    ObjBytes* b = bytes_new(vm, AS_STRING(v)->len);
    if (b->len) memcpy(b->data, AS_CSTRING(v), (size_t)b->len);
    RET(OBJ_VAL(b));
  }
  if (IS_LIST(v)) {
    ObjList* l = AS_LIST(v);
    ObjBytes* b = bytes_new(vm, l->count);
    for (int i = 0; i < l->count; i++) {
      Value e = l->items[i];
      if (!IS_NUM(e) || AS_NUM(e) < 0 || AS_NUM(e) > 255 || AS_NUM(e) != floor(AS_NUM(e)))
        return vm_error(vm, "bytes: element %d is not an integer 0..255.", i);
      b->data[i] = (uint8_t)AS_NUM(e);
    }
    RET(OBJ_VAL(b));
  }
  if (IS_BYTES(v)) RET(v);
  return bad_arg(vm, "bytes", 0, "a number, string or list", v);
}
NATIVE(n_to_string) { /* bytes -> string */
  UNUSED_ALL;
  if (!IS_BYTES(args[0])) return bad_arg(vm, "to_string", 0, "bytes", args[0]);
  ObjBytes* b = AS_BYTES(args[0]);
  RET(OBJ_VAL(mkstr(vm, (const char*)b->data, (size_t)b->len)));
}
NATIVE(n_to_bytes) {
  UNUSED_ALL;
  return n_bytes(vm, 1, args, ret, NULL);
}
NATIVE(n_to_hex) {
  UNUSED_ALL;
  if (!IS_BYTES(args[0])) return bad_arg(vm, "to_hex", 0, "bytes", args[0]);
  ObjBytes* b = AS_BYTES(args[0]);
  Buf o;
  buf_init(&o);
  for (int i = 0; i < b->len; i++) buf_addf(&o, "%02x", b->data[i]);
  ObjString* r = mkstr(vm, o.d ? o.d : "", o.len);
  buf_free(&o);
  RET(OBJ_VAL(r));
}

/* -------------------------------------------------------------------- io -- */
NATIVE(n_input) {
  UNUSED_ALL;
  if (argc > 0) print_values(vm, 1, args, false);
  fflush(stdout);
  Buf b;
  buf_init(&b);
  int c, got = 0;
  while ((c = getchar()) != EOF) {
    got = 1;
    if (c == '\n') break;
    buf_addc(&b, (char)c);
  }
  if (!got) {
    buf_free(&b);
    RET(NIL_VAL);
  }
  if (b.len && b.d[b.len - 1] == '\r') b.len--;
  ObjString* r = mkstr(vm, b.d ? b.d : "", b.len);
  buf_free(&b);
  RET(OBJ_VAL(r));
}
NATIVE(n_read_file) {
  UNUSED_ALL;
  REQ_STR("read_file", 0, p);
  size_t n;
  char* d = vm_read_file(p->chars, &n);
  if (!d) return vm_error(vm, "read_file: cannot open '%s'.", p->chars);
  ObjString* r = mkstr(vm, d, n);
  free(d);
  RET(OBJ_VAL(r));
}
NATIVE(n_read_bytes) {
  UNUSED_ALL;
  REQ_STR("read_bytes", 0, p);
  size_t n;
  char* d = vm_read_file(p->chars, &n);
  if (!d) return vm_error(vm, "read_bytes: cannot open '%s'.", p->chars);
  ObjBytes* b = bytes_new(vm, (int)n);
  if (n) memcpy(b->data, d, n);
  free(d);
  RET(OBJ_VAL(b));
}
static int write_impl(VM* vm, const Value* args, Value* ret, const char* mode, const char* fn) {
  if (!IS_STRING(args[0])) return bad_arg(vm, fn, 0, "a string", args[0]);
  const char* data;
  size_t n;
  if (IS_STRING(args[1])) {
    data = AS_CSTRING(args[1]);
    n = (size_t)AS_STRING(args[1])->len;
  } else if (IS_BYTES(args[1])) {
    data = (const char*)AS_BYTES(args[1])->data;
    n = (size_t)AS_BYTES(args[1])->len;
  } else return bad_arg(vm, fn, 1, "a string or bytes", args[1]);
  FILE* f = fopen(AS_CSTRING(args[0]), mode);
  if (!f) return vm_error(vm, "%s: cannot open '%s'.", fn, AS_CSTRING(args[0]));
  if (n) fwrite(data, 1, n, f);
  fclose(f);
  *ret = NIL_VAL;
  return 1;
}
NATIVE(n_write_file) {
  (void)argc, (void)ud;
  return write_impl(vm, args, ret, "wb", "write_file");
}
NATIVE(n_append_file) {
  (void)argc, (void)ud;
  return write_impl(vm, args, ret, "ab", "append_file");
}
NATIVE(n_file_exists) {
  UNUSED_ALL;
  REQ_STR("file_exists", 0, p);
  FILE* f = fopen(p->chars, "rb");
  if (f) fclose(f);
  RET(BOOL_VAL(f != NULL));
}
NATIVE(n_exit) {
  UNUSED_ALL;
  int code = 0;
  if (argc > 0 && IS_NUM(args[0])) code = (int)AS_NUM(args[0]);
  fflush(stdout);
  exit(code);
}
NATIVE(n_env) {
  UNUSED_ALL;
  REQ_STR("env", 0, n);
  const char* v = getenv(n->chars);
  RET(v ? OBJ_VAL(str_from(vm, v)) : NIL_VAL);
}

/* ------------------------------------------------------- errors & scope -- */
NATIVE(n_error) {
  UNUSED_ALL;
  vm_throw(vm, args[0]);
  return 0;
}
NATIVE(n_assert) {
  UNUSED_ALL;
  if (value_truthy(args[0])) RET(NIL_VAL);
  if (argc > 1) {
    Buf b;
    buf_init(&b);
    buf_adds(&b, "Assertion failed: ");
    value_tostr_buf(vm, &b, args[1]);
    vm_error(vm, "%s", buf_str(&b));
    buf_free(&b);
  } else {
    vm_error(vm, "Assertion failed.");
  }
  return 0;
}
NATIVE(n_pcall) {
  UNUSED_ALL;
  REQ_FN("pcall", 0);
  Value res = NIL_VAL;
  /* The result list is allocated AFTER the call: the GC can run inside it, and
   * nothing allocated before would be rooted. No safepoint follows. */
  bool ok = vm_call(vm, args[0], argc - 1, args + 1, &res);
  Value e = vm->errval;
  if (!ok) {
    vm->trace.len = 0;
    if (vm->trace.d) vm->trace.d[0] = 0;
  }
  ObjList* out = list_new(vm);
  list_push(vm, out, BOOL_VAL(ok));
  list_push(vm, out, ok ? res : e);
  RET(OBJ_VAL(out));
}
NATIVE(n_apply) { /* apply(fn, [args...]) */
  UNUSED_ALL;
  REQ_FN("apply", 0);
  REQ_LIST("apply", 1, l);
  if (l->count > 200) return vm_error(vm, "apply: too many arguments.");
  Value res = NIL_VAL;
  Value tmp[200];
  for (int i = 0; i < l->count; i++) tmp[i] = l->items[i];
  if (!vm_call(vm, args[0], l->count, tmp, &res)) return 0;
  RET(res);
}
NATIVE(n_require) {
  UNUSED_ALL;
  REQ_STR("require", 0, p);
  return module_require(vm, p->chars, ret) ? 1 : 0;
}
NATIVE(n_getglobal) {
  UNUSED_ALL;
  REQ_STR("getglobal", 0, n);
  Value v;
  RET(map_get(vm->globals, OBJ_VAL(n), &v) ? v : NIL_VAL);
}
NATIVE(n_setglobal) {
  UNUSED_ALL;
  REQ_STR("setglobal", 0, n);
  map_set(vm, vm->globals, OBJ_VAL(n), args[1]);
  RET(args[1]);
}
NATIVE(n_defined) {
  UNUSED_ALL;
  REQ_STR("defined", 0, n);
  RET(BOOL_VAL(map_get(vm->globals, OBJ_VAL(n), NULL)));
}
NATIVE(n_is_callable) {
  UNUSED_ALL;
  RET(BOOL_VAL(IS_CALLABLE(args[0])));
}

/* ---------------------------------------------------------- require() ----- */
static bool file_readable(const char* p) {
  FILE* f = fopen(p, "rb");
  if (f) fclose(f);
  return f != NULL;
}
static bool resolve_module(VM* vm, const char* name, Buf* out) {
  static const char* exts[] = {"", ".exb", "/init.exb", NULL};
  Buf base;
  buf_init(&base);
  /* 1: relative to the requiring file */
  if (vm->frameCount > 0) {
    ObjFunction* fn = vm->frames[vm->frameCount - 1].closure->fn;
    if (fn->file) {
      const char* f = fn->file->chars;
      const char* slash = strrchr(f, '/');
      if (slash) buf_add(&base, f, (size_t)(slash - f + 1));
    }
  }
  for (int i = 0; exts[i]; i++) {
    out->len = 0;
    buf_addf(out, "%s%s%s", base.d ? base.d : "", name, exts[i]);
    if (file_readable(out->d) && out->d[out->len - 1] != '/') {
      buf_free(&base);
      return true;
    }
  }
  buf_free(&base);
  /* 2: plain name, then each library path */
  for (int i = 0; exts[i]; i++) {
    out->len = 0;
    buf_addf(out, "%s%s", name, exts[i]);
    if (file_readable(out->d)) return true;
  }
  for (int p = 0; p < vm->libCount; p++) {
    for (int i = 0; exts[i]; i++) {
      out->len = 0;
      buf_addf(out, "%s/%s%s", vm->libPaths[p], name, exts[i]);
      if (file_readable(out->d)) return true;
    }
  }
  return false;
}

bool module_require(VM* vm, const char* name, Value* out) {
  Buf path;
  buf_init(&path);
  if (!resolve_module(vm, name, &path)) {
    buf_free(&path);
    return vm_error(vm, "require: cannot find module '%s'.", name);
  }
  Value key = OBJ_VAL(str_copy(vm, path.d, path.len));
  buf_free(&path);
  Value cached;
  if (map_get(vm->modules, key, &cached)) {
    if (IS_MAP(cached) && AS_OBJ(cached) == (Obj*)vm->modules) return vm_error(vm, "require: circular dependency on '%s'.", name);
    *out = cached;
    return true;
  }
  size_t n;
  char* src = vm_read_file(AS_CSTRING(key), &n);
  if (!src) return vm_error(vm, "require: cannot read '%s'.", name);
  ObjFunction* fn;
  if (bytecode_check_magic(src, n)) fn = bytecode_read(vm, (const uint8_t*)src, n);
  else fn = compile_source(vm, src, AS_CSTRING(key));
  free(src);
  if (!fn) {
    const char* m = vm->errmsg ? vm->errmsg : "unknown error";
    return vm_error(vm, "require('%s') failed:\n%s", name, m);
  }
  map_set(vm, vm->modules, key, OBJ_VAL(vm->modules)); /* loading marker */
  Value result = NIL_VAL;
  vm_push(vm, key);
  bool ok = vm_call(vm, OBJ_VAL(fn), 0, NULL, &result);
  vm_pop(vm);
  if (!ok) {
    map_delete(vm->modules, key);
    return false;
  }
  map_set(vm, vm->modules, key, result);
  *out = result;
  return true;
}

/* ------------------------------------------------------------- register -- */
void natives_register(VM* vm) {
  vm_define_global(vm, "PI", NUM_VAL(3.14159265358979323846));
  vm_define_global(vm, "E", NUM_VAL(2.71828182845904523536));
  vm_define_global(vm, "INF", NUM_VAL(INFINITY));
  vm_define_global(vm, "EXBIN_VERSION", OBJ_VAL(str_from(vm, EXBIN_VERSION)));

  natives_def(vm, "print", n_print, 0, -1);
  natives_def(vm, "write", n_write, 0, -1);
  natives_def(vm, "input", n_input, 0, 1);
  natives_def(vm, "type", n_type, 1, 1);
  natives_def(vm, "str", n_str, 1, 1);
  natives_def(vm, "repr", n_repr, 1, 1);
  natives_def(vm, "num", n_num, 1, 1);
  natives_def(vm, "int", n_int, 1, 1);
  natives_def(vm, "list", n_list, 1, 1);
  natives_def(vm, "range", n_range, 1, 3);
  natives_def(vm, "bytes", n_bytes, 1, 1);
  natives_def(vm, "error", n_error, 1, 1);
  natives_def(vm, "assert", n_assert, 1, 2);
  natives_def(vm, "pcall", n_pcall, 1, -1);
  natives_def(vm, "apply", n_apply, 2, 2);
  natives_def(vm, "require", n_require, 1, 1);
  natives_def(vm, "getglobal", n_getglobal, 1, 1);
  natives_def(vm, "setglobal", n_setglobal, 2, 2);
  natives_def(vm, "defined", n_defined, 1, 1);
  natives_def(vm, "callable", n_is_callable, 1, 1);
  natives_def(vm, "exit", n_exit, 0, 1);
  natives_def(vm, "env", n_env, 1, 1);
  natives_def(vm, "read_file", n_read_file, 1, 1);
  natives_def(vm, "read_bytes", n_read_bytes, 1, 1);
  natives_def(vm, "write_file", n_write_file, 2, 2);
  natives_def(vm, "append_file", n_append_file, 2, 2);
  natives_def(vm, "file_exists", n_file_exists, 1, 1);
  natives_def(vm, "format", n_format, 1, -1);
  natives_def(vm, "clock", n_clock, 0, 0);
  natives_def(vm, "time", n_time, 0, 0);
  natives_def(vm, "sleep", n_sleep, 1, 1);
  natives_def(vm, "random", n_random, 0, 2);
  natives_def(vm, "seed", n_seed, 1, 1);
  natives_def(vm, "chr", n_chr, 1, 1);
  natives_def(vm, "ord", n_ord, 1, 1);
  natives_def(vm, "min", n_min, 1, -1);
  natives_def(vm, "max", n_max, 1, -1);
  natives_def(vm, "clamp", n_clamp, 3, 3);
  natives_def(vm, "pow", n_pow, 2, 2);
  natives_def(vm, "atan2", n_atan2, 2, 2);
  natives_def(vm, "zip", n_zip, 2, 2);
  natives_def(vm, "enumerate", n_enumerate, 1, 1);

  def_both(vm, "len", n_len, 1, 1, "string,list,map,bytes,range");
  def_both(vm, "push", n_push, 2, -1, "list,bytes");
  def_both(vm, "pop", n_pop, 1, 2, "list,bytes");
  def_both(vm, "insert", n_insert, 3, 3, "list");
  def_both(vm, "remove", n_remove, 2, 2, "list");
  def_both(vm, "delete", n_delete, 2, 2, "map");
  def_both(vm, "keys", n_keys, 1, 1, "map");
  def_both(vm, "values", n_values, 1, 1, "map");
  def_both(vm, "items", n_items, 1, 1, "map");
  def_both(vm, "has", n_has, 2, 2, "map,list,string,bytes");
  def_both(vm, "find", n_find, 2, 3, "string,list");
  def_both(vm, "copy", n_copy, 1, 1, "list,map,bytes");
  def_both(vm, "slice", n_slice, 2, 3, "string,list,bytes");
  def_both(vm, "join", n_join, 1, 2, "list");
  def_both(vm, "chars", n_chars, 1, 1, "string");
  def_both(vm, "split", n_split, 1, 2, "string");
  def_both(vm, "lines", n_lines, 1, 1, "string");
  def_both(vm, "replace", n_replace, 3, 4, "string");
  def_both(vm, "upper", n_upper, 1, 1, "string");
  def_both(vm, "lower", n_lower, 1, 1, "string");
  def_both(vm, "trim", n_trim, 1, 1, "string");
  def_both(vm, "trim_start", n_trim_start, 1, 1, "string");
  def_both(vm, "trim_end", n_trim_end, 1, 1, "string");
  def_both(vm, "starts_with", n_starts_with, 2, 2, "string");
  def_both(vm, "ends_with", n_ends_with, 2, 2, "string");
  def_both(vm, "repeat", n_repeat, 2, 2, "string");
  def_both(vm, "pad_left", n_pad_left, 2, 3, "string");
  def_both(vm, "pad_right", n_pad_right, 2, 3, "string");
  def_both(vm, "substr", n_substr, 2, 3, "string");
  def_both(vm, "utf8_len", n_utf8_len, 1, 1, "string");
  def_both(vm, "reverse", n_reverse, 1, 1, "string,list");
  def_both(vm, "map", n_map, 2, 2, "list");
  def_both(vm, "filter", n_filter, 2, 2, "list");
  def_both(vm, "reduce", n_reduce, 2, 3, "list");
  def_both(vm, "each", n_each, 2, 2, "list");
  def_both(vm, "any", n_any, 1, 2, "list");
  def_both(vm, "all", n_all, 1, 2, "list");
  def_both(vm, "sum", n_sum, 1, 1, "list");
  def_both(vm, "sort", n_sort, 1, 2, "list");
  def_both(vm, "to_string", n_to_string, 1, 1, "bytes");
  def_both(vm, "to_bytes", n_to_bytes, 1, 1, "string,list");
  def_both(vm, "to_hex", n_to_hex, 1, 1, "bytes");
  def_both(vm, "hex", n_hex, 1, 1, "number");
  def_both(vm, "bin", n_bin, 1, 1, "number");
  def_both(vm, "abs", n_abs, 1, 1, "number");
  def_both(vm, "floor", n_floor, 1, 1, "number");
  def_both(vm, "ceil", n_ceil, 1, 1, "number");
  def_both(vm, "round", n_round, 1, 1, "number");
  def_both(vm, "sqrt", n_sqrt, 1, 1, "number");
  def_both(vm, "sign", n_sign, 1, 1, "number");
  natives_def(vm, "sin", n_sin, 1, 1);
  natives_def(vm, "cos", n_cos, 1, 1);
  natives_def(vm, "tan", n_tan, 1, 1);
  natives_def(vm, "asin", n_asin, 1, 1);
  natives_def(vm, "acos", n_acos, 1, 1);
  natives_def(vm, "atan", n_atan, 1, 1);
  natives_def(vm, "log", n_log, 1, 1);
  natives_def(vm, "log2", n_log2, 1, 1);
  natives_def(vm, "log10", n_log10, 1, 1);
  natives_def(vm, "exp", n_exp, 1, 1);
}
