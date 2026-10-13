/* EXBIN: single-pass Pratt compiler (source -> bytecode). */
#include <ctype.h>

#include "scanner.h"
#include "vm.h"

typedef struct {
  Token current, previous;
  Scanner scanner;
  bool hadError, panicMode;
  VM* vm;
  ObjString* file;
  Buf err;
  int depth;
} Parser;

typedef enum {
  PREC_NONE,
  PREC_ASSIGNMENT,
  PREC_OR,
  PREC_AND,
  PREC_EQUALITY,
  PREC_COMPARISON,
  PREC_BOR,
  PREC_BXOR,
  PREC_BAND,
  PREC_SHIFT,
  PREC_TERM,
  PREC_FACTOR,
  PREC_UNARY,
  PREC_POWER,
  PREC_CALL,
  PREC_PRIMARY
} Precedence;

typedef void (*ParseFn)(bool canAssign);
typedef struct {
  ParseFn prefix, infix;
  Precedence precedence;
} ParseRule;

typedef struct {
  Token name;
  int depth;
  bool isCaptured;
} Local;
typedef struct {
  uint8_t index;
  bool isLocal;
} UpvalueRef;
typedef enum { FT_FUNCTION, FT_SCRIPT } FnType;

#define MAX_BREAKS 256
typedef struct LoopCtx {
  struct LoopCtx* enclosing;
  int start;
  int depth;
  int breaks[MAX_BREAKS];
  int breakCount;
} LoopCtx;

typedef struct Compiler {
  struct Compiler* enclosing;
  ObjFunction* function;
  FnType type;
  Local locals[256];
  int localCount;
  UpvalueRef upvalues[256];
  int scopeDepth;
  LoopCtx* loop;
} Compiler;

static _Thread_local Parser parser;
static _Thread_local Compiler* current = NULL;

static void expression(void);
static void declaration(void);
static void statement(void);
static void block(void);
static void function(FnType type, const char* name, int nameLen);
static ParseRule* get_rule(TokenType t);
static void parse_precedence(Precedence p);

/* -------------------------------------------------------------- errors --- */
static void error_at(Token* tok, const char* msg) {
  if (parser.panicMode) return;
  parser.panicMode = true;
  buf_addf(&parser.err, "[%s:%d] Error", parser.file->chars, tok->line);
  if (tok->type == T_EOF) buf_adds(&parser.err, " at end");
  else if (tok->type != T_ERROR) buf_addf(&parser.err, " at '%.*s'", tok->length, tok->start);
  buf_addf(&parser.err, ": %s\n", msg);
  parser.hadError = true;
}
static void error(const char* msg) { error_at(&parser.previous, msg); }
static void error_at_current(const char* msg) { error_at(&parser.current, msg); }

static void advance(void) {
  parser.previous = parser.current;
  for (;;) {
    parser.current = scanner_next(&parser.scanner);
    if (parser.current.type != T_ERROR) break;
    error_at_current(parser.current.start);
  }
}
static bool check(TokenType t) { return parser.current.type == t; }
static bool match(TokenType t) {
  if (!check(t)) return false;
  advance();
  return true;
}
static void consume(TokenType t, const char* msg) {
  if (check(t)) {
    advance();
    return;
  }
  error_at_current(msg);
}
static Token peek_token(void) {
  Scanner s = parser.scanner;
  return scanner_next(&s);
}
static bool is_name_token(const Token* t) {
  if (t->type == T_STRING || t->type == T_NUMBER || t->type == T_ERROR || t->type == T_EOF) return false;
  unsigned char c = (unsigned char)t->start[0];
  return isalpha(c) || c == '_' || c >= 0x80;
}

