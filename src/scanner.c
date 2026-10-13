/* EXBIN: scanner. Identifiers may contain UTF-8 (e.g. Arabic) bytes. */
#include "scanner.h"

#include <string.h>

void scanner_init(Scanner* s, const char* source) {
  s->start = source;
  s->current = source;
  s->end = source + strlen(source);
  s->line = 1;
  s->nl = 0;
  /* shebang */
  if (s->end - source >= 2 && source[0] == '#' && source[1] == '!') {
    while (s->current < s->end && *s->current != '\n') s->current++;
  }
}

static int at_end(Scanner* s) { return s->current >= s->end; }
static char peek(Scanner* s) { return at_end(s) ? '\0' : *s->current; }
static char peek_next(Scanner* s) { return (s->current + 1 >= s->end) ? '\0' : s->current[1]; }
static char advance(Scanner* s) { return *s->current++; }
static int match(Scanner* s, char c) {
  if (at_end(s) || *s->current != c) return 0;
  s->current++;
  return 1;
}

static Token make(Scanner* s, TokenType t) {
  Token tk;
  tk.type = t;
  tk.start = s->start;
  tk.length = (int)(s->current - s->start);
  tk.line = s->line;
  tk.nl = s->nl;
  return tk;
}
static Token error_tok(Scanner* s, const char* msg) {
  Token tk;
  tk.type = T_ERROR;
  tk.start = msg;
  tk.length = (int)strlen(msg);
  tk.line = s->line;
  tk.nl = s->nl;
  return tk;
}

static int is_alpha(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || ((unsigned char)c >= 0x80);
}
static int is_digit(char c) { return c >= '0' && c <= '9'; }
static int is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

static int skip_ws(Scanner* s) { /* returns 1 on unterminated comment */
  for (;;) {
    char c = peek(s);
    switch (c) {
      case ' ':
      case '\r':
      case '\t': s->current++; break;
      case '\n':
        s->line++;
        s->nl = 1;
        s->current++;
        break;
      case '/':
        if (peek_next(s) == '/') {
          while (!at_end(s) && peek(s) != '\n') s->current++;
        } else if (peek_next(s) == '*') {
          s->current += 2;
          for (;;) {
            if (at_end(s)) return 1;
            if (peek(s) == '*' && peek_next(s) == '/') {
              s->current += 2;
              break;
            }
            if (peek(s) == '\n') {
              s->line++;
              s->nl = 1;
            }
            s->current++;
          }
        } else {
          return 0;
        }
        break;
      default: return 0;
    }
  }
}

static TokenType check_kw(Scanner* s, const char* kw, TokenType t) {
  size_t n = strlen(kw);
  if ((size_t)(s->current - s->start) == n && memcmp(s->start, kw, n) == 0) return t;
  return T_IDENT;
}

static TokenType ident_type(Scanner* s) {
  size_t n = (size_t)(s->current - s->start);
  TokenType t;
  switch (s->start[0]) {
    case 'a': return check_kw(s, "and", T_ANDAND);
    case 'b': return check_kw(s, "break", T_BREAK);
    case 'c': return check_kw(s, "continue", T_CONTINUE);
    case 'e': return check_kw(s, "else", T_ELSE);
    case 'f':
      if (n == 2) return check_kw(s, "fn", T_FN);
      if ((t = check_kw(s, "for", T_FOR)) != T_IDENT) return t;
      return check_kw(s, "false", T_FALSE);
    case 'i':
      if ((t = check_kw(s, "if", T_IF)) != T_IDENT) return t;
      return check_kw(s, "in", T_IN);
    case 'l': return check_kw(s, "let", T_LET);
    case 'n':
      if ((t = check_kw(s, "nil", T_NIL)) != T_IDENT) return t;
      return check_kw(s, "not", T_BANG);
    case 'o': return check_kw(s, "or", T_OROR);
    case 'r': return check_kw(s, "return", T_RETURN);
    case 't': return check_kw(s, "true", T_TRUE);
    case 'w': return check_kw(s, "while", T_WHILE);
  }
  return T_IDENT;
}

