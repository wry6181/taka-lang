#pragma once
#include "ast/ast.h"
#include "parser.h"

typedef enum BindingPower {
  defalt_bp = 0,
  comma,
  assignment,
  logical,
  relational,
  additive,
  multiplicative,
  unary,
  call,
  member,
  primary
} BindingPower;

typedef Stmt* (*StmtHandler)(Parser* p);
typedef Expr* (*NudHandler)(Parser* p);
typedef Expr* (*LedHandler)(Parser* p, Expr* left, BindingPower bp);

Expr* parse_expr(Parser* p, BindingPower bp);
Expr* parse_binary_expr(Parser* p, Expr* left, BindingPower bp);
Expr* parse_primary_expr(Parser* p);

typedef struct Lookups Lookups;
struct Lookups {
  Map* bp;
  Map* nud;
  Map* led;
  Map* stmt;
};

static inline void lookup_register_led(Lookups* lu, TokenKind kind, BindingPower bp,
                                       LedHandler handler) {
  map_set(lu->bp, (u64)kind, (u64)bp);
  map_set_ptr(lu->led, (u64)kind, (void*)handler);
}

static inline void lookup_register_nud(Lookups* lu, TokenKind kind, NudHandler handler) {
  map_set(lu->bp, (u64)kind, (u64)primary);
  map_set_ptr(lu->nud, (u64)kind, (void*)handler);
}

static inline void lookup_register_stmt(Lookups* lu, TokenKind kind, StmtHandler handler) {
  map_set(lu->bp, (u64)kind, (u64)defalt_bp);
  map_set_ptr(lu->stmt, (u64)kind, (void*)handler);
}

static inline BindingPower lookup_bp(Lookups* lu, TokenKind kind) {
  u64 bp = (u64)defalt_bp;
  map_get(lu->bp, (u64)kind, &bp);
  return (BindingPower)bp;
}

static inline NudHandler lookup_nud(Lookups* lu, TokenKind kind) {
  return (NudHandler)map_get_ptr(lu->nud, (u64)kind);
}

static inline LedHandler lookup_led(Lookups* lu, TokenKind kind) {
  return (LedHandler)map_get_ptr(lu->led, (u64)kind);
}

static inline StmtHandler lookup_stmt(Lookups* lu, TokenKind kind) {
  return (StmtHandler)map_get_ptr(lu->stmt, (u64)kind);
}

static inline void lookup_init(Lookups* lu, MemArena* arena) {
  lu->bp = map_new(arena);
  lu->nud = map_new(arena);
  lu->led = map_new(arena);
  lu->stmt = map_new(arena);

  // Logical
  lookup_register_led(lu, TOKEN_AMP_AMP, logical, parse_binary_expr);
  lookup_register_led(lu, TOKEN_PIPE_PIPE, logical, parse_binary_expr);
  lookup_register_led(lu, TOKEN_DOT_DOT, logical, parse_binary_expr);

  // Relational
  lookup_register_led(lu, TOKEN_LESS, relational, parse_binary_expr);
  lookup_register_led(lu, TOKEN_LESS_EQUALS, relational, parse_binary_expr);
  lookup_register_led(lu, TOKEN_GREATER, relational, parse_binary_expr);
  lookup_register_led(lu, TOKEN_GREATER_EQUALS, relational, parse_binary_expr);
  lookup_register_led(lu, TOKEN_EQUALS, relational, parse_binary_expr);
  lookup_register_led(lu, TOKEN_NOT_EQUALS, relational, parse_binary_expr);

  // Additive & Multiplicative
  lookup_register_led(lu, TOKEN_PLUS, additive, parse_binary_expr);
  lookup_register_led(lu, TOKEN_MINUS, additive, parse_binary_expr);

  lookup_register_led(lu, TOKEN_STAR, multiplicative, parse_binary_expr);
  lookup_register_led(lu, TOKEN_SLASH, multiplicative, parse_binary_expr);
  lookup_register_led(lu, TOKEN_PERCENT, multiplicative, parse_binary_expr);

  // Literals & Symbols
  lookup_register_nud(lu, TOKEN_NUMBER, parse_primary_expr);
  lookup_register_nud(lu, TOKEN_STRING, parse_primary_expr);
  lookup_register_nud(lu, TOKEN_IDENTIFIER, parse_primary_expr);
}

static inline void lookup_free(Lookups* lu) {
  map_free(lu->bp);
  map_free(lu->nud);
  map_free(lu->led);
  map_free(lu->stmt);
}