/* ------------------------------------------------------------ emission --- */
static Chunk* cur_chunk(void) { return &current->function->chunk; }
static void emit_byte(int b) { chunk_write(parser.vm, cur_chunk(), (uint8_t)b, parser.previous.line); }
static void emit_op(int op) { emit_byte(op); }
static void emit_u16(int v) {
  emit_byte((v >> 8) & 0xff);
  emit_byte(v & 0xff);
}
static int make_constant(Value v) {
  int idx = chunk_add_const(parser.vm, cur_chunk(), v);
  if (idx > 0xffff) {
    error("Too many constants in one function.");
    return 0;
  }
  return idx;
}
static void emit_constant(Value v) {
  emit_op(OP_CONSTANT);
  emit_u16(make_constant(v));
}
static int emit_jump(int op) {
  emit_op(op);
  emit_byte(0xff);
  emit_byte(0xff);
  return cur_chunk()->count - 2;
}
static void patch_jump(int offset) {
  int jump = cur_chunk()->count - offset - 2;
  if (jump > 0xffff) error("Too much code to jump over.");
  cur_chunk()->code[offset] = (uint8_t)((jump >> 8) & 0xff);
  cur_chunk()->code[offset + 1] = (uint8_t)(jump & 0xff);
}
static void emit_loop(int loopStart) {
  emit_op(OP_LOOP);
  int offset = cur_chunk()->count - loopStart + 2;
  if (offset > 0xffff) error("Loop body too large.");
  emit_u16(offset);
}
static void emit_return(void) {
  emit_op(OP_NIL);
  emit_op(OP_RETURN);
}
static int identifier_constant(Token* name) {
  return make_constant(OBJ_VAL(str_copy(parser.vm, name->start, (size_t)name->length)));
}

/* ------------------------------------------------------------ compiler --- */
static void compiler_init(Compiler* c, FnType type, const char* name, int nameLen) {
  c->enclosing = current;
  c->type = type;
  c->localCount = 0;
  c->scopeDepth = 0;
  c->loop = NULL;
  c->function = fn_new(parser.vm);
  c->function->file = parser.file;
  if (type == FT_FUNCTION) c->function->name = str_copy(parser.vm, name, (size_t)nameLen);
  current = c;
  Local* l = &current->locals[current->localCount++];
  l->depth = 0;
  l->isCaptured = false;
  l->name.start = "";
  l->name.length = 0;
}
static ObjFunction* end_compiler(void) {
  emit_return();
  ObjFunction* fn = current->function;
  current = current->enclosing;
  return fn;
}
static void begin_scope(void) { current->scopeDepth++; }
static void end_scope(void) {
  current->scopeDepth--;
  while (current->localCount > 0 && current->locals[current->localCount - 1].depth > current->scopeDepth) {
    if (current->locals[current->localCount - 1].isCaptured) emit_op(OP_CLOSE_UPVALUE);
    else emit_op(OP_POP);
    current->localCount--;
  }
}
/* pops locals deeper than `depth` WITHOUT forgetting them (for break/continue) */
static void emit_pops_to(int depth) {
  for (int i = current->localCount - 1; i >= 0 && current->locals[i].depth > depth; i--) {
    if (current->locals[i].isCaptured) emit_op(OP_CLOSE_UPVALUE);
    else emit_op(OP_POP);
  }
}