static Token number(Scanner* s) {
  if (s->current[-1] == '0' && (peek(s) == 'x' || peek(s) == 'X')) {
    advance(s);
    if (!is_hex(peek(s))) return error_tok(s, "Expected hex digits after '0x'.");
    while (is_hex(peek(s)) || peek(s) == '_') advance(s);
    return make(s, T_NUMBER);
  }
  if (s->current[-1] == '0' && (peek(s) == 'b' || peek(s) == 'B')) {
    advance(s);
    if (peek(s) != '0' && peek(s) != '1') return error_tok(s, "Expected binary digits after '0b'.");
    while (peek(s) == '0' || peek(s) == '1' || peek(s) == '_') advance(s);
    return make(s, T_NUMBER);
  }
  while (is_digit(peek(s)) || peek(s) == '_') advance(s);
  if (peek(s) == '.' && is_digit(peek_next(s))) {
    advance(s);
    while (is_digit(peek(s)) || peek(s) == '_') advance(s);
  }
  if (peek(s) == 'e' || peek(s) == 'E') {
    char n1 = peek_next(s);
    const char* save = s->current;
    if (is_digit(n1) || ((n1 == '+' || n1 == '-') && s->current + 2 < s->end && is_digit(s->current[2]))) {
      advance(s);
      if (peek(s) == '+' || peek(s) == '-') advance(s);
      while (is_digit(peek(s))) advance(s);
    } else {
      s->current = save;
    }
  }
  return make(s, T_NUMBER);
}

static Token string(Scanner* s, char quote) {
  while (!at_end(s) && peek(s) != quote) {
    if (peek(s) == '\n') s->line++;
    if (peek(s) == '\\' && s->current + 1 < s->end) {
      if (s->current[1] == '\n') s->line++;
      s->current++;
    }
    s->current++;
  }
  if (at_end(s)) return error_tok(s, "Unterminated string.");
  advance(s);
  return make(s, T_STRING);
}

Token scanner_next(Scanner* s) {
  s->nl = 0;
  if (skip_ws(s)) return error_tok(s, "Unterminated block comment.");
  s->start = s->current;
  if (at_end(s)) return make(s, T_EOF);
  char c = advance(s);
  if (is_alpha(c)) {
    while (is_alpha(peek(s)) || is_digit(peek(s))) advance(s);
    return make(s, ident_type(s));
  }
  if (is_digit(c)) return number(s);
  switch (c) {
    case '(': return make(s, T_LPAREN);
    case ')': return make(s, T_RPAREN);
    case '{': return make(s, T_LBRACE);
    case '}': return make(s, T_RBRACE);
    case '[': return make(s, T_LBRACKET);
    case ']': return make(s, T_RBRACKET);
    case ',': return make(s, T_COMMA);
    case '.': return make(s, T_DOT);
    case ':': return make(s, T_COLON);
    case ';': return make(s, T_SEMI);
    case '+': return make(s, match(s, '=') ? T_PLUS_EQ : T_PLUS);
    case '-': return make(s, match(s, '=') ? T_MINUS_EQ : T_MINUS);
    case '*':
      if (match(s, '*')) return make(s, T_POW);
      return make(s, match(s, '=') ? T_STAR_EQ : T_STAR);
    case '/': return make(s, match(s, '=') ? T_SLASH_EQ : T_SLASH);
    case '%': return make(s, match(s, '=') ? T_PERCENT_EQ : T_PERCENT);
    case '&': return make(s, match(s, '&') ? T_ANDAND : T_AMP);
    case '|': return make(s, match(s, '|') ? T_OROR : T_PIPE);
    case '^': return make(s, T_CARET);
    case '~': return make(s, T_TILDE);
    case '!': return make(s, match(s, '=') ? T_BANG_EQ : T_BANG);
    case '=': return make(s, match(s, '=') ? T_EQ_EQ : T_EQ);
    case '<':
      if (match(s, '<')) return make(s, T_SHL);
      return make(s, match(s, '=') ? T_LE : T_LT);
    case '>':
      if (match(s, '>')) return make(s, T_SHR);
      return make(s, match(s, '=') ? T_GE : T_GT);
    case '"': return string(s, '"');
    case '\'': return string(s, '\'');
  }
  return error_tok(s, "Unexpected character.");
}
