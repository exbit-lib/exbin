/* EXBIN: the language-building toolkit.
 *
 *   regex(pattern, flags)      compiled regular expressions
 *   lexer(rules, opts)         table-driven maximal-munch lexer generator
 *   asm(name, arity, upvals)   bytecode assembler -> runnable functions
 *   compile / eval / disasm / serialize / deserialize / save_bytecode ...
 *
 * With these, a language implemented *in* EXBIN gets a real lexer and can
 * either interpret its AST itself or compile to EXBIN bytecode and run on
 * this VM (and ship it as a .exbc file).
 */
#include "regex.h"
#include "vm.h"

#define NATIVE(n) static int n(VM* vm, int argc, const Value* args, Value* ret, void* ud)
#define UNUSED_ALL (void)vm, (void)argc, (void)args, (void)ret, (void)ud
#define RET(v) \
  do {         \
    *ret = (v);\
    return 1;  \
  } while (0)

static int bad_arg(VM* vm, const char* fn, int i, const char* want, Value got) {
  vm_error(vm, "%s: argument %d must be %s, got %s.", fn, i + 1, want, value_type_name(got));
  return 0;
}

static ObjHandle* self_handle(VM* vm, Value v, const char* kind, const char* fn) {
  if (!IS_HANDLE(v) || strcmp(AS_HANDLE(v)->kind, kind) != 0) {
    vm_error(vm, "%s: expected a %s object, got %s.", fn, kind, value_type_name(v));
    return NULL;
  }
  return AS_HANDLE(v);
}

/* ================================================================ regex === */
typedef struct {
  Regex* re;
  ObjString* pattern;
} RegexH;

static void regex_mark(VM* vm, void* d) { gc_mark_obj(vm, (Obj*)((RegexH*)d)->pattern); }
static void regex_destroy(void* d) {
  regex_free(((RegexH*)d)->re);
  free(d);
}

static Regex* compile_regex(VM* vm, ObjString* pat, int flags, const char* fn) {
  char err[160];
  Regex* re = regex_compile(pat->chars, (size_t)pat->len, flags, err, sizeof err);
  if (!re) vm_error(vm, "%s: invalid pattern /%s/: %s", fn, pat->chars, err);
  return re;
}

NATIVE(n_regex) {
  UNUSED_ALL;
  if (!IS_STRING(args[0])) return bad_arg(vm, "regex", 0, "a string", args[0]);
  int flags = 0;
  if (argc > 1) {
    if (!IS_STRING(args[1])) return bad_arg(vm, "regex", 1, "a string", args[1]);
    for (int i = 0; i < AS_STRING(args[1])->len; i++) {
      char f = AS_CSTRING(args[1])[i];
      if (f == 'i') flags |= REGEX_FLAG_ICASE;
      else return vm_error(vm, "regex: unknown flag '%c' (supported: i).", f);
    }
  }
  Regex* re = compile_regex(vm, AS_STRING(args[0]), flags, "regex");
  if (!re) return 0;
  RegexH* h = (RegexH*)malloc(sizeof(RegexH));
  h->re = re;
  h->pattern = AS_STRING(args[0]);
  RET(OBJ_VAL(handle_new(vm, "regex", h, regex_mark, regex_destroy)));
}

static Value make_match(VM* vm, const Regex* re, ObjString* text, const int* caps) {
  ObjMap* m = map_new(vm);
  int ng = regex_group_count(re);
  map_set_str(vm, m, "start", NUM_VAL(caps[0]));
  map_set_str(vm, m, "end", NUM_VAL(caps[1]));
  map_set_str(vm, m, "text", OBJ_VAL(str_copy(vm, text->chars + caps[0], (size_t)(caps[1] - caps[0]))));
  ObjList* groups = list_new(vm);
  for (int g = 1; g <= ng; g++) {
    if (caps[2 * g] >= 0 && caps[2 * g + 1] >= caps[2 * g])
      list_push(vm, groups, OBJ_VAL(str_copy(vm, text->chars + caps[2 * g], (size_t)(caps[2 * g + 1] - caps[2 * g]))));
    else list_push(vm, groups, NIL_VAL);
  }
  map_set_str(vm, m, "groups", OBJ_VAL(groups));
  return OBJ_VAL(m);
}

#define REGEX_SELF(fn)                                         \
  ObjHandle* hh = self_handle(vm, args[0], "regex", fn);       \
  if (!hh) return 0;                                           \
  RegexH* rh = (RegexH*)hh->data

#define NEED_STR(fn, i, var)                                   \
  if (!IS_STRING(args[i])) return bad_arg(vm, fn, i, "a string", args[i]); \
  ObjString* var = AS_STRING(args[i])

