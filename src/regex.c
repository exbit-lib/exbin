/* EXBIN: backtracking regex engine (see regex.h). Pattern -> AST -> program,
 * executed with an explicit backtrack stack and hard step/stack limits so
 * hostile patterns cannot hang or exhaust memory. */
#include "regex.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { N_CHAR, N_ANY, N_CLASS, N_CAT, N_ALT, N_REPEAT, N_GROUP, N_BOL, N_EOL, N_WORDB, N_NWORDB, N_EMPTY };
enum { I_CHAR, I_ANY, I_CLASS, I_SPLIT, I_JMP, I_SAVE, I_BOL, I_EOL, I_WORDB, I_NWORDB, I_MATCH };

typedef struct Node {
  int type;
  int c; /* char / class index / group index */
  int min, max;
  int lazy;
  struct Node *a, *b;
  struct Node* nextAlloc;
} Node;

typedef struct {
  uint8_t op;
  int x, y;
} Inst;

typedef uint8_t ClassSet[32];

struct Regex {
  Inst* prog;
  int n, cap;
  ClassSet* classes;
  int nclasses, classcap;
  int ngroups;
};

#define MAX_PROG 200000
#define MAX_STEPS 20000000L
#define MAX_BT 2000000
#define MAX_REPEAT 1000

typedef struct {
  const char* p;
  const char* end;
  int flags;
  Regex* re;
  Node* nodes; /* allocation list */
  char* err;
  size_t errsz;
  int failed;
  int depth;
} P;

static void perr(P* p, const char* msg) {
  if (!p->failed) {
    p->failed = 1;
    snprintf(p->err, p->errsz, "%s", msg);
  }
}

static Node* mk(P* p, int type) {
  Node* n = (Node*)calloc(1, sizeof(Node));
  if (!n) {
    perr(p, "out of memory");
    return NULL;
  }
  n->type = type;
  n->nextAlloc = p->nodes;
  p->nodes = n;
  return n;
}

static int new_class(P* p, const ClassSet set) {
  Regex* re = p->re;
  if (re->nclasses == re->classcap) {
    int nc = re->classcap ? re->classcap * 2 : 8;
    ClassSet* t = (ClassSet*)realloc(re->classes, sizeof(ClassSet) * (size_t)nc);
    if (!t) {
      perr(p, "out of memory");
      return 0;
    }
    re->classes = t;
    re->classcap = nc;
  }
  memcpy(re->classes[re->nclasses], set, 32);
  return re->nclasses++;
}

static void set_bit(ClassSet s, int c) { s[(c >> 3) & 31] |= (uint8_t)(1u << (c & 7)); }
static int has_bit(const uint8_t* s, int c) { return (s[(c >> 3) & 31] >> (c & 7)) & 1; }
static void set_range(ClassSet s, int a, int b) {
  for (int c = a; c <= b; c++) set_bit(s, c);
}
static int is_word_byte(int c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c >= 0x80;
}
static void add_named(ClassSet s, char kind) {
  ClassSet t;
  memset(t, 0, 32);
  int neg = (kind == 'D' || kind == 'W' || kind == 'S');
  switch (kind) {
    case 'd':
    case 'D': set_range(t, '0', '9'); break;
    case 'w':
    case 'W':
      for (int c = 0; c < 256; c++)
        if (is_word_byte(c)) set_bit(t, c);
      break;
    default:
      set_bit(t, ' ');
      set_bit(t, '\t');
      set_bit(t, '\n');
      set_bit(t, '\r');
      set_bit(t, '\f');
      set_bit(t, '\v');
  }
  for (int c = 0; c < 256; c++)
    if (has_bit(t, c) != neg) set_bit(s, c);
}
static int hexv(int c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}
static int lower(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }
static int upper(int c) { return (c >= 'a' && c <= 'z') ? c - 32 : c; }

/* Parses an escape after '\' (p->p points at the char after the backslash).
 * Returns the literal byte value, or -1 if it was a class escape (stored in
 * *named), or -2 on error. */
