#pragma once

#include <stdio.h>

#include "ast/statements.h"

// Human-readable, indented AST dump for debugging.
//
// One node per line; children are indented two spaces per depth level.
// NULL nodes print as <null ...> so partially built trees are safe to dump.
// Stmt values are printed recursively; Expr pointers are followed.
void ast_fprint(FILE* out, Stmt* stmt);
void ast_print(Stmt* stmt);

void ast_fprint_expr(FILE* out, Expr* expr);
void ast_print_expr(Expr* expr);