NATIVE(rx_test) {
  (void)argc, (void)ret, (void)ud;
  REGEX_SELF("test");
  NEED_STR("test", 1, s);
  *ret = BOOL_VAL(regex_search(rh->re, s->chars, (size_t)s->len, 0, NULL, NULL, 0) >= 0);
  return 1;
}
NATIVE(rx_match) {
  (void)ud;
  REGEX_SELF("match");
  NEED_STR("match", 1, s);
  long from = 0;
  if (argc > 2) {
    if (!IS_NUM(args[2])) return bad_arg(vm, "match", 2, "a number", args[2]);
    from = (long)AS_NUM(args[2]);
    if (from < 0) from = 0;
    if (from > s->len) RET(NIL_VAL);
  }
  int caps[2 * (REGEX_MAX_GROUPS + 1)], len;
  int st = regex_search(rh->re, s->chars, (size_t)s->len, (size_t)from, &len, caps, 2 * (REGEX_MAX_GROUPS + 1));
  if (st < 0) RET(NIL_VAL);
  RET(make_match(vm, rh->re, s, caps));
}
NATIVE(rx_match_at) {
  UNUSED_ALL;
  REGEX_SELF("match_at");
  NEED_STR("match_at", 1, s);
  if (!IS_NUM(args[2])) return bad_arg(vm, "match_at", 2, "a number", args[2]);
  long pos = (long)AS_NUM(args[2]);
  if (pos < 0 || pos > s->len) RET(NIL_VAL);
  int caps[2 * (REGEX_MAX_GROUPS + 1)];
  int n = regex_match_at(rh->re, s->chars, (size_t)s->len, (size_t)pos, caps, 2 * (REGEX_MAX_GROUPS + 1));
  if (n < 0) RET(NIL_VAL);
  RET(make_match(vm, rh->re, s, caps));
}
NATIVE(rx_find_all) {
  UNUSED_ALL;
  REGEX_SELF("find_all");
  NEED_STR("find_all", 1, s);
  ObjList* out = list_new(vm);
  size_t pos = 0;
  while (pos <= (size_t)s->len) {
    int caps[2 * (REGEX_MAX_GROUPS + 1)], len;
    int st = regex_search(rh->re, s->chars, (size_t)s->len, pos, &len, caps, 2 * (REGEX_MAX_GROUPS + 1));
    if (st < 0) break;
    list_push(vm, out, make_match(vm, rh->re, s, caps));
    pos = (size_t)st + (len > 0 ? (size_t)len : 1);
  }
  RET(OBJ_VAL(out));
}
NATIVE(rx_replace) {
  UNUSED_ALL;
  REGEX_SELF("replace");
  NEED_STR("replace", 1, s);
  NEED_STR("replace", 2, repl);
  long limit = -1;
  if (argc > 3 && IS_NUM(args[3])) limit = (long)AS_NUM(args[3]);
  Buf out;
  buf_init(&out);
  size_t pos = 0, copied = 0;
  while (pos <= (size_t)s->len && limit != 0) {
    int caps[2 * (REGEX_MAX_GROUPS + 1)], len;
    int st = regex_search(rh->re, s->chars, (size_t)s->len, pos, &len, caps, 2 * (REGEX_MAX_GROUPS + 1));
    if (st < 0) break;
    buf_add(&out, s->chars + copied, (size_t)st - copied);
    for (int i = 0; i < repl->len; i++) {
      char c = repl->chars[i];
      if (c == '$' && i + 1 < repl->len) {
        char d = repl->chars[i + 1];
        if (d == '$') {
          buf_addc(&out, '$');
          i++;
          continue;
        }
        if (d >= '0' && d <= '9') {
          int g = d - '0';
          if (g <= regex_group_count(rh->re) && caps[2 * g] >= 0)
            buf_add(&out, s->chars + caps[2 * g], (size_t)(caps[2 * g + 1] - caps[2 * g]));
          i++;
          continue;
        }
      }
      buf_addc(&out, c);
    }
    copied = (size_t)st + (size_t)len;
    pos = len > 0 ? copied : (size_t)st + 1;
    if (len == 0 && (size_t)st < (size_t)s->len) {
      buf_addc(&out, s->chars[st]);
      copied = (size_t)st + 1;
    }
    if (limit > 0) limit--;
  }
  if (copied < (size_t)s->len) buf_add(&out, s->chars + copied, (size_t)s->len - copied);
  ObjString* r = str_copy(vm, out.d ? out.d : "", out.len);
  buf_free(&out);
  RET(OBJ_VAL(r));
}
NATIVE(rx_split) {
  UNUSED_ALL;
  REGEX_SELF("split");
  NEED_STR("split", 1, s);
  ObjList* out = list_new(vm);
  size_t pos = 0, last = 0;
  while (pos <= (size_t)s->len) {
    int caps[2 * (REGEX_MAX_GROUPS + 1)], len;
    int st = regex_search(rh->re, s->chars, (size_t)s->len, pos, &len, caps, 2 * (REGEX_MAX_GROUPS + 1));
    if (st < 0) break;
    if (len == 0) {
      pos = (size_t)st + 1;
      continue;
    }
    list_push(vm, out, OBJ_VAL(str_copy(vm, s->chars + last, (size_t)st - last)));
    last = (size_t)st + (size_t)len;
    pos = last;
  }
  list_push(vm, out, OBJ_VAL(str_copy(vm, s->chars + last, (size_t)s->len - last)));
  RET(OBJ_VAL(out));
}
NATIVE(rx_groups) {
  UNUSED_ALL;
  REGEX_SELF("groups");
  RET(NUM_VAL(regex_group_count(rh->re)));
}
NATIVE(rx_pattern) {
  UNUSED_ALL;
  REGEX_SELF("pattern");
  RET(OBJ_VAL(rh->pattern));
}