static int parse_escape(P* p, char* named) {
  if (p->p >= p->end) {
    perr(p, "trailing backslash");
    return -2;
  }
  char c = *p->p++;
  *named = 0;
  switch (c) {
    case 'd': case 'D': case 'w': case 'W': case 's': case 'S': *named = c; return -1;
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    case '0': return 0;
    case 'x': {
      if (p->end - p->p < 2 || hexv(p->p[0]) < 0 || hexv(p->p[1]) < 0) {
        perr(p, "\\x needs two hex digits");
        return -2;
      }
      int v = hexv(p->p[0]) * 16 + hexv(p->p[1]);
      p->p += 2;
      return v;
    }
    default: return (unsigned char)c;
  }
}

static Node* parse_alt(P* p);

static Node* parse_class(P* p) {
  ClassSet s;
  memset(s, 0, 32);
  int neg = 0;
  if (p->p < p->end && *p->p == '^') {
    neg = 1;
    p->p++;
  }
  int first = 1;
  for (;;) {
    if (p->p >= p->end) {
      perr(p, "unterminated character class");
      return NULL;
    }
    int c = (unsigned char)*p->p++;
    if (c == ']' && !first) break;
    first = 0;
    char named = 0;
    if (c == '\\') {
      c = parse_escape(p, &named);
      if (c == -2) return NULL;
      if (c == -1) {
        add_named(s, named);
        continue;
      }
    }
    int lo = c, hi = c;
    if (p->p + 1 < p->end && *p->p == '-' && p->p[1] != ']') {
      p->p++;
      int d = (unsigned char)*p->p++;
      if (d == '\\') {
        d = parse_escape(p, &named);
        if (d == -2) return NULL;
        if (d == -1) {
          perr(p, "invalid range in character class");
          return NULL;
        }
      }
      hi = d;
      if (hi < lo) {
        perr(p, "invalid range (end before start)");
        return NULL;
      }
    }
    set_range(s, lo, hi);
  }
  if (p->flags & REGEX_FLAG_ICASE) {
    for (int c = 'a'; c <= 'z'; c++) {
      if (has_bit(s, c)) set_bit(s, upper(c));
      if (has_bit(s, upper(c))) set_bit(s, c);
    }
  }
  if (neg)
    for (int i = 0; i < 32; i++) s[i] = (uint8_t)~s[i];
  Node* n = mk(p, N_CLASS);
  if (!n) return NULL;
  n->c = new_class(p, s);
  return n;
}

static Node* parse_atom(P* p) {
  if (p->p >= p->end) return mk(p, N_EMPTY);
  char c = *p->p++;
  switch (c) {
    case '(': {
      int capture = 1;
      if (p->end - p->p >= 2 && p->p[0] == '?' && p->p[1] == ':') {
        capture = 0;
        p->p += 2;
      }
      int idx = 0;
      if (capture) {
        if (p->re->ngroups >= REGEX_MAX_GROUPS) {
          perr(p, "too many capture groups");
          return NULL;
        }
        idx = ++p->re->ngroups;
      }
      if (++p->depth > 100) {
        perr(p, "pattern nested too deeply");
        return NULL;
      }
      Node* inner = parse_alt(p);
      p->depth--;
      if (!inner) return NULL;
      if (p->p >= p->end || *p->p != ')') {
        perr(p, "missing ')'");
        return NULL;
      }
      p->p++;
      if (!capture) return inner;
      Node* g = mk(p, N_GROUP);
      if (!g) return NULL;
      g->c = idx;
      g->a = inner;
      return g;
    }
    case '[': return parse_class(p);
    case '.': return mk(p, N_ANY);
    case '^': return mk(p, N_BOL);
    case '$': return mk(p, N_EOL);
    case '\\': {
      char named = 0;
      if (p->p < p->end && (*p->p == 'b' || *p->p == 'B')) {
        char k = *p->p++;
        return mk(p, k == 'b' ? N_WORDB : N_NWORDB);
      }
      int v = parse_escape(p, &named);
      if (v == -2) return NULL;
      if (v == -1) {
        ClassSet s;
        memset(s, 0, 32);
        add_named(s, named);
        Node* n = mk(p, N_CLASS);
        if (!n) return NULL;
        n->c = new_class(p, s);
        return n;
      }
      c = (char)v;
      break;
    }
    case '*': case '+': case '?':
      perr(p, "nothing to repeat");
      return NULL;
    default: break;
  }
  if ((p->flags & REGEX_FLAG_ICASE) && lower((unsigned char)c) != upper((unsigned char)c)) {
    ClassSet s;
    memset(s, 0, 32);
    set_bit(s, lower((unsigned char)c));
    set_bit(s, upper((unsigned char)c));
    Node* n = mk(p, N_CLASS);
    if (!n) return NULL;
    n->c = new_class(p, s);
    return n;
  }
  Node* n = mk(p, N_CHAR);
  if (n) n->c = (unsigned char)c;
  return n;
}

