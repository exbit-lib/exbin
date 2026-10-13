/* EXBIN internal: source scanner. */
#ifndef EXBIN_SCANNER_H
#define EXBIN_SCANNER_H

typedef enum {
  T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACKET, T_RBRACKET,
  T_COMMA, T_DOT, T_COLON, T_SEMI,
  T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_POW,
  T_AMP, T_PIPE, T_CARET, T_TILDE, T_SHL, T_SHR,
  T_BANG, T_BANG_EQ, T_EQ, T_EQ_EQ, T_GT, T_GE, T_LT, T_LE,
  T_ANDAND, T_OROR,
  T_PLUS_EQ, T_MINUS_EQ, T_STAR_EQ, T_SLASH_EQ, T_PERCENT_EQ,
  T_IDENT, T_STRING, T_NUMBER,
  T_LET, T_FN, T_IF, T_ELSE, T_WHILE, T_FOR, T_IN, T_RETURN, T_BREAK,
  T_CONTINUE, T_NIL, T_TRUE, T_FALSE,
  T_ERROR, T_EOF
} TokenType;

typedef struct {
  TokenType type;
  const char* start;
  int length;
  int line;
  int nl; /* 1 if a newline separates this token from the previous one */
} Token;

typedef struct {
  const char* start;
  const char* current;
  const char* end;
  int line;
  int nl;
} Scanner;

void scanner_init(Scanner* s, const char* source);
Token scanner_next(Scanner* s);

#endif
