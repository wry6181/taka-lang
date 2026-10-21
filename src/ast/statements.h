
#include "utils/types.h"
#include "utils/utils.h"
#include "lexer/tokenizer.h"
#include "expressions.h"

typedef enum { STMT_BLOCK, STMT_EXPRESION } StmtKind;

typedef struct Stmt Stmt;
struct Stmt {
  StmtKind kind;
  union {
    struct { Vector* body; } block;
    struct { Expr expr; } expresion;
  } as;
};

//void eval(Stmt* stms) {
//}
