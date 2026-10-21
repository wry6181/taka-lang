// Parser helper test: the Go parser.go helper port (currentToken /
// currentTokenKind / advance / hasTokens / expect) on a synthetic token
// stream, plus the Parse() port over it. Run via `make test`.
//
// The expect() mismatch path is fatal (Go panics there) — exercised only
// in its success form here; the failure form is checked manually.
#include "parser/parser.h"

#include <assert.h>
#include <stdio.h>

int main(void) {
    MemArena* arena = arena_create(MByte(4));
    assert(arena);

    // mut x = 42 <EOF>
    Token toks[] = {
        token_new(TOKEN_MUT, str8_lit("mut"), 1, 1),
        token_new(TOKEN_IDENTIFIER, str8_lit("x"), 1, 5),
        token_new(TOKEN_ASSIGNMENT, str8_lit("="), 1, 7),
        token_new(TOKEN_NUMBER, str8_lit("42"), 1, 9),
        token_new(TOKEN_EOF, str8_lit(""), 1, 11),
    };
    const u64 ntoks = sizeof(toks) / sizeof(toks[0]);

    Parser p = create_parser(arena, toks, ntoks);
    assert(p.pos == 0);
    assert(p.lookups != NULL);

    // hasTokens: true over real tokens...
    assert(parser_has_tokens(&p));
    assert(parser_current_kind(&p) == TOKEN_MUT);
    assert(parser_current(&p).kind == TOKEN_MUT);
    assert(parser_current(&p).line == 1);

    // advance: returns the consumed token, like Go's advance()
    Token tk = parser_advance(&p);
    assert(tk.kind == TOKEN_MUT);
    assert(p.pos == 1);

    // expect: consumes on match
    tk = parser_expect(&p, TOKEN_IDENTIFIER);
    assert(tk.kind == TOKEN_IDENTIFIER);
    assert(tk.value.size == 1 && tk.value.data[0] == 'x');
    assert(p.pos == 2);
    assert(parser_current_kind(&p) == TOKEN_ASSIGNMENT);

    // expect with custom error string (success path ignores the message)
    tk = parser_expect_error(&p, TOKEN_ASSIGNMENT, "unreachable");
    assert(tk.kind == TOKEN_ASSIGNMENT);

    // hasTokens still true on the last real token
    assert(parser_has_tokens(&p));
    assert(parser_current_kind(&p) == TOKEN_NUMBER);
    parser_advance(&p);

    // hasTokens false AT the EOF token (Go parity) — but currentToken
    // still reads it (it is a real slot in the stream)
    assert(!parser_has_tokens(&p));
    assert(parser_current_kind(&p) == TOKEN_EOF);

    // Parse() port with stmt_lu dispatch: `const x = 42;` is claimed by the
    // CONST handler and parsed as one STMT_VAR; the trailing EOF never
    // becomes a statement (Go: return ast.BlockStmt{Body: Body}).
    parser_free(&p);
    Token stmt_toks[] = {
        token_new(TOKEN_CONST, str8_lit("const"), 1, 1),
        token_new(TOKEN_IDENTIFIER, str8_lit("x"), 1, 7),
        token_new(TOKEN_ASSIGNMENT, str8_lit("="), 1, 9),
        token_new(TOKEN_NUMBER, str8_lit("42"), 1, 11),
        token_new(TOKEN_SEMICOLON, str8_lit(";"), 1, 13),
        token_new(TOKEN_EOF, str8_lit(""), 1, 14),
    };
    const u64 nstmt_toks = sizeof(stmt_toks) / sizeof(stmt_toks[0]);

    Stmt* program = parse_program(arena, stmt_toks, nstmt_toks);
    assert(program->kind == STMT_BLOCK);
    Vector* body = program->as.block.body;
    assert(body != NULL);
    assert(vec_len(body) == 1); // one statement, EOF excluded
    Stmt* var = vec_at_ptr(body, 0);
    assert(var != NULL && var->kind == STMT_VAR);
    assert(var->as.var_expr.is_const);
    assert(var->as.var_expr.var_name.size == 1);
    assert(var->as.var_expr.assigned_value.kind == EXPR_NUMBER);

    const u64 nstmts = vec_len(body);
    arena_destroy(arena);
    printf("parser helper test OK (%llu tokens -> %llu stmts)\n", (unsigned long long)nstmt_toks,
           (unsigned long long)nstmts);
    return 0;
}