/* ================================================================ lexer === */
typedef struct {
  int nrules;
  Regex** res;
  ObjString** names; /* NULL name => skipped */
  ObjString* identRule;
  ObjMap* keywords;
  ObjString* eofName;
  bool recover;
} LexerH;

static void lexer_mark(VM* vm, void* d) {
  LexerH* l = (LexerH*)d;
  for (int i = 0; i < l->nrules; i++) gc_mark_obj(vm, (Obj*)l->names[i]);
  gc_mark_obj(vm, (Obj*)l->identRule);
  gc_mark_obj(vm, (Obj*)l->keywords);
  gc_mark_obj(vm, (Obj*)l->eofName);
}
static void lexer_destroy(void* d) {
  LexerH* l = (LexerH*)d;
  for (int i = 0; i < l->nrules; i++) regex_free(l->res[i]);
  free(l->res);
  free(l->names);
  free(l);
}

NATIVE(n_lexer) {
  UNUSED_ALL;
  if (!IS_LIST(args[0])) return bad_arg(vm, "lexer", 0, "a list of rules", args[0]);
  ObjList* rules = AS_LIST(args[0]);
  if (rules->count == 0) return vm_error(vm, "lexer: rule list is empty.");
  LexerH* L = (LexerH*)calloc(1, sizeof(LexerH));
  L->res = (Regex**)calloc((size_t)rules->count, sizeof(Regex*));
  L->names = (ObjString**)calloc((size_t)rules->count, sizeof(ObjString*));
  L->nrules = 0;
  L->eofName = str_from(vm, "EOF");
  L->identRule = str_from(vm, "IDENT");
  L->keywords = NULL;
  for (int i = 0; i < rules->count; i++) {
    Value r = rules->items[i];
    Value name = NIL_VAL, pat = NIL_VAL, skip = BOOL_VAL(false), flags = NIL_VAL;
    if (IS_LIST(r)) {
      ObjList* rl = AS_LIST(r);
      if (rl->count < 2) {
        lexer_destroy(L);
        return vm_error(vm, "lexer: rule %d must be [name, pattern].", i + 1);
      }
      name = rl->items[0];
      pat = rl->items[1];
      if (rl->count > 2) flags = rl->items[2];
    } else if (IS_MAP(r)) {
      map_get_str(vm, AS_MAP(r), "name", &name);
      map_get_str(vm, AS_MAP(r), "pattern", &pat);
      map_get_str(vm, AS_MAP(r), "skip", &skip);
      map_get_str(vm, AS_MAP(r), "flags", &flags);
    } else {
      lexer_destroy(L);
      return vm_error(vm, "lexer: rule %d must be a list or a map.", i + 1);
    }
    if (!IS_STRING(pat)) {
      lexer_destroy(L);
      return vm_error(vm, "lexer: rule %d has no pattern string.", i + 1);
    }
    int fl = 0;
    if (IS_STRING(flags) && strchr(AS_CSTRING(flags), 'i')) fl |= REGEX_FLAG_ICASE;
    bool skipped = IS_NIL(name) || value_truthy(skip) || (IS_STRING(name) && strcmp(AS_CSTRING(name), "_") == 0);
    if (!skipped && !IS_STRING(name)) {
      lexer_destroy(L);
      return vm_error(vm, "lexer: rule %d name must be a string (or nil to skip).", i + 1);
    }
    char err[160];
    Regex* re = regex_compile(AS_CSTRING(pat), (size_t)AS_STRING(pat)->len, fl, err, sizeof err);
    if (!re) {
      lexer_destroy(L);
      return vm_error(vm, "lexer: rule %d (/%s/): %s", i + 1, AS_CSTRING(pat), err);
    }
    L->res[L->nrules] = re;
    L->names[L->nrules] = skipped ? NULL : AS_STRING(name);
    L->nrules++;
  }
  if (argc > 1 && IS_MAP(args[1])) {
    Value v;
    if (map_get_str(vm, AS_MAP(args[1]), "keywords", &v) && IS_MAP(v)) L->keywords = AS_MAP(v);
    if (map_get_str(vm, AS_MAP(args[1]), "ident", &v) && IS_STRING(v)) L->identRule = AS_STRING(v);
    if (map_get_str(vm, AS_MAP(args[1]), "eof", &v) && IS_STRING(v)) L->eofName = AS_STRING(v);
    if (map_get_str(vm, AS_MAP(args[1]), "recover", &v)) L->recover = value_truthy(v);
  }
  RET(OBJ_VAL(handle_new(vm, "lexer", L, lexer_mark, lexer_destroy)));
}