static int parse_int(P* p, int* out) {
  if (p->p >= p->end || *p->p < '0' || *p->p > '9') return 0;
  long v = 0;
  while (p->p < p->end && *p->p >= '0' && *p->p <= '9') {
    v = v * 10 + (*p->p++ - '0');
    if (v > 100000) v = 100000;
  }
  *out = (int)v;
  return 1;
}

static Node* parse_repeat(P* p) {
  Node* atom = parse_atom(p);
  if (!atom) return NULL;
  for (;;) {
    if (p->p >= p->end) break;
    int min, max;
    char c = *p->p;
    if (c == '*') { min = 0; max = -1; p->p++; }
    else if (c == '+') { min = 1; max = -1; p->p++; }
    else if (c == '?') { min = 0; max = 1; p->p++; }
    else if (c == '{') {
      const char* save = p->p;
      p->p++;
      int a, b;
      if (!parse_int(p, &a)) {
        p->p = save; /* literal '{' */
        break;
      }
      min = a;
      max = a;
      if (p->p < p->end && *p->p == ',') {
        p->p++;
        if (parse_int(p, &b)) max = b;
        else max = -1;
      }
      if (p->p >= p->end || *p->p != '}') {
        p->p = save;
        break;
      }
      p->p++;
      if (min > MAX_REPEAT || max > MAX_REPEAT) {
        perr(p, "repeat count too large");
        return NULL;
      }
      if (max >= 0 && max < min) {
        perr(p, "invalid repeat range");
        return NULL;
      }
    } else break;
    Node* r = mk(p, N_REPEAT);
    if (!r) return NULL;
    r->a = atom;
    r->min = min;
    r->max = max;
    if (p->p < p->end && *p->p == '?') {
      r->lazy = 1;
      p->p++;
    }
    atom = r;
  }
  return atom;
}

static Node* parse_cat(P* p) {
  Node* left = NULL;
  while (p->p < p->end && *p->p != '|' && *p->p != ')') {
    Node* r = parse_repeat(p);
    if (!r) return NULL;
    if (!left) left = r;
    else {
      Node* c = mk(p, N_CAT);
      if (!c) return NULL;
      c->a = left;
      c->b = r;
      left = c;
    }
  }
  return left ? left : mk(p, N_EMPTY);
}

static Node* parse_alt(P* p) {
  Node* left = parse_cat(p);
  if (!left) return NULL;
  while (p->p < p->end && *p->p == '|') {
    p->p++;
    Node* right = parse_cat(p);
    if (!right) return NULL;
    Node* a = mk(p, N_ALT);
    if (!a) return NULL;
    a->a = left;
    a->b = right;
    left = a;
  }
  return left;
}

/* ------------------------------------------------------------- codegen -- */
static int emit(P* p, int op, int x, int y) {
  Regex* re = p->re;
  if (re->n >= MAX_PROG) {
    perr(p, "pattern too large");
    return -1;
  }
  if (re->n == re->cap) {
    int nc = re->cap ? re->cap * 2 : 64;
    Inst* t = (Inst*)realloc(re->prog, sizeof(Inst) * (size_t)nc);
    if (!t) {
      perr(p, "out of memory");
      return -1;
    }
    re->prog = t;
    re->cap = nc;
  }
  re->prog[re->n].op = (uint8_t)op;
  re->prog[re->n].x = x;
  re->prog[re->n].y = y;
  return re->n++;
}

