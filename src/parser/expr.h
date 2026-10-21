#pragma once
#include "lookup.h"
#include "parser.h"

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

// Pratt core: NUD of the current token, then keep folding LED tokens
// while their binding power is stronger than the caller's minimum.
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

    left = led(p, left, bp);
  }

  return left;
}

Expr* parse_binary_expr(Parser* p, Expr* left, BindingPower bp) {
  Token op = parser_advance(p); // the operator itself
  Expr* right = parse_expr(p, bp);

  Expr* e = PUSH_STRUCT_ZERO(p->arena, Expr);
  e->kind = EXPR_BINARY;
  e->as.bin.lhs = left;
  e->as.bin.oprt = op;
  e->as.bin.rhs = right;
  return e;
}
