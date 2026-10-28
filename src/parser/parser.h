#pragma once
#include "ast/ast.h"
#include "lexer/tokens.h"
#include "utils/utils.h"

typedef struct Lookups Lookups; // defined in parser/lookup.h
typedef struct Parser Parser;
struct Parser {
  MemArena* arena; // AST nodes are allocated here
  Token* tokens;
  u64 count;
  u64 pos;
  Lookups* lookups; // the four token lookups (bp/nud/led/stmt)
};

// Go: createParser(tokens) — the lookups live on the parser here instead
// of package globals (same behavior, cleaner C).
Parser create_parser(MemArena* arena, Token* tokens, u64 count);

// Release the malloc-backed lookup tables (khash backend; no-op for the
// arena backend). Go has no equivalent — GC handles it there. The arena
// part of the parser is released with arena_destroy().
void parser_free(Parser* p);

// Go helper methods (tlaceby/parser-series src/parser/parser.go):
Token parser_current(const Parser* p);          // currentToken()
TokenKind parser_current_kind(const Parser* p); // currentTokenKind()
Token parser_advance(Parser* p);                // advance() — returns the consumed token
bool parser_has_tokens(const Parser* p);        // hasTokens() — also stops at TOKEN_EOF

// Go: expectError / expect — on mismatch a fatal diagnostic (Go panics
// there), on success advance and return the consumed token.
Token parser_expect_error(Parser* p, TokenKind expected, const char* err);
Token parser_expect(Parser* p, TokenKind expected);

// Fatal parse error: log + exit(1) (Go panics there).
#if defined(__GNUC__)
__attribute__((noreturn))
#endif
void parser_die(void);

// Statement dispatch: a registered StmtHandler for the current token wins;
// otherwise the statement is parsed as an expression statement
// (expr ;). Advances past exactly one statement.
Stmt* parse_stmt(Parser* p);

// Port of Go Parse(): parse the whole token stream into a block statement
// (Go: return ast.BlockStmt{Body: Body}). Returns the STMT_BLOCK node —
// program->as.block.body is a Vector of Stmt* — owned by `arena`; the
// lookups created for the parser are freed before returning.
Stmt* parse_program(MemArena* arena, Token* tokens, u64 count);