static void gen(P* p, Node* n) {
  if (p->failed || !n) return;
  Regex* re = p->re;
  switch (n->type) {
    case N_CHAR: emit(p, I_CHAR, n->c, 0); break;
    case N_ANY: emit(p, I_ANY, 0, 0); break;
    case N_CLASS: emit(p, I_CLASS, n->c, 0); break;
    case N_BOL: emit(p, I_BOL, 0, 0); break;
    case N_EOL: emit(p, I_EOL, 0, 0); break;
    case N_WORDB: emit(p, I_WORDB, 0, 0); break;
    case N_NWORDB: emit(p, I_NWORDB, 0, 0); break;
    case N_EMPTY: break;
    case N_CAT: gen(p, n->a); gen(p, n->b); break;
    case N_ALT: {
      int split = emit(p, I_SPLIT, 0, 0);
      if (split < 0) return;
      re->prog[split].x = re->n;
      gen(p, n->a);
      int jmp = emit(p, I_JMP, 0, 0);
      if (jmp < 0) return;
      re->prog[split].y = re->n;
      gen(p, n->b);
      re->prog[jmp].x = re->n;
      break;
    }
    case N_GROUP:
      emit(p, I_SAVE, 2 * n->c, 0);
      gen(p, n->a);
      emit(p, I_SAVE, 2 * n->c + 1, 0);
      break;
    case N_REPEAT: {
      for (int i = 0; i < n->min && !p->failed; i++) gen(p, n->a);
      if (n->max < 0) { /* star */
        int split = emit(p, I_SPLIT, 0, 0);
        if (split < 0) return;
        int body = re->n;
        gen(p, n->a);
        emit(p, I_JMP, split, 0);
        int out = re->n;
        re->prog[split].x = n->lazy ? out : body;
        re->prog[split].y = n->lazy ? body : out;
      } else {
        int optional = n->max - n->min;
        int* splits = (int*)malloc(sizeof(int) * (size_t)(optional > 0 ? optional : 1));
        if (!splits) {
          perr(p, "out of memory");
          return;
        }
        for (int i = 0; i < optional && !p->failed; i++) {
          splits[i] = emit(p, I_SPLIT, 0, 0);
          if (splits[i] < 0) break;
          int body = re->n;
          re->prog[splits[i]].x = body; /* fixed below for lazy */
          gen(p, n->a);
        }
        if (!p->failed) {
          int out = re->n;
          for (int i = 0; i < optional; i++) {
            int body = splits[i] + 1;
            re->prog[splits[i]].x = n->lazy ? out : body;
            re->prog[splits[i]].y = n->lazy ? body : out;
          }
        }
        free(splits);
      }
      break;
    }
  }
}

void regex_free(Regex* re) {
  if (!re) return;
  free(re->prog);
  free(re->classes);
  free(re);
}

Regex* regex_compile(const char* pat, size_t len, int flags, char* err, size_t errsz) {
  Regex* re = (Regex*)calloc(1, sizeof(Regex));
  if (!re) return NULL;
  P p;
  memset(&p, 0, sizeof p);
  p.p = pat;
  p.end = pat + len;
  p.flags = flags;
  p.re = re;
  p.err = err;
  p.errsz = errsz;
  if (errsz) err[0] = 0;
  Node* root = parse_alt(&p);
  if (!p.failed && p.p < p.end) perr(&p, "unmatched ')'");
  if (!p.failed) {
    emit(&p, I_SAVE, 0, 0);
    gen(&p, root);
    emit(&p, I_SAVE, 1, 0);
    emit(&p, I_MATCH, 0, 0);
  }
  for (Node* n = p.nodes; n;) {
    Node* nx = n->nextAlloc;
    free(n);
    n = nx;
  }
  if (p.failed) {
    regex_free(re);
    return NULL;
  }
  return re;
}