NATIVE(lx_tokenize) {
  UNUSED_ALL;
  ObjHandle* hh = self_handle(vm, args[0], "lexer", "tokenize");
  if (!hh) return 0;
  LexerH* L = (LexerH*)hh->data;
  NEED_STR("tokenize", 1, src);
  const char* s = src->chars;
  size_t n = (size_t)src->len;
  ObjString* kType = str_from(vm, "type");
  ObjString* kText = str_from(vm, "text");
  ObjString* kLine = str_from(vm, "line");
  ObjString* kCol = str_from(vm, "col");
  ObjString* kPos = str_from(vm, "pos");
  ObjList* out = list_new(vm);
  size_t pos = 0;
  int line = 1, col = 1;
  while (pos < n) {
    int best = 0, bestRule = -1;
    for (int r = 0; r < L->nrules; r++) {
      int m = regex_match_at(L->res[r], s, n, pos, NULL, 0);
      if (m > best) {
        best = m;
        bestRule = r;
      }
    }
    ObjMap* tok = NULL;
    size_t consumed;
    if (bestRule < 0) {
      /* one UTF-8 character */
      size_t cl = 1;
      while (pos + cl < n && ((unsigned char)s[pos + cl] & 0xC0) == 0x80) cl++;
      if (!L->recover) {
        Buf b;
        buf_init(&b);
        buf_add(&b, s + pos, cl);
        int e = vm_error(vm, "Lexer error at %d:%d: unexpected '%s'.", line, col, buf_str(&b));
        buf_free(&b);
        return e;
      }
      tok = map_new(vm);
      map_set(vm, tok, OBJ_VAL(kType), OBJ_VAL(str_from(vm, "ERROR")));
      map_set(vm, tok, OBJ_VAL(kText), OBJ_VAL(str_copy(vm, s + pos, cl)));
      consumed = cl;
    } else {
      consumed = (size_t)best;
      if (L->names[bestRule]) {
        tok = map_new(vm);
        ObjString* type = L->names[bestRule];
        ObjString* text = str_copy(vm, s + pos, consumed);
        if (L->keywords && type->len == L->identRule->len && memcmp(type->chars, L->identRule->chars, (size_t)type->len) == 0) {
          Value kw;
          if (map_get(L->keywords, OBJ_VAL(text), &kw) && IS_STRING(kw)) type = AS_STRING(kw);
        }
        map_set(vm, tok, OBJ_VAL(kType), OBJ_VAL(type));
        map_set(vm, tok, OBJ_VAL(kText), OBJ_VAL(text));
      }
    }
    if (tok) {
      map_set(vm, tok, OBJ_VAL(kLine), NUM_VAL(line));
      map_set(vm, tok, OBJ_VAL(kCol), NUM_VAL(col));
      map_set(vm, tok, OBJ_VAL(kPos), NUM_VAL((double)pos));
      list_push(vm, out, OBJ_VAL(tok));
    }
    for (size_t i = 0; i < consumed; i++) {
      unsigned char c = (unsigned char)s[pos + i];
      if (c == '\n') {
        line++;
        col = 1;
      } else if ((c & 0xC0) != 0x80) {
        col++;
      }
    }
    pos += consumed;
  }
  ObjMap* eof = map_new(vm);
  map_set(vm, eof, OBJ_VAL(kType), OBJ_VAL(L->eofName));
  map_set(vm, eof, OBJ_VAL(kText), OBJ_VAL(str_from(vm, "")));
  map_set(vm, eof, OBJ_VAL(kLine), NUM_VAL(line));
  map_set(vm, eof, OBJ_VAL(kCol), NUM_VAL(col));
  map_set(vm, eof, OBJ_VAL(kPos), NUM_VAL((double)n));
  list_push(vm, out, OBJ_VAL(eof));
  RET(OBJ_VAL(out));
}