static bool ident_eq(const Token* a, const Token* b) {
  return a->length == b->length && memcmp(a->start, b->start, (size_t)a->length) == 0;
}
static int resolve_local(Compiler* c, Token* name) {
  for (int i = c->localCount - 1; i >= 0; i--) {
    Local* l = &c->locals[i];
    if (ident_eq(name, &l->name)) {
      if (l->depth == -1) error("Can't read a local variable in its own initializer.");
      return i;
    }
  }
  return -1;
}
static int add_upvalue(Compiler* c, uint8_t index, bool isLocal) {
  int n = c->function->upvalueCount;
  for (int i = 0; i < n; i++)
    if (c->upvalues[i].index == index && c->upvalues[i].isLocal == isLocal) return i;
  if (n == 256) {
    error("Too many closure variables in function.");
    return 0;
  }
  c->upvalues[n].isLocal = isLocal;
  c->upvalues[n].index = index;
  return c->function->upvalueCount++;
}
static int resolve_upvalue(Compiler* c, Token* name) {
  if (c->enclosing == NULL) return -1;
  int local = resolve_local(c->enclosing, name);
  if (local != -1) {
    c->enclosing->locals[local].isCaptured = true;
    return add_upvalue(c, (uint8_t)local, true);
  }
  int up = resolve_upvalue(c->enclosing, name);
  if (up != -1) return add_upvalue(c, (uint8_t)up, false);
  return -1;
}
static void add_local(Token name) {
  if (current->localCount == 256) {
    error("Too many local variables in function.");
    return;
  }
  Local* l = &current->locals[current->localCount++];
  l->name = name;
  l->depth = -1;
  l->isCaptured = false;
}
static void mark_initialized(void) {
  if (current->scopeDepth == 0) return;
  current->locals[current->localCount - 1].depth = current->scopeDepth;
}
static void add_hidden_local(const char* name) {
  Token t;
  t.type = T_IDENT;
  t.start = name;
  t.length = (int)strlen(name);
  t.line = parser.previous.line;
  t.nl = 0;
  add_local(t);
  mark_initialized();
}
static void declare_variable(void) {
  if (current->scopeDepth == 0) return;
  Token* name = &parser.previous;
  for (int i = current->localCount - 1; i >= 0; i--) {
    Local* l = &current->locals[i];
    if (l->depth != -1 && l->depth < current->scopeDepth) break;
    if (ident_eq(name, &l->name)) error("A variable with this name already exists in this scope.");
  }
  add_local(*name);
}
static int parse_variable(const char* msg) {
  consume(T_IDENT, msg);
  declare_variable();
  if (current->scopeDepth > 0) return 0;
  return identifier_constant(&parser.previous);
}
static void define_variable(int global) {
  if (current->scopeDepth > 0) {
    mark_initialized();
    return;
  }
  emit_op(OP_DEFINE_GLOBAL);
  emit_u16(global);
}

/* --------------------------------------------------------- expressions --- */
static int argument_list(void) {
  int argc = 0;
  if (!check(T_RPAREN)) {
    do {
      if (check(T_RPAREN)) break;
      expression();
      if (argc == 255) error("Can't have more than 255 arguments.");
      argc++;
    } while (match(T_COMMA));
  }
  consume(T_RPAREN, "Expect ')' after arguments.");
  return argc;
}

static int compound_op(TokenType t) {
  switch (t) {
    case T_PLUS_EQ: return OP_ADD;
    case T_MINUS_EQ: return OP_SUB;
    case T_STAR_EQ: return OP_MUL;
    case T_SLASH_EQ: return OP_DIV;
    case T_PERCENT_EQ: return OP_MOD;
    default: return -1;
  }
}

static void emit_var(int op, int arg, bool global) {
  emit_op(op);
  if (global) emit_u16(arg);
  else emit_byte(arg);
}

static void named_variable(Token name, bool canAssign) {
  int getOp, setOp;
  bool global = false;
  int arg = resolve_local(current, &name);
  if (arg != -1) {
    getOp = OP_GET_LOCAL;
    setOp = OP_SET_LOCAL;
  } else if ((arg = resolve_upvalue(current, &name)) != -1) {
    getOp = OP_GET_UPVALUE;
    setOp = OP_SET_UPVALUE;
  } else {
    arg = identifier_constant(&name);
    getOp = OP_GET_GLOBAL;
    setOp = OP_SET_GLOBAL;
    global = true;
  }
  if (canAssign && match(T_EQ)) {
    expression();
    emit_var(setOp, arg, global);
  } else if (canAssign && compound_op(parser.current.type) >= 0) {
    int op = compound_op(parser.current.type);
    advance();
    emit_var(getOp, arg, global);
    expression();
    emit_op(op);
    emit_var(setOp, arg, global);
  } else {
    emit_var(getOp, arg, global);
  }
}

static void variable(bool canAssign) { named_variable(parser.previous, canAssign); }

static void grouping(bool canAssign) {
  (void)canAssign;
  expression();
  consume(T_RPAREN, "Expect ')' after expression.");
}