int regex_group_count(const Regex* re) { return re->ngroups; }

/* -------------------------------------------------------------- matcher -- */
typedef struct {
  int kind; /* 0 branch, 1 restore capture */
  int a, b;
} BT;

int regex_match_at(const Regex* re, const char* s, size_t n, size_t pos, int* caps, int maxcaps) {
  int cap[2 * (REGEX_MAX_GROUPS + 1)];
  int ncap = 2 * (re->ngroups + 1);
  for (int i = 0; i < ncap; i++) cap[i] = -1;
  size_t bcap = 64, bn = 0;
  BT* bt = (BT*)malloc(sizeof(BT) * bcap);
  if (!bt) return -1;
  long steps = 0;
  int pc = 0;
  size_t sp = pos;
  int result = -1;
  for (;;) {
    if (++steps > MAX_STEPS) break;
    const Inst* in = &re->prog[pc];
    int ok = 1;
    switch (in->op) {
      case I_CHAR:
        if (sp < n && (unsigned char)s[sp] == in->x) { sp++; pc++; } else ok = 0;
        break;
      case I_ANY:
        if (sp < n && s[sp] != '\n') { sp++; pc++; } else ok = 0;
        break;
      case I_CLASS:
        if (sp < n && has_bit(re->classes[in->x], (unsigned char)s[sp])) { sp++; pc++; } else ok = 0;
        break;
      case I_SPLIT:
        if (bn == bcap) {
          if (bcap >= MAX_BT) { ok = 0; goto fail_limit; }
          bcap *= 2;
          BT* t = (BT*)realloc(bt, sizeof(BT) * bcap);
          if (!t) { ok = 0; goto fail_limit; }
          bt = t;
        }
        bt[bn].kind = 0;
        bt[bn].a = in->y;
        bt[bn].b = (int)sp;
        bn++;
        pc = in->x;
        break;
      case I_JMP: pc = in->x; break;
      case I_SAVE:
        if (bn == bcap) {
          if (bcap >= MAX_BT) { ok = 0; goto fail_limit; }
          bcap *= 2;
          BT* t = (BT*)realloc(bt, sizeof(BT) * bcap);
          if (!t) { ok = 0; goto fail_limit; }
          bt = t;
        }
        bt[bn].kind = 1;
        bt[bn].a = in->x;
        bt[bn].b = cap[in->x];
        bn++;
        cap[in->x] = (int)sp;
        pc++;
        break;
      case I_BOL:
        if (sp == 0 || s[sp - 1] == '\n') pc++; else ok = 0;
        break;
      case I_EOL:
        if (sp == n || s[sp] == '\n') pc++; else ok = 0;
        break;
      case I_WORDB:
      case I_NWORDB: {
        int a = sp > 0 && is_word_byte((unsigned char)s[sp - 1]);
        int b = sp < n && is_word_byte((unsigned char)s[sp]);
        if ((a != b) == (in->op == I_WORDB)) pc++; else ok = 0;
        break;
      }
      case I_MATCH:
        result = (int)(sp - pos);
        if (caps)
          for (int i = 0; i < maxcaps && i < 2 * (REGEX_MAX_GROUPS + 1); i++) caps[i] = i < ncap ? cap[i] : -1;
        goto done;
    }
    if (!ok) {
      for (;;) {
        if (bn == 0) goto done;
        BT e = bt[--bn];
        if (e.kind == 1) {
          cap[e.a] = e.b;
          continue;
        }
        pc = e.a;
        sp = (size_t)e.b;
        break;
      }
    }
  }
fail_limit:
done:
  free(bt);
  return result;
}

int regex_search(const Regex* re, const char* text, size_t n, size_t from, int* len, int* caps, int maxcaps) {
  for (size_t i = from; i <= n; i++) {
    int m = regex_match_at(re, text, n, i, caps, maxcaps);
    if (m >= 0) {
      if (len) *len = m;
      return (int)i;
    }
  }
  return -1;
}