/* ================================================================= asm === */
typedef struct {
  int op, line;
  Value a, b;
} AIns;

typedef struct {
  AIns* ins;
  int n, cap;
  char** labelNames;
  int* labelTarget;
  int nl, lcap;
  int arity, upvalues;
  int curLine;
  ObjString* name;
} AsmData;

static void asm_mark(VM* vm, void* d) {
  AsmData* a = (AsmData*)d;
  for (int i = 0; i < a->n; i++) {
    gc_mark_value(vm, a->ins[i].a);
    gc_mark_value(vm, a->ins[i].b);
  }
  gc_mark_obj(vm, (Obj*)a->name);
}
static void asm_destroy(void* d) {
  AsmData* a = (AsmData*)d;
  for (int i = 0; i < a->nl; i++) free(a->labelNames[i]);
  free(a->labelNames);
  free(a->labelTarget);
  free(a->ins);
  free(a);
}

NATIVE(n_asm) {
  UNUSED_ALL;
  if (!IS_STRING(args[0])) return bad_arg(vm, "asm", 0, "a string (function name)", args[0]);
  double arity = 0, ups = 0;
  if (argc > 1) {
    if (!IS_NUM(args[1])) return bad_arg(vm, "asm", 1, "a number", args[1]);
    arity = AS_NUM(args[1]);
  }
  if (argc > 2) {
    if (!IS_NUM(args[2])) return bad_arg(vm, "asm", 2, "a number", args[2]);
    ups = AS_NUM(args[2]);
  }
  if (arity < 0 || arity > 255 || ups < 0 || ups > 255 || arity != floor(arity) || ups != floor(ups))
    return vm_error(vm, "asm: arity and upvalue count must be integers 0..255.");
  AsmData* a = (AsmData*)calloc(1, sizeof(AsmData));
  a->arity = (int)arity;
  a->upvalues = (int)ups;
  a->curLine = 1;
  a->name = AS_STRING(args[0]);
  RET(OBJ_VAL(handle_new(vm, "asm", a, asm_mark, asm_destroy)));
}

#define ASM_SELF(fn)                                          \
  ObjHandle* hh = self_handle(vm, args[0], "asm", fn);        \
  if (!hh) return 0;                                          \
  AsmData* A = (AsmData*)hh->data

static int as_uint(VM* vm, const char* fn, Value v, int max, int* out) {
  if (!IS_NUM(v) || AS_NUM(v) < 0 || AS_NUM(v) > max || AS_NUM(v) != floor(AS_NUM(v)))
    return vm_error(vm, "%s: operand must be an integer 0..%d.", fn, max) ? 1 : 0;
  *out = (int)AS_NUM(v);
  return 1;
}

NATIVE(as_op) {
  UNUSED_ALL;
  ASM_SELF("op");
  if (!IS_STRING(args[1])) return bad_arg(vm, "op", 1, "an opcode name", args[1]);
  const char* name = AS_CSTRING(args[1]);
  int op = op_from_name(name);
  if (op < 0) return vm_error(vm, "asm: unknown opcode '%s'.", name);
  int given = argc - 2;
  int want;
  switch (op_kind(op)) {
    case OPK_NONE: want = 0; break;
    case OPK_INVOKE: want = 2; break;
    case OPK_CLOSURE: want = 1; break; /* + optional upvalue list */
    default: want = 1;
  }
  if (given < want || given > want + (op_kind(op) == OPK_CLOSURE ? 1 : 0))
    return vm_error(vm, "asm: %s takes %d operand%s, got %d.", op_name(op), want, want == 1 ? "" : "s", given);
  AIns ins;
  ins.op = op;
  ins.line = A->curLine;
  ins.a = NIL_VAL;
  ins.b = NIL_VAL;
  int tmp = 0;
  switch (op_kind(op)) {
    case OPK_NONE: break;
    case OPK_U8:
      if (!as_uint(vm, name, args[2], 255, &tmp)) return 0;
      ins.a = NUM_VAL(tmp);
      break;
    case OPK_U16:
      if (!as_uint(vm, name, args[2], 65535, &tmp)) return 0;
      ins.a = NUM_VAL(tmp);
      break;
    case OPK_CONST: ins.a = args[2]; break;
    case OPK_NAME:
    case OPK_JUMP:
    case OPK_LOOP:
      if (!IS_STRING(args[2])) return bad_arg(vm, name, 0, "a string", args[2]);
      ins.a = args[2];
      break;
    case OPK_INVOKE:
      if (!IS_STRING(args[2])) return bad_arg(vm, name, 0, "a method name string", args[2]);
      if (!as_uint(vm, name, args[3], 255, &tmp)) return 0;
      ins.a = args[2];
      ins.b = NUM_VAL(tmp);
      break;
    case OPK_CLOSURE: {
      if (!IS_FUNCTION(args[2])) return bad_arg(vm, name, 0, "a function built by asm", args[2]);
      ObjFunction* f = AS_FUNCTION(args[2]);
      ins.a = args[2];
      if (given == 2) {
        if (!IS_LIST(args[3])) return bad_arg(vm, name, 1, "a list of [isLocal, index]", args[3]);
        ins.b = args[3];
      }
      int have = IS_LIST(ins.b) ? AS_LIST(ins.b)->count : 0;
      if (have != f->upvalueCount)
        return vm_error(vm, "asm: CLOSURE of '%s' needs %d upvalue descriptor(s), got %d.",
                        f->name ? f->name->chars : "?", f->upvalueCount, have);
      if (IS_LIST(ins.b)) {
        for (int i = 0; i < have; i++) {
          Value d = AS_LIST(ins.b)->items[i];
          if (!IS_LIST(d) || AS_LIST(d)->count != 2 || !IS_NUM(AS_LIST(d)->items[1]))
            return vm_error(vm, "asm: upvalue descriptor %d must be [isLocal, index].", i);
        }
      }
      break;
    }
  }
  if (A->n == A->cap) {
    A->cap = A->cap ? A->cap * 2 : 32;
    A->ins = (AIns*)realloc(A->ins, sizeof(AIns) * (size_t)A->cap);
  }
  A->ins[A->n++] = ins;
  RET(args[0]);
}

