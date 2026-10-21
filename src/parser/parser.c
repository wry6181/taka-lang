#include "parser.h"
#include "lookup.h"

#include <stdlib.h>

Parser create_parser(MemArena* arena, Token* tokens, u64 count) {
  Lookups* lookups = PUSH_STRUCT(arena, Lookups);
  lookup_init(lookups, arena);
  return (Parser){
      .arena = arena,
      .tokens = tokens,
      .count = count,
      .pos = 0,
      .lookups = lookups,
  };
}

void parser_free(Parser* p) {
  if (p) {
    lookup_free(p->lookups);
  }
}

// the Go reference panics on these paths: T_FATAL + exit keeps that
// contract — deterministic in every build (unlike assert under NDEBUG)
#if defined(__GNUC__)
__attribute__((noreturn))
#endif
static void parser_die(void) {
  exit(1);
}

Token parser_current(const Parser* p) {
  if (!p || p->pos >= p->count) {
    T_FATAL("parser: unexpected end of token stream");
    parser_die();
  }
  return p->tokens[p->pos];
}

TokenKind parser_current_kind(const Parser* p) {
  return parser_current(p).kind;
}

Token parser_advance(Parser* p) {
  Token tk = parser_current(p);
  p->pos++;
  return tk;
}

bool parser_has_tokens(const Parser* p) {
  return p && p->pos < p->count && p->tokens[p->pos].kind != TOKEN_EOF;
}

Token parser_expect_error(Parser* p, TokenKind expected, const char* err) {
  Token token = parser_current(p);
  if (token.kind != expected) {
    if (err) {
      T_FATAL("parser: %s [%lu:%lu]", err, token.line, token.column);
      parser_die();
    }
    String8 want = tokenkind_to_string8(expected);
    String8 got = tokenkind_to_string8(token.kind);
    T_FATAL("parser: expected %.*s but received %.*s instead [%lu:%lu]", str8_fmt(want),
            str8_fmt(got), token.line, token.column);
    parser_die();
  }
  return parser_advance(p);
}

Token parser_expect(Parser* p, TokenKind expected) {
  return parser_expect_error(p, expected, NULL);
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
  e->as.bin.oprt = op;
  e->as.bin.rhs = right;
  return e;
}

// TODO: stmt_lu dispatch — lookup_stmt(p->lookups, parser_current_kind(p))
Stmt* parse_stmt(Parser* p) {
  parser_advance(p);
  return NULL;
}

Stmt* parse_program(MemArena* arena, Token* tokens, u64 count) {
  Parser parser = create_parser(arena, tokens, count);

  // Go: Body := make([]ast.Stmt, 0) — []ast.Stmt is a Vector of Stmt* for us
  Vector* body = vec_new(arena, sizeof(Stmt*), 0);

  // Go: for p.hasTokens() { Body = append(Body, parse_stmt(p)) }
  while (parser_has_tokens(&parser)) {
    Stmt* stmt = parse_stmt(&parser);
    vec_push_ptr(body, stmt);
  }

  parser_free(&parser);

  // Go: return ast.BlockStmt{ Body: Body }
  Stmt* block = PUSH_STRUCT_ZERO(arena, Stmt);
  block->kind = STMT_BLOCK;
  block->as.block.body = body;
  return block;
}
