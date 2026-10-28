// Expression parser test: the Pratt core (parse_expr / parse_primary_expr /
// parse_binary_expr) on synthetic token streams. Run via `make test`.
#include "parser/lookup.h"

#include <assert.h>
#include <stdio.h>

// 42
static Expr* parse_one(MemArena* arena, Token* toks, u64 ntoks) {
    Parser p = create_parser(arena, toks, ntoks);
    Expr* e = parse_expr(&p, defalt_bp);
    assert(parser_current_kind(&p) == TOKEN_EOF); // whole stream consumed
    parser_free(&p);
    return e;
}

int main(void) {
    MemArena* arena = arena_create(MByte(4));
    assert(arena);

    // primaries
    {
        Token toks[] = {
            token_new(TOKEN_NUMBER, str8_lit("42"), 1, 1),
            token_new(TOKEN_EOF, str8_lit(""), 1, 3),
        };
        Expr* e = parse_one(arena, toks, 2);
        assert(e->kind == EXPR_NUMBER);
        assert(e->as.number.value == 42);
    }
    {
        Token toks[] = {
            token_new(TOKEN_STRING, str8_lit("hi"), 1, 1),
            token_new(TOKEN_EOF, str8_lit(""), 1, 5),
        };
        Expr* e = parse_one(arena, toks, 2);
        assert(e->kind == EXPR_STRING);
        assert(str8_eq(e->as.string.value, str8_lit("hi")));
    }
    {
        Token toks[] = {
            token_new(TOKEN_IDENTIFIER, str8_lit("x"), 1, 1),
            token_new(TOKEN_EOF, str8_lit(""), 1, 2),
        };
        Expr* e = parse_one(arena, toks, 2);
        assert(e->kind == EXPR_SYMBOL);
        assert(str8_eq(e->as.symbol.value, str8_lit("x")));
    }

    // 10 + 6 * 2  ->  10 + (6 * 2)   (* binds tighter than +)
    {
        Token toks[] = {
            token_new(TOKEN_NUMBER, str8_lit("10"), 1, 1),  token_new(TOKEN_PLUS, str8_lit("+"), 1, 4),
            token_new(TOKEN_NUMBER, str8_lit("6"), 1, 6),   token_new(TOKEN_STAR, str8_lit("*"), 1, 8),
            token_new(TOKEN_NUMBER, str8_lit("2"), 1, 10),  token_new(TOKEN_EOF, str8_lit(""), 1, 11),
        };
        Expr* e = parse_one(arena, toks, 6);
        assert(e->kind == EXPR_BINARY);
        assert(e->as.bin.oper.kind == TOKEN_PLUS);
        assert(e->as.bin.lhs->kind == EXPR_NUMBER);
        assert(e->as.bin.lhs->as.number.value == 10);

        Expr* rhs = e->as.bin.rhs;
        assert(rhs->kind == EXPR_BINARY);
        assert(rhs->as.bin.oper.kind == TOKEN_STAR);
        assert(rhs->as.bin.lhs->as.number.value == 6);
        assert(rhs->as.bin.rhs->as.number.value == 2);
    }

    // x + 10
    {
        Token toks[] = {
            token_new(TOKEN_IDENTIFIER, str8_lit("x"), 1, 1), token_new(TOKEN_PLUS, str8_lit("+"), 1, 3),
            token_new(TOKEN_NUMBER, str8_lit("10"), 1, 5),    token_new(TOKEN_EOF, str8_lit(""), 1, 7),
        };
        Expr* e = parse_one(arena, toks, 4);
        assert(e->kind == EXPR_BINARY && e->as.bin.oper.kind == TOKEN_PLUS);
        assert(e->as.bin.lhs->kind == EXPR_SYMBOL);
        assert(e->as.bin.rhs->kind == EXPR_NUMBER);
    }

    // 10 - 3 - 4  ->  (10 - 3) - 4  (same-precedence ops fold left)
    {
        Token toks[] = {
            token_new(TOKEN_NUMBER, str8_lit("10"), 1, 1), token_new(TOKEN_MINUS, str8_lit("-"), 1, 4),
            token_new(TOKEN_NUMBER, str8_lit("3"), 1, 6),  token_new(TOKEN_MINUS, str8_lit("-"), 1, 8),
            token_new(TOKEN_NUMBER, str8_lit("4"), 1, 10), token_new(TOKEN_EOF, str8_lit(""), 1, 11),
        };
        Expr* e = parse_one(arena, toks, 6);
        assert(e->kind == EXPR_BINARY && e->as.bin.oper.kind == TOKEN_MINUS);
        assert(e->as.bin.rhs->as.number.value == 4);

        Expr* lhs = e->as.bin.lhs;
        assert(lhs->kind == EXPR_BINARY && lhs->as.bin.oper.kind == TOKEN_MINUS);
        assert(lhs->as.bin.lhs->as.number.value == 10);
        assert(lhs->as.bin.rhs->as.number.value == 3);
    }

    // 8 * 2 + 1  ->  (8 * 2) + 1  (* binds tighter than +)
    {
        Token toks[] = {
            token_new(TOKEN_NUMBER, str8_lit("8"), 1, 1),   token_new(TOKEN_STAR, str8_lit("*"), 1, 3),
            token_new(TOKEN_NUMBER, str8_lit("2"), 1, 5),   token_new(TOKEN_PLUS, str8_lit("+"), 1, 7),
            token_new(TOKEN_NUMBER, str8_lit("1"), 1, 9),   token_new(TOKEN_EOF, str8_lit(""), 1, 10),
        };
        Expr* e = parse_one(arena, toks, 6);
        assert(e->kind == EXPR_BINARY && e->as.bin.oper.kind == TOKEN_PLUS);
        assert(e->as.bin.rhs->as.number.value == 1);

        Expr* lhs = e->as.bin.lhs;
        assert(lhs->kind == EXPR_BINARY && lhs->as.bin.oper.kind == TOKEN_STAR);
        assert(lhs->as.bin.lhs->as.number.value == 8);
        assert(lhs->as.bin.rhs->as.number.value == 2);
    }

    arena_destroy(arena);
    printf("expr test OK (primaries, precedence, associativity)\n");
    return 0;
}