NATIVE(as_label) {
  UNUSED_ALL;
  ASM_SELF("label");
  NEED_STR("label", 1, name);
  for (int i = 0; i < A->nl; i++)
    if (strcmp(A->labelNames[i], name->chars) == 0) return vm_error(vm, "asm: label '%s' defined twice.", name->chars);
  if (A->nl == A->lcap) {
    A->lcap = A->lcap ? A->lcap * 2 : 16;
    A->labelNames = (char**)realloc(A->labelNames, sizeof(char*) * (size_t)A->lcap);
    A->labelTarget = (int*)realloc(A->labelTarget, sizeof(int) * (size_t)A->lcap);
  }
  A->labelNames[A->nl] = strdup(name->chars);
  A->labelTarget[A->nl] = A->n;
  A->nl++;
  RET(args[0]);
}
NATIVE(as_line) {
  UNUSED_ALL;
  ASM_SELF("line");
  if (!IS_NUM(args[1])) return bad_arg(vm, "line", 1, "a number", args[1]);
  A->curLine = (int)AS_NUM(args[1]);
  RET(args[0]);
}
NATIVE(as_count) {
  UNUSED_ALL;
  ASM_SELF("count");
  RET(NUM_VAL(A->n));
}

static int ins_size(const AIns* in) {
  switch (op_kind(in->op)) {
    case OPK_NONE: return 1;
    case OPK_U8: return 2;
    case OPK_INVOKE: return 4;
    case OPK_CLOSURE: return 3 + 2 * AS_FUNCTION(in->a)->upvalueCount;
    default: return 3;
  }
}

