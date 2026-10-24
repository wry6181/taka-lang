#include "ast/ast_print.h"

static void print_indent(FILE* out, u32 depth) {
  for (u32 i = 0; i < depth; ++i) {
    fputs("  ", out);
  }
}

static void print_expr_to(FILE* out, Expr* expr, u32 depth) {
  print_indent(out, depth);

  if (!expr) {
    fputs("<null expr>\n", out);
    return;
  }

  switch (expr->kind) {
    case EXPR_NUMBER:
      fprintf(out, "Number %lld\n", (long long)expr->as.number.value);
      break;
    case EXPR_STRING:
      fprintf(out, "String %.*s\n", str8_fmt(expr->as.string.value));
      break;
    case EXPR_SYMBOL:
      fprintf(out, "Symbol %.*s\n", str8_fmt(expr->as.symbol.value));
      break;
    case EXPR_BINARY: {
      Token op = expr->as.bin.oprt;
      String8 kind = tokenkind_to_string8(op.kind);
      fprintf(out, "Binary kind=%.*s value=", str8_fmt(kind));
      if (op.value.size > 0) {
        fprintf(out, "\"%.*s\"", str8_fmt(op.value));
      } else {
        fputs("<none>", out);
      }
      fprintf(out, " [%llu:%llu]\n", (unsigned long long)op.line,
              (unsigned long long)op.column);
      print_indent(out, depth + 1);
      fputs("left:\n", out);
      print_expr_to(out, expr->as.bin.lhs, depth + 2);
      print_indent(out, depth + 1);
      fputs("right:\n", out);
      print_expr_to(out, expr->as.bin.rhs, depth + 2);
      break;
    }
    default:
      fprintf(out, "<unknown expr kind %d>\n", (int)expr->kind);
      break;
  }
}

static void print_stmt_to(FILE* out, Stmt* stmt, u32 depth) {
  print_indent(out, depth);

  if (!stmt) {
    fputs("<null stmt>\n", out);
    return;
  }

  switch (stmt->kind) {
    case STMT_BLOCK: {
      Vector* body = stmt->as.block.body;
      u64 count = body ? vec_len(body) : 0;
      fprintf(out, "Block (%llu statements)\n", (unsigned long long)count);
      for (u64 i = 0; i < count; ++i) {
        print_stmt_to(out, (Stmt*)vec_at_ptr(body, i), depth + 1);
      }
      break;
    }
    case STMT_EXPRESION:
      fputs("ExpressionStatement\n", out);
      print_expr_to(out, &stmt->as.expresion.expr, depth + 1);
      break;
    case STMT_VAR:
      fprintf(out, "Var name=%.*s is_const=%s\n", str8_fmt(stmt->as.var_expr.var_name),
              stmt->as.var_expr.is_const ? "true" : "false");
      print_indent(out, depth + 1);
      fputs("value:\n", out);
      print_expr_to(out, &stmt->as.var_expr.assigned_value, depth + 2);
      break;
    default:
      fprintf(out, "<unknown stmt kind %d>\n", (int)stmt->kind);
      break;
  }
}

void ast_fprint(FILE* out, Stmt* stmt) {
  if (!out) {
    return;
  }
  print_stmt_to(out, stmt, 0);
}

void ast_print(Stmt* stmt) {
  ast_fprint(stdout, stmt);
}

void ast_fprint_expr(FILE* out, Expr* expr) {
  if (!out) {
    return;
  }
  print_expr_to(out, expr, 0);
}

void ast_print_expr(Expr* expr) {
  ast_fprint_expr(stdout, expr);
}