static void number(bool canAssign) {
  (void)canAssign;
  char tmp[128];
  int n = 0;
  for (int i = 0; i < parser.previous.length && n < 126; i++)
    if (parser.previous.start[i] != '_') tmp[n++] = parser.previous.start[i];
  tmp[n] = 0;
  double v;
  if (n > 2 && tmp[0] == '0' && (tmp[1] == 'x' || tmp[1] == 'X')) v = (double)strtoull(tmp + 2, NULL, 16);
  else if (n > 2 && tmp[0] == '0' && (tmp[1] == 'b' || tmp[1] == 'B')) v = (double)strtoull(tmp + 2, NULL, 2);
  else v = strtod(tmp, NULL);
  emit_constant(NUM_VAL(v));
}

static void utf8_encode(Buf* b, unsigned long cp) {
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
static int hexval(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static void string_lit(bool canAssign) {
  (void)canAssign;
  const char* s = parser.previous.start + 1;
  int n = parser.previous.length - 2;
  Buf b;
  buf_init(&b);
  for (int i = 0; i < n; i++) {
    char c = s[i];
    if (c != '\\') {
      buf_addc(&b, c);
      continue;
    }
    i++;
    if (i >= n) break;
    switch (s[i]) {
      case 'n': buf_addc(&b, '\n'); break;
      case 't': buf_addc(&b, '\t'); break;
      case 'r': buf_addc(&b, '\r'); break;
      case '0': buf_addc(&b, '\0'); break;
      case 'a': buf_addc(&b, '\a'); break;
      case 'b': buf_addc(&b, '\b'); break;
      case 'f': buf_addc(&b, '\f'); break;
      case 'v': buf_addc(&b, '\v'); break;
      case 'e': buf_addc(&b, 27); break;
      case '\\': buf_addc(&b, '\\'); break;
      case '"': buf_addc(&b, '"'); break;
      case '\'': buf_addc(&b, '\''); break;
      case '\n': break; /* line continuation */
      case 'x': {
        int h1 = i + 1 < n ? hexval(s[i + 1]) : -1, h2 = i + 2 < n ? hexval(s[i + 2]) : -1;
        if (h1 < 0 || h2 < 0) {
          error("Invalid \\x escape (needs two hex digits).");
        } else {
          buf_addc(&b, (char)(h1 * 16 + h2));
          i += 2;
        }
        break;
      }
      case 'u': {
        unsigned long cp = 0;
        int digits = 0;
        if (i + 1 < n && s[i + 1] == '{') {
          i += 2;
          while (i < n && s[i] != '}' && hexval(s[i]) >= 0 && digits < 8) {
            cp = cp * 16 + (unsigned long)hexval(s[i]);
            i++;
            digits++;
          }
          if (i >= n || s[i] != '}' || digits == 0) error("Invalid \\u{...} escape.");
        } else {
          while (digits < 4 && i + 1 < n && hexval(s[i + 1]) >= 0) {
            cp = cp * 16 + (unsigned long)hexval(s[i + 1]);
            i++;
            digits++;
          }
          if (digits != 4) error("Invalid \\u escape (needs 4 hex digits).");
        }
        if (cp > 0x10FFFF) error("Unicode escape out of range.");
        else utf8_encode(&b, cp);
        break;
      }
      default: error("Invalid escape sequence."); break;
    }
  }
  emit_constant(OBJ_VAL(str_copy(parser.vm, b.d ? b.d : "", b.len)));
  buf_free(&b);
}

static void literal(bool canAssign) {
  (void)canAssign;
  switch (parser.previous.type) {
    case T_NIL: emit_op(OP_NIL); break;
    case T_TRUE: emit_op(OP_TRUE); break;
    case T_FALSE: emit_op(OP_FALSE); break;
    default: break;
  }
}

static void list_lit(bool canAssign) {
  (void)canAssign;
  int count = 0;
  if (!check(T_RBRACKET)) {
    do {
      if (check(T_RBRACKET)) break;
      expression();
      if (count == 0xffff) error("Too many elements in list literal.");
      count++;
    } while (match(T_COMMA));
  }
  consume(T_RBRACKET, "Expect ']' after list elements.");
  emit_op(OP_BUILD_LIST);
  emit_u16(count);
}

static void map_lit(bool canAssign) {
  (void)canAssign;
  int count = 0;
  if (!check(T_RBRACE)) {
    do {
      if (check(T_RBRACE)) break;
      if (match(T_LBRACKET)) {
        expression();
        consume(T_RBRACKET, "Expect ']' after computed key.");
      } else if (match(T_STRING)) {
        string_lit(false);
      } else if (match(T_NUMBER)) {
        number(false);
      } else if (is_name_token(&parser.current)) {
        advance();
        emit_constant(OBJ_VAL(str_copy(parser.vm, parser.previous.start, (size_t)parser.previous.length)));
      } else {
        error_at_current("Expect map key.");
        return;
      }
      consume(T_COLON, "Expect ':' after map key.");
      expression();
      if (count == 0xffff) error("Too many entries in map literal.");
      count++;
    } while (match(T_COMMA));
  }
  consume(T_RBRACE, "Expect '}' after map entries.");
  emit_op(OP_BUILD_MAP);
  emit_u16(count);
}

static void lambda(bool canAssign) {
  (void)canAssign;
  function(FT_FUNCTION, "fn", 2);
}

static void unary(bool canAssign) {
  (void)canAssign;
  TokenType op = parser.previous.type;
  parse_precedence(PREC_UNARY);
  switch (op) {
    case T_MINUS: emit_op(OP_NEG); break;
    case T_BANG: emit_op(OP_NOT); break;
    case T_TILDE: emit_op(OP_BNOT); break;
    default: break;
  }
}

static void binary(bool canAssign) {
  (void)canAssign;
  TokenType op = parser.previous.type;
  ParseRule* rule = get_rule(op);
  /* ** is right-associative */
  parse_precedence(op == T_POW ? PREC_POWER : (Precedence)(rule->precedence + 1));
  switch (op) {
    case T_PLUS: emit_op(OP_ADD); break;
    case T_MINUS: emit_op(OP_SUB); break;
    case T_STAR: emit_op(OP_MUL); break;
    case T_SLASH: emit_op(OP_DIV); break;
    case T_PERCENT: emit_op(OP_MOD); break;
    case T_POW: emit_op(OP_POW); break;
    case T_AMP: emit_op(OP_BAND); break;
    case T_PIPE: emit_op(OP_BOR); break;
    case T_CARET: emit_op(OP_BXOR); break;
    case T_SHL: emit_op(OP_SHL); break;
    case T_SHR: emit_op(OP_SHR); break;
    case T_EQ_EQ: emit_op(OP_EQUAL); break;
    case T_BANG_EQ: emit_op(OP_NOT_EQUAL); break;
    case T_LT: emit_op(OP_LESS); break;
    case T_LE: emit_op(OP_LESS_EQUAL); break;
    case T_GT: emit_op(OP_GREATER); break;
    case T_GE: emit_op(OP_GREATER_EQUAL); break;
    default: break;
  }
}

static void and_(bool canAssign) {
  (void)canAssign;
  int endJump = emit_jump(OP_JUMP_IF_FALSE);
  emit_op(OP_POP);
  parse_precedence(PREC_AND);
  patch_jump(endJump);
}
static void or_(bool canAssign) {
  (void)canAssign;
  int endJump = emit_jump(OP_JUMP_IF_TRUE);
  emit_op(OP_POP);
  parse_precedence(PREC_OR);
  patch_jump(endJump);
}

static void call(bool canAssign) {
  (void)canAssign;
  int argc = argument_list();
  emit_op(OP_CALL);
  emit_byte(argc);
}

static void dot(bool canAssign) {
  if (!is_name_token(&parser.current)) {
    error_at_current("Expect property name after '.'.");
    return;
  }
  advance();
  int name = identifier_constant(&parser.previous);
  int cop;
  if (canAssign && match(T_EQ)) {
    expression();
    emit_op(OP_SET_FIELD);
    emit_u16(name);
  } else if (canAssign && (cop = compound_op(parser.current.type)) >= 0) {
    advance();
    emit_op(OP_DUP);
    emit_op(OP_GET_FIELD);
    emit_u16(name);
    expression();
    emit_op(cop);
    emit_op(OP_SET_FIELD);
    emit_u16(name);
  } else if (check(T_LPAREN) && !parser.current.nl) {
    advance();
    int argc = argument_list();
    emit_op(OP_INVOKE);
    emit_u16(name);
    emit_byte(argc);
  } else {
    emit_op(OP_GET_FIELD);
    emit_u16(name);
  }
}

static void subscript(bool canAssign) {
  expression();
  consume(T_RBRACKET, "Expect ']' after index.");
  int cop;
  if (canAssign && match(T_EQ)) {
    expression();
    emit_op(OP_SET_INDEX);
  } else if (canAssign && (cop = compound_op(parser.current.type)) >= 0) {
    advance();
    emit_op(OP_DUP2);
    emit_op(OP_GET_INDEX);
    expression();
    emit_op(cop);
    emit_op(OP_SET_INDEX);
  } else {
    emit_op(OP_GET_INDEX);
  }
}

static ParseRule rules[T_EOF + 1] = {
    [T_LPAREN] = {grouping, call, PREC_CALL},
    [T_RPAREN] = {NULL, NULL, PREC_NONE},
    [T_LBRACE] = {map_lit, NULL, PREC_NONE},
    [T_RBRACE] = {NULL, NULL, PREC_NONE},
    [T_LBRACKET] = {list_lit, subscript, PREC_CALL},
    [T_RBRACKET] = {NULL, NULL, PREC_NONE},
    [T_COMMA] = {NULL, NULL, PREC_NONE},
    [T_DOT] = {NULL, dot, PREC_CALL},
    [T_COLON] = {NULL, NULL, PREC_NONE},
    [T_SEMI] = {NULL, NULL, PREC_NONE},
    [T_PLUS] = {NULL, binary, PREC_TERM},
    [T_MINUS] = {unary, binary, PREC_TERM},
    [T_STAR] = {NULL, binary, PREC_FACTOR},
    [T_SLASH] = {NULL, binary, PREC_FACTOR},
    [T_PERCENT] = {NULL, binary, PREC_FACTOR},
    [T_POW] = {NULL, binary, PREC_POWER},
    [T_AMP] = {NULL, binary, PREC_BAND},
    [T_PIPE] = {NULL, binary, PREC_BOR},
    [T_CARET] = {NULL, binary, PREC_BXOR},
    [T_TILDE] = {unary, NULL, PREC_NONE},
    [T_SHL] = {NULL, binary, PREC_SHIFT},
    [T_SHR] = {NULL, binary, PREC_SHIFT},
    [T_BANG] = {unary, NULL, PREC_NONE},
    [T_BANG_EQ] = {NULL, binary, PREC_EQUALITY},
    [T_EQ] = {NULL, NULL, PREC_NONE},
    [T_EQ_EQ] = {NULL, binary, PREC_EQUALITY},
    [T_GT] = {NULL, binary, PREC_COMPARISON},
    [T_GE] = {NULL, binary, PREC_COMPARISON},
    [T_LT] = {NULL, binary, PREC_COMPARISON},
    [T_LE] = {NULL, binary, PREC_COMPARISON},
    [T_ANDAND] = {NULL, and_, PREC_AND},
    [T_OROR] = {NULL, or_, PREC_OR},
    [T_IDENT] = {variable, NULL, PREC_NONE},
    [T_STRING] = {string_lit, NULL, PREC_NONE},
    [T_NUMBER] = {number, NULL, PREC_NONE},
    [T_FN] = {lambda, NULL, PREC_NONE},
    [T_NIL] = {literal, NULL, PREC_NONE},
    [T_TRUE] = {literal, NULL, PREC_NONE},
    [T_FALSE] = {literal, NULL, PREC_NONE},
};
static ParseRule* get_rule(TokenType t) { return &rules[t]; }

static void parse_precedence(Precedence precedence) {
  if (++parser.depth > 200) {
    error_at_current("Expression nested too deeply.");
    parser.depth--;
    return;
  }
  advance();
  ParseFn prefix = get_rule(parser.previous.type)->prefix;
  if (prefix == NULL) {
    error("Expect expression.");
    parser.depth--;
    return;
  }
  bool canAssign = precedence <= PREC_ASSIGNMENT;
  prefix(canAssign);
  while (precedence <= get_rule(parser.current.type)->precedence) {
    TokenType ct = parser.current.type;
    /* a line starting with ( [ or - begins a new statement */
    if (parser.current.nl && (ct == T_LPAREN || ct == T_LBRACKET || ct == T_MINUS)) break;
    advance();
    ParseFn infix = get_rule(parser.previous.type)->infix;
    infix(canAssign);
  }
  if (canAssign && (check(T_EQ) || compound_op(parser.current.type) >= 0)) {
    advance();
    error("Invalid assignment target.");
  }
  parser.depth--;
}

static void expression(void) { parse_precedence(PREC_ASSIGNMENT); }

/* ---------------------------------------------------------- statements --- */
static void block(void) {
  while (!check(T_RBRACE) && !check(T_EOF)) declaration();
  consume(T_RBRACE, "Expect '}' after block.");
}

static void function(FnType type, const char* name, int nameLen) {
  Compiler compiler;
  compiler_init(&compiler, type, name, nameLen);
  begin_scope();
  consume(T_LPAREN, "Expect '(' after function name.");
  if (!check(T_RPAREN)) {
    do {
      if (check(T_RPAREN)) break;
      current->function->arity++;
      if (current->function->arity > 255) error_at_current("Can't have more than 255 parameters.");
      int c = parse_variable("Expect parameter name.");
      define_variable(c);
    } while (match(T_COMMA));
  }
  consume(T_RPAREN, "Expect ')' after parameters.");
  consume(T_LBRACE, "Expect '{' before function body.");
  block();
  ObjFunction* fn = end_compiler();
  emit_op(OP_CLOSURE);
  emit_u16(make_constant(OBJ_VAL(fn)));
  for (int i = 0; i < fn->upvalueCount; i++) {
    emit_byte(compiler.upvalues[i].isLocal ? 1 : 0);
    emit_byte(compiler.upvalues[i].index);
  }
}

static void fn_declaration(void) {
  int global = parse_variable("Expect function name.");
  Token nameTok = parser.previous;
  mark_initialized();
  function(FT_FUNCTION, nameTok.start, nameTok.length);
  define_variable(global);
}

static void let_declaration(void) {
  int global = parse_variable("Expect variable name.");
  if (match(T_EQ)) expression();
  else emit_op(OP_NIL);
  match(T_SEMI);
  define_variable(global);
}

static void block_required(void) {
  consume(T_LBRACE, "Expect '{' before block.");
  begin_scope();
  block();
  end_scope();
}

static void expression_statement(void) {
  expression();
  match(T_SEMI);
  emit_op(OP_POP);
}

static void if_statement(void) {
  expression();
  int thenJump = emit_jump(OP_JUMP_IF_FALSE);
  emit_op(OP_POP);
  block_required();
  int elseJump = emit_jump(OP_JUMP);
  patch_jump(thenJump);
  emit_op(OP_POP);
  if (match(T_ELSE)) {
    if (match(T_IF)) if_statement();
    else block_required();
  }
  patch_jump(elseJump);
}

static void patch_breaks(LoopCtx* loop) {
  for (int i = 0; i < loop->breakCount; i++) patch_jump(loop->breaks[i]);
}

static void while_statement(void) {
  int loopStart = cur_chunk()->count;
  LoopCtx loop;
  loop.enclosing = current->loop;
  loop.start = loopStart;
  loop.depth = current->scopeDepth;
  loop.breakCount = 0;
  current->loop = &loop;
  expression();
  int exitJump = emit_jump(OP_JUMP_IF_FALSE);
  emit_op(OP_POP);
  block_required();
  emit_loop(loopStart);
  patch_jump(exitJump);
  emit_op(OP_POP);
  patch_breaks(&loop);
  current->loop = loop.enclosing;
}

static void for_statement(void) {
  begin_scope();
  consume(T_IDENT, "Expect loop variable name after 'for'.");
  Token var = parser.previous;
  consume(T_IN, "Expect 'in' after loop variable.");
  expression();
  add_hidden_local("$it");
  emit_op(OP_ITER_INIT);
  emit_constant(NUM_VAL(0));
  add_hidden_local("$i");

  int loopStart = cur_chunk()->count;
  LoopCtx loop;
  loop.enclosing = current->loop;
  loop.start = loopStart;
  loop.depth = current->scopeDepth;
  loop.breakCount = 0;
  current->loop = &loop;

  int exitJump = emit_jump(OP_FOR_NEXT);
  begin_scope();
  add_local(var);
  mark_initialized();
  consume(T_LBRACE, "Expect '{' before loop body.");
  block();
  end_scope();
  emit_loop(loopStart);
  patch_jump(exitJump);
  patch_breaks(&loop);
  current->loop = loop.enclosing;
  end_scope();
}

static void return_statement(void) {
  if (match(T_SEMI) || check(T_RBRACE) || check(T_EOF) || parser.current.nl) {
    emit_return();
  } else {
    expression();
    match(T_SEMI);
    emit_op(OP_RETURN);
  }
}

static void break_statement(void) {
  if (current->loop == NULL) {
    error("Can't use 'break' outside of a loop.");
    return;
  }
  match(T_SEMI);
  emit_pops_to(current->loop->depth);
  if (current->loop->breakCount >= MAX_BREAKS) {
    error("Too many 'break' statements in one loop.");
    return;
  }
  current->loop->breaks[current->loop->breakCount++] = emit_jump(OP_JUMP);
}

static void continue_statement(void) {
  if (current->loop == NULL) {
    error("Can't use 'continue' outside of a loop.");
    return;
  }
  match(T_SEMI);
  emit_pops_to(current->loop->depth);
  emit_loop(current->loop->start);
}

static void synchronize(void) {
  parser.panicMode = false;
  while (parser.current.type != T_EOF) {
    if (parser.previous.type == T_SEMI || parser.previous.type == T_RBRACE) return;
    switch (parser.current.type) {
      case T_LET:
      case T_FN:
      case T_IF:
      case T_WHILE:
      case T_FOR:
      case T_RETURN:
      case T_BREAK:
      case T_CONTINUE: return;
      default: break;
    }
    advance();
  }
}

static void statement(void) {
  if (match(T_IF)) if_statement();
  else if (match(T_WHILE)) while_statement();
  else if (match(T_FOR)) for_statement();
  else if (match(T_RETURN)) return_statement();
  else if (match(T_BREAK)) break_statement();
  else if (match(T_CONTINUE)) continue_statement();
  else if (match(T_LBRACE)) {
    begin_scope();
    block();
    end_scope();
  } else if (match(T_SEMI)) {
    /* empty statement */
  } else expression_statement();
}

static void declaration(void) {
  if (match(T_LET)) {
    let_declaration();
  } else if (check(T_FN) && peek_token().type == T_IDENT) {
    advance();
    fn_declaration();
  } else {
    statement();
  }
  if (parser.panicMode) synchronize();
}

ObjFunction* compile_source(VM* vm, const char* src, const char* file) {
  memset(&parser, 0, sizeof parser);
  scanner_init(&parser.scanner, src);
  parser.vm = vm;
  parser.file = str_from(vm, file ? file : "<script>");
  buf_init(&parser.err);
  current = NULL;
  Compiler compiler;
  compiler_init(&compiler, FT_SCRIPT, NULL, 0);
  advance();
  while (!match(T_EOF)) declaration();
  ObjFunction* fn = end_compiler();
  current = NULL;
  bool failed = parser.hadError;
  if (!failed) {
    char verr[200];
    if (!chunk_verify(fn, verr, sizeof verr)) {
      buf_addf(&parser.err, "[%s] Error: internal compiler error: %s\n", parser.file->chars, verr);
      failed = true;
    }
  }
  if (failed) vm_set_errmsg(vm, buf_str(&parser.err));
  buf_free(&parser.err);
  return failed ? NULL : fn;
}