NATIVE(as_build) {
  UNUSED_ALL;
  ASM_SELF("build");
  int* offs = (int*)malloc(sizeof(int) * (size_t)(A->n + 1));
  int total = 0;
  for (int i = 0; i < A->n; i++) {
    offs[i] = total;
    total += ins_size(&A->ins[i]);
  }
  offs[A->n] = total;
  ObjFunction* fn = fn_new(vm);
  fn->arity = A->arity;
  fn->upvalueCount = A->upvalues;
  fn->name = A->name;
  fn->file = str_from(vm, "<asm>");
  Chunk* c = &fn->chunk;
#define EMIT(b) chunk_write(vm, c, (uint8_t)(b), line)
#define EMIT16(v) (EMIT(((v) >> 8) & 0xff), EMIT((v)&0xff))
  for (int i = 0; i < A->n; i++) {
    AIns* in = &A->ins[i];
    int line = in->line;
    EMIT(in->op);
    switch (op_kind(in->op)) {
      case OPK_NONE: break;
      case OPK_U8: EMIT((int)AS_NUM(in->a)); break;
      case OPK_U16: {
        int v = (int)AS_NUM(in->a);
        EMIT16(v);
        break;
      }
      case OPK_CONST:
      case OPK_NAME: {
        int k = chunk_add_const(vm, c, in->a);
        if (k > 0xffff) {
          free(offs);
          return vm_error(vm, "asm: too many constants.");
        }
        EMIT16(k);
        break;
      }
      case OPK_JUMP:
      case OPK_LOOP: {
        int target = -1;
        for (int l = 0; l < A->nl; l++)
          if (strcmp(A->labelNames[l], AS_CSTRING(in->a)) == 0) target = offs[A->labelTarget[l]];
        if (target < 0) {
          free(offs);
          return vm_error(vm, "asm: undefined label '%s'.", AS_CSTRING(in->a));
        }
        int end = offs[i] + 3;
        int delta = op_kind(in->op) == OPK_JUMP ? target - end : end - target;
        if (delta < 0 || delta > 0xffff) {
          free(offs);
          return vm_error(vm, "asm: label '%s' is %s of %s or too far.", AS_CSTRING(in->a),
                          op_kind(in->op) == OPK_JUMP ? "behind" : "ahead", op_name(in->op));
        }
        EMIT16(delta);
        break;
      }
      case OPK_INVOKE: {
        int k = chunk_add_const(vm, c, in->a);
        if (k > 0xffff) {
          free(offs);
          return vm_error(vm, "asm: too many constants.");
        }
        EMIT16(k);
        EMIT((int)AS_NUM(in->b));
        break;
      }
      case OPK_CLOSURE: {
        int k = chunk_add_const(vm, c, in->a);
        if (k > 0xffff) {
          free(offs);
          return vm_error(vm, "asm: too many constants.");
        }
        EMIT16(k);
        ObjFunction* f = AS_FUNCTION(in->a);
        for (int u = 0; u < f->upvalueCount; u++) {
          ObjList* d = AS_LIST(AS_LIST(in->b)->items[u]);
          Value loc = d->items[0];
          EMIT(value_truthy(loc) && !(IS_NUM(loc) && AS_NUM(loc) == 0) ? 1 : 0);
          EMIT((int)AS_NUM(d->items[1]));
        }
        break;
      }
    }
  }
#undef EMIT
#undef EMIT16
  free(offs);
  if (c->count == 0) return vm_error(vm, "asm: no instructions.");
  char err[200];
  if (!chunk_verify(fn, err, sizeof err)) return vm_error(vm, "asm: verification failed: %s", err);
  RET(OBJ_VAL(fn));
}

NATIVE(n_opcodes) {
  UNUSED_ALL;
  ObjList* l = list_new(vm);
  for (int i = 0; i < OP__COUNT; i++) list_push(vm, l, OBJ_VAL(str_from(vm, op_name(i))));
  RET(OBJ_VAL(l));
}

/* ====================================== compile / run / serialize tools === */
static ObjFunction* as_function(Value v) {
  if (IS_FUNCTION(v)) return AS_FUNCTION(v);
  if (IS_CLOSURE(v)) return AS_CLOSURE(v)->fn;
  return NULL;
}

