#pragma once

#include "lookup.h"
#include "parser.h"
#include "ast/statements.h"

Stmt* parse_var_stmt(Parser* p);
