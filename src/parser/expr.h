#pragma once

#include "lookup.h"
#include "parser.h"

Expr* parse_expr(Parser* p, BindingPower bp) {
  TokenKind kind = parser_current_kind(p);

  NudHandler nud = lookup_nud(p->lookups, kind);
  if (!nud) {
    Token t = parser_current(p);
    String8 name = tokenkind_to_string8(kind);
    T_FATAL("parser: nud handler expected for token %.*s [%lu:%lu]", str8_fmt(name), t.line,
            t.column);
    parser_die();
  }

  Expr* left = nud(p);

  while (lookup_bp(p->lookups, parser_current_kind(p)) > bp) {
    kind = parser_current_kind(p);

    LedHandler led = lookup_led(p->lookups, kind);
    if (!led) {
      Token t = parser_current(p);
      String8 name = tokenkind_to_string8(kind);
      T_FATAL("parser: led handler expected for token %.*s [%lu:%lu]", str8_fmt(name), t.line,
              t.column);
      parser_die();
    }

    left = led(p, left, lookup_bp(p->lookups, parser_current_kind(p)));
  }

  return left;
}

Expr* parse_primary_expr(Parser* p) {
  switch (parser_current_kind(p)) {
    case TOKEN_NUMBER: {
      Expr* e = PUSH_STRUCT_ZERO(p->arena, Expr);
      e->kind = EXPR_NUMBER;
      e->as.number.value = (i64)str8_to_u64(parser_advance(p).value);
      return e;
    }
    case TOKEN_STRING: {
      Expr* e = PUSH_STRUCT_ZERO(p->arena, Expr);
      e->kind = EXPR_STRING;
      e->as.string.value = parser_advance(p).value;
      return e;
    }
    case TOKEN_IDENTIFIER: {
      Expr* e = PUSH_STRUCT_ZERO(p->arena, Expr);
      e->kind = EXPR_SYMBOL;
      e->as.symbol.value = parser_advance(p).value;
      return e;
    }
    default: {
      Token t = parser_current(p);
      String8 name = tokenkind_to_string8(t.kind);
      T_FATAL("parser: cannot create primary expression from %.*s [%lu:%lu]", str8_fmt(name),
              t.line, t.column);
      parser_die();
    }
  }
}

Expr* parse_binary_expr(Parser* p, Expr* left, BindingPower bp) {
  Token op = parser_advance(p); // the operator itself
  Expr* right = parse_expr(p, bp);

  Expr* e = PUSH_STRUCT_ZERO(p->arena, Expr);
  e->kind = EXPR_BINARY;
  e->as.bin.lhs = left;
  e->as.bin.oper = op;
  e->as.bin.rhs = right;
  return e;
}

Expr* parse_prefix_expr(Parser* p) {
  Token op = parser_advance(p);
  Expr* right = parse_expr(p, defalt_bp);

  Expr* e = PUSH_STRUCT_ZERO(p->arena, Expr);
  e->kind = EXPR_PREFIX;
  e->as.prefix.right_expr = right;
  e->as.prefix.oper = op;

  return e;
}

Expr* parse_assignment_expr(Parser* p, Expr* left, BindingPower bp) {
  Token op = parser_advance(p);
  Expr* right = parse_expr(p, bp);

  Expr* e = PUSH_STRUCT_ZERO(p->arena, Expr);
  e->kind = EXPR_ASSIGNMENT;
  e->as.assignment.assigne_expr = left;
  e->as.assignment.oper = op;
  e->as.assignment.rhs = right;
  return e;
}

static inline void lookup_register_exprs(Lookups* lu) {
  // Assignment
  lookup_register_led(lu, TOKEN_ASSIGNMENT, assignment, parse_assignment_expr);
  lookup_register_led(lu, TOKEN_MINUS_EQUALS, assignment, parse_assignment_expr);
  lookup_register_led(lu, TOKEN_PLUS_EQUALS, assignment, parse_assignment_expr);

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

  // Prefix
  lookup_register_nud(lu, TOKEN_MINUS, parse_prefix_expr);
}