NATIVE(n_compile) {
  UNUSED_ALL;
  NEED_STR("compile", 0, src);
  const char* name = "<compile>";
  if (argc > 1 && IS_STRING(args[1])) name = AS_CSTRING(args[1]);
  ObjFunction* fn = compile_source(vm, src->chars, name);
  if (!fn) return vm_error(vm, "%s", vm->errmsg ? vm->errmsg : "compile error");
  RET(OBJ_VAL(fn));
}
NATIVE(n_eval) {
  UNUSED_ALL;
  NEED_STR("eval", 0, src);
  const char* name = "<eval>";
  if (argc > 1 && IS_STRING(args[1])) name = AS_CSTRING(args[1]);
  ObjFunction* fn = compile_source(vm, src->chars, name);
  if (!fn) return vm_error(vm, "%s", vm->errmsg ? vm->errmsg : "compile error");
  Value out = NIL_VAL;
  if (!vm_call(vm, OBJ_VAL(fn), 0, NULL, &out)) return 0;
  RET(out);
}
NATIVE(n_disasm) {
  UNUSED_ALL;
  ObjFunction* fn = as_function(args[0]);
  if (!fn && IS_STRING(args[0])) {
    fn = compile_source(vm, AS_CSTRING(args[0]), "<disasm>");
    if (!fn) return vm_error(vm, "%s", vm->errmsg ? vm->errmsg : "compile error");
  }
  if (!fn) return bad_arg(vm, "disasm", 0, "a function or source string", args[0]);
  Buf b;
  buf_init(&b);
  chunk_disassemble(fn, &b);
  ObjString* s = str_copy(vm, b.d ? b.d : "", b.len);
  buf_free(&b);
  RET(OBJ_VAL(s));
}
NATIVE(n_serialize) {
  UNUSED_ALL;
  ObjFunction* fn = as_function(args[0]);
  if (!fn) return bad_arg(vm, "serialize", 0, "a function", args[0]);
  Buf b;
  buf_init(&b);
  if (!bytecode_write(vm, fn, &b)) {
    buf_free(&b);
    return 0;
  }
  ObjBytes* out = bytes_new(vm, (int)b.len);
  memcpy(out->data, b.d, b.len);
  buf_free(&b);
  RET(OBJ_VAL(out));
}
NATIVE(n_deserialize) {
  UNUSED_ALL;
  if (!IS_BYTES(args[0])) return bad_arg(vm, "deserialize", 0, "bytes", args[0]);
  ObjFunction* fn = bytecode_read(vm, AS_BYTES(args[0])->data, (size_t)AS_BYTES(args[0])->len);
  if (!fn) return vm_error(vm, "%s", vm->errmsg ? vm->errmsg : "bad bytecode");
  RET(OBJ_VAL(fn));
}
NATIVE(n_save_bytecode) {
  UNUSED_ALL;
  ObjFunction* fn = as_function(args[0]);
  if (!fn) return bad_arg(vm, "save_bytecode", 0, "a function", args[0]);
  NEED_STR("save_bytecode", 1, path);
  Buf b;
  buf_init(&b);
  if (!bytecode_write(vm, fn, &b)) {
    buf_free(&b);
    return 0;
  }
  FILE* f = fopen(path->chars, "wb");
  if (!f) {
    buf_free(&b);
    return vm_error(vm, "save_bytecode: cannot open '%s'.", path->chars);
  }
  fwrite(b.d, 1, b.len, f);
  fclose(f);
  buf_free(&b);
  RET(NIL_VAL);
}
NATIVE(n_load_bytecode) {
  UNUSED_ALL;
  NEED_STR("load_bytecode", 0, path);
  size_t n;
  char* d = vm_read_file(path->chars, &n);
  if (!d) return vm_error(vm, "load_bytecode: cannot open '%s'.", path->chars);
  ObjFunction* fn = bytecode_read(vm, (const uint8_t*)d, n);
  free(d);
  if (!fn) return vm_error(vm, "%s", vm->errmsg ? vm->errmsg : "bad bytecode");
  RET(OBJ_VAL(fn));
}
NATIVE(n_fn_info) {
  UNUSED_ALL;
  ObjFunction* fn = as_function(args[0]);
  if (!fn) return bad_arg(vm, "fn_info", 0, "a function", args[0]);
  ObjMap* m = map_new(vm);
  map_set_str(vm, m, "name", fn->name ? OBJ_VAL(fn->name) : NIL_VAL);
  map_set_str(vm, m, "arity", NUM_VAL(fn->arity));
  map_set_str(vm, m, "upvalues", NUM_VAL(fn->upvalueCount));
  map_set_str(vm, m, "code_size", NUM_VAL(fn->chunk.count));
  map_set_str(vm, m, "constants", NUM_VAL(fn->chunk.consts.count));
  map_set_str(vm, m, "max_stack", NUM_VAL(fn->maxStack));
  RET(OBJ_VAL(m));
}

void toolkit_register(VM* vm) {
  natives_def(vm, "regex", n_regex, 1, 2);
  natives_def_method(vm, "regex", "test", rx_test, 1, 1);
  natives_def_method(vm, "regex", "match", rx_match, 1, 2);
  natives_def_method(vm, "regex", "match_at", rx_match_at, 2, 2);
  natives_def_method(vm, "regex", "find_all", rx_find_all, 1, 1);
  natives_def_method(vm, "regex", "replace", rx_replace, 2, 3);
  natives_def_method(vm, "regex", "split", rx_split, 1, 1);
  natives_def_method(vm, "regex", "groups", rx_groups, 0, 0);
  natives_def_method(vm, "regex", "pattern", rx_pattern, 0, 0);

  natives_def(vm, "lexer", n_lexer, 1, 2);
  natives_def_method(vm, "lexer", "tokenize", lx_tokenize, 1, 1);

  natives_def(vm, "asm", n_asm, 1, 3);
  natives_def_method(vm, "asm", "op", as_op, 1, -1);
  natives_def_method(vm, "asm", "label", as_label, 1, 1);
  natives_def_method(vm, "asm", "line", as_line, 1, 1);
  natives_def_method(vm, "asm", "count", as_count, 0, 0);
  natives_def_method(vm, "asm", "build", as_build, 0, 0);
  natives_def(vm, "opcodes", n_opcodes, 0, 0);

  natives_def(vm, "compile", n_compile, 1, 2);
  natives_def(vm, "eval", n_eval, 1, 2);
  natives_def(vm, "disasm", n_disasm, 1, 1);
  natives_def(vm, "serialize", n_serialize, 1, 1);
  natives_def(vm, "deserialize", n_deserialize, 1, 1);
  natives_def(vm, "save_bytecode", n_save_bytecode, 2, 2);
  natives_def(vm, "load_bytecode", n_load_bytecode, 1, 1);
  natives_def(vm, "fn_info", n_fn_info, 1, 1);
}
