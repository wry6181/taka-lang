#include "ast/ast_print.h"

static void print_indent(FILE* out, u32 depth) {
  for (u32 i = 0; i < depth; ++i) {
    fputs("  ", out);
  }
}

static void print_token(FILE* out, Token op) {
  String8 kind = tokenkind_to_string8(op.kind);
  fprintf(out, "kind=%.*s value=", str8_fmt(kind));
  if (op.value.size > 0) {
    fprintf(out, "\"%.*s\"", str8_fmt(op.value));
  } else {
    fputs("<none>", out);
  }
  fprintf(out, " [%llu:%llu]", (unsigned long long)op.line, (unsigned long long)op.column);
}

static void print_path(FILE* out, Path path) {
  u64 count = path.segments ? vec_len(path.segments) : 0;
  for (u64 i = 0; i < count; ++i) {
    String8* segment = vec_at(path.segments, i);
    if (i > 0) {
      fputs("::", out);
    }
    if (segment) {
      fprintf(out, "%.*s", str8_fmt(*segment));
    }
  }
}

static void print_type_to(FILE* out, Type* type);

static void print_type_list(FILE* out, Vector* types) {
  u64 count = types ? vec_len(types) : 0;
  for (u64 i = 0; i < count; ++i) {
    if (i > 0) {
      fputs(", ", out);
    }
    print_type_to(out, (Type*)vec_at_ptr(types, i));
  }
}

static void print_effect_row(FILE* out, EffectRow* row) {
  if (!row) {
    return;
  }
  fputs(" <", out);
  u64 count = row->items ? vec_len(row->items) : 0;
  for (u64 i = 0; i < count; ++i) {
    if (i > 0) {
      fputs(", ", out);
    }
    EffectItem* item = vec_at_ptr(row->items, i);
    if (!item) {
      continue;
    }
    print_path(out, item->path);
    if (item->generic_args) {
      fputs("<", out);
      print_type_list(out, item->generic_args);
      fputs(">", out);
    }
  }
  if (row->has_tail) {
    if (count > 0) {
      fputs(", ", out);
    }
    fputs("| ", out);
    fprintf(out, "%.*s", str8_fmt(row->tail));
  }
  fputs(">", out);
}

static void print_type_to(FILE* out, Type* type) {
  if (!type) {
    fputs("<null type>", out);
    return;
  }
  if (type->is_const) {
    fputs("const ", out);
  }
  switch (type->kind) {
    case TYPE_PRIMITIVE:
      fprintf(out, "%.*s", str8_fmt(type->as.primitive.name));
      break;
    case TYPE_PATH:
      print_path(out, type->as.path.path);
      if (type->as.path.generic_args) {
        fputs("<", out);
        print_type_list(out, type->as.path.generic_args);
        fputs(">", out);
      }
      break;
    case TYPE_FN:
      if (type->as.fn.is_extern) {
        fputs("extern ", out);
      }
      fputs("fn(", out);
      print_type_list(out, type->as.fn.params);
      fputs(")", out);
      print_effect_row(out, type->as.fn.effects);
      if (type->as.fn.return_type) {
        fputs(" -> ", out);
        print_type_to(out, type->as.fn.return_type);
      }
      break;
    case TYPE_DYN:
      fputs("dyn ", out);
      print_path(out, type->as.dyn.path);
      if (type->as.dyn.generic_args) {
        fputs("<", out);
        print_type_list(out, type->as.dyn.generic_args);
        fputs(">", out);
      }
      break;
    case TYPE_TUPLE:
      fputs("(", out);
      print_type_list(out, type->as.tuple.types);
      fputs(")", out);
      break;
    case TYPE_ARRAY:
      fputs("[", out);
      print_type_to(out, type->as.array.element);
      fputs("; ", out);
      if (type->as.array.length) {
        fputs("<const-expr>", out);
      }
      fputs("]", out);
      break;
    case TYPE_AUTO:
      fputs("auto", out);
      break;
  }
  for (u32 i = 0; i < type->pointer_depth; ++i) {
    fputs("*", out);
  }
  if (type->is_reference) {
    fputs("&", out);
  }
}

static void print_expr_to(FILE* out, Expr* expr, u32 depth);
static void print_stmt_to(FILE* out, Stmt* stmt, u32 depth);
static void print_item_to(FILE* out, Item* item, u32 depth);

static void print_pattern_to(FILE* out, Pattern* pattern) {
  if (!pattern) {
    fputs("<null pattern>", out);
    return;
  }
  switch (pattern->kind) {
    case PAT_WILDCARD:
      fputs("_", out);
      break;
    case PAT_REF_BIND:
      fprintf(out, "&%.*s", str8_fmt(pattern->as.bind.name));
      break;
    case PAT_LITERAL:
      print_expr_to(out, pattern->as.literal.literal, 0);
      break;
    case PAT_BIND:
      if (pattern->as.bind.is_mut) {
        fputs("mut ", out);
      }
      fprintf(out, "%.*s", str8_fmt(pattern->as.bind.name));
      break;
    case PAT_PATH:
      print_path(out, pattern->as.path.path);
      if (pattern->as.path.has_tuple) {
        fputs("(", out);
        u64 count = pattern->as.path.tuple ? vec_len(pattern->as.path.tuple) : 0;
        for (u64 i = 0; i < count; ++i) {
          if (i > 0) {
            fputs(", ", out);
          }
          print_pattern_to(out, vec_at_ptr(pattern->as.path.tuple, i));
        }
        fputs(")", out);
      }
      if (pattern->as.path.has_struct) {
        fputs(" { ", out);
        u64 count = pattern->as.path.fields ? vec_len(pattern->as.path.fields) : 0;
        for (u64 i = 0; i < count; ++i) {
          FieldPat* field = vec_at_ptr(pattern->as.path.fields, i);
          if (!field) {
            continue;
          }
          if (i > 0) {
            fputs(", ", out);
          }
          fprintf(out, "%.*s", str8_fmt(field->name));
          if (field->has_pattern) {
            fputs(": ", out);
            print_pattern_to(out, field->pattern);
          }
        }
        fputs(" }", out);
      }
      break;
    case PAT_ALT: {
      u64 count = pattern->as.alt.alternatives ? vec_len(pattern->as.alt.alternatives) : 0;
      for (u64 i = 0; i < count; ++i) {
        if (i > 0) {
          fputs(" | ", out);
        }
        print_pattern_to(out, vec_at_ptr(pattern->as.alt.alternatives, i));
      }
      break;
    }
  }
}

static void print_condition(FILE* out, Condition cond) {
  if (cond.is_binding) {
    if (cond.is_auto) {
      fputs("auto ", out);
    }
    if (cond.is_mut) {
      fputs("mut ", out);
    }
    print_pattern_to(out, cond.pattern);
    fputs(" = ", out);
  }
  print_expr_to(out, cond.expr, 0);
}


static void print_expr_to(FILE* out, Expr* expr, u32 depth) {
  if (!expr) {
    fputs("<null expr>", out);
    return;
  }
  switch (expr->kind) {
    case EXPR_NUMBER:
      fprintf(out, "Number %lld", (long long)expr->as.number.value);
      break;
    case EXPR_FLOAT:
      fprintf(out, "Float %g", expr->as.float_lit.value);
      break;
    case EXPR_STRING:
      fprintf(out, "String \"%.*s\"", str8_fmt(expr->as.string.value));
      break;
    case EXPR_CHAR:
      fprintf(out, "Char %.*s", str8_fmt(expr->as.char_lit.value));
      break;
    case EXPR_BOOL:
      fprintf(out, "Bool %s", expr->as.boolean.value ? "true" : "false");
      break;
    case EXPR_NULL:
      fputs("Null", out);
      break;
    case EXPR_UNDEF:
      fputs("Undef", out);
      break;
    case EXPR_SYMBOL:
      fprintf(out, "Symbol %.*s", str8_fmt(expr->as.symbol.value));
      break;
    case EXPR_PATH:
      fputs("Path ", out);
      print_path(out, expr->as.path.path);
      if (expr->as.path.has_turbofish) {
        fputs("::<", out);
        print_type_list(out, expr->as.path.generic_args);
        fputs(">", out);
      }
      break;
    case EXPR_BINARY:
      fputs("Binary ", out);
      print_token(out, expr->as.bin.oper);
      fprintf(out, "\n");
      print_indent(out, depth + 1);
      fputs("left:\n", out);
      print_expr_to(out, expr->as.bin.lhs, depth + 2);
      fputc('\n', out);
      print_indent(out, depth + 1);
      fputs("right:\n", out);
      print_expr_to(out, expr->as.bin.rhs, depth + 2);
      break;
    case EXPR_PREFIX:
      fputs("Prefix ", out);
      print_token(out, expr->as.prefix.oper);
      fprintf(out, "\n");
      print_indent(out, depth + 1);
      print_expr_to(out, expr->as.prefix.right_expr, depth + 1);
      break;
    case EXPR_POSTFIX:
      fputs("Postfix ", out);
      print_token(out, expr->as.postfix.oper);
      fprintf(out, "\n");
      print_indent(out, depth + 1);
      print_expr_to(out, expr->as.postfix.expr, depth + 1);
      break;
    case EXPR_ASSIGNMENT:
      fputs("Assignment ", out);
      print_token(out, expr->as.assignment.oper);
      fprintf(out, "\n");
      print_indent(out, depth + 1);
      fputs("target:\n", out);
      print_expr_to(out, expr->as.assignment.assigne_expr, depth + 2);
      fputc('\n', out);
      print_indent(out, depth + 1);
      fputs("value:\n", out);
      print_expr_to(out, expr->as.assignment.rhs, depth + 2);
      break;
    case EXPR_TERNARY:
      fputs("Ternary\n", out);
      print_indent(out, depth + 1);
      fputs("cond:\n", out);
      print_expr_to(out, expr->as.ternary.cond, depth + 2);
      fputc('\n', out);
      print_indent(out, depth + 1);
      fputs("then:\n", out);
      print_expr_to(out, expr->as.ternary.then_expr, depth + 2);
      fputc('\n', out);
      print_indent(out, depth + 1);
      fputs("else:\n", out);
      print_expr_to(out, expr->as.ternary.else_expr, depth + 2);
      break;
    case EXPR_CAST:
      fputs("Cast to ", out);
      print_type_to(out, expr->as.cast.type);
      fputc('\n', out);
      print_indent(out, depth + 1);
      print_expr_to(out, expr->as.cast.expr, depth + 1);
      break;
    case EXPR_CALL:
      fputs("Call\n", out);
      print_indent(out, depth + 1);
      fputs("callee:\n", out);
      print_expr_to(out, expr->as.call.callee, depth + 2);
      fputc('\n', out);
      print_indent(out, depth + 1);
      fputs("args:\n", out);
      for (u64 i = 0; i < vec_len(expr->as.call.args); ++i) {
        print_expr_to(out, vec_at_ptr(expr->as.call.args, i), depth + 2);
        fputc('\n', out);
      }
      break;
    case EXPR_INDEX:
      fputs("Index\n", out);
      print_indent(out, depth + 1);
      fputs("object:\n", out);
      print_expr_to(out, expr->as.index.object, depth + 2);
      fputc('\n', out);
      print_indent(out, depth + 1);
      fputs("index:\n", out);
      print_expr_to(out, expr->as.index.index, depth + 2);
      break;
    case EXPR_FIELD:
      fputs("Field ", out);
      print_expr_to(out, expr->as.field.object, 0);
      fprintf(out, ".%.*s", str8_fmt(expr->as.field.field));
      break;
    case EXPR_STRUCT_LIT:
      fputs("StructLiteral ", out);
      print_path(out, expr->as.struct_lit.path);
      fprintf(out, " (%llu fields)", (unsigned long long)vec_len(expr->as.struct_lit.fields));
      break;
    case EXPR_ARRAY_LIT:
      if (expr->as.array_lit.is_fill) {
        fprintf(out, "ArrayLiteral fill (%llu items)",
                (unsigned long long)vec_len(expr->as.array_lit.items));
      } else {
        fprintf(out, "ArrayLiteral (%llu items)",
                (unsigned long long)vec_len(expr->as.array_lit.items));
      }
      break;
    case EXPR_LAMBDA:
      fprintf(out, "Lambda (%llu captures, %llu params)",
              (unsigned long long)vec_len(expr->as.lambda.captures),
              (unsigned long long)vec_len(expr->as.lambda.params));
      break;
    case EXPR_SIZEOF:
      fprintf(out, "Sizeof %s", expr->as.sizeof_expr.is_type ? "type" : "expr");
      break;
    case EXPR_OFFSETOF:
      fputs("Offsetof ", out);
      print_type_to(out, expr->as.offsetof_expr.type);
      fprintf(out, ".%.*s", str8_fmt(expr->as.offsetof_expr.field));
      break;
    case EXPR_IF:
      fprintf(out, "IfExpr (%llu branches)", (unsigned long long)vec_len(expr->as.if_expr.branches));
      break;
    case EXPR_MATCH:
      fprintf(out, "Match (%llu arms)", (unsigned long long)vec_len(expr->as.match.arms));
      break;
    case EXPR_PERFORM:
      fputs("Perform ", out);
      print_path(out, expr->as.perform.path);
      break;
    case EXPR_HANDLE:
      fputs("Handle ", out);
      if (expr->as.handle.handler_is_path) {
        print_path(out, expr->as.handle.handler_path);
      } else {
        fprintf(out, "(%llu ops)", (unsigned long long)vec_len(expr->as.handle.handler_ops));
      }
      break;
    case EXPR_SPAWN:
      fprintf(out, "Spawn %s", expr->as.spawn.is_block ? "block" : "expr");
      break;
    case EXPR_RESUME:
      fputs("Resume", out);
      break;
    case EXPR_TRANSACT:
      fputs("Transact", out);
      break;
    case EXPR_BLOCK:
      fputs("BlockExpression", out);
      break;
  }
}

static void print_expr_line(FILE* out, Expr* expr, u32 depth) {
  print_indent(out, depth);
  print_expr_to(out, expr, depth);
  fputc('\n', out);
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
      if (stmt->as.block.tail) {
        print_indent(out, depth + 1);
        fputs("tail:\n", out);
        print_expr_line(out, stmt->as.block.tail, depth + 2);
      }
      break;
    }
    case STMT_EXPRESION:
      fputs("ExpressionStatement\n", out);
      print_expr_line(out, &stmt->as.expresion.expr, depth + 1);
      break;
    case STMT_VAR:
      fprintf(out, "Var name=%.*s is_const=%s is_mut=%s is_auto=%s\n",
              str8_fmt(stmt->as.var_expr.var_name),
              stmt->as.var_expr.is_const ? "true" : "false",
              stmt->as.var_expr.is_mut ? "true" : "false",
              stmt->as.var_expr.is_auto ? "true" : "false");
      if (stmt->as.var_expr.type) {
        print_indent(out, depth + 1);
        fputs("type:\n", out);
        print_indent(out, depth + 2);
        print_type_to(out, stmt->as.var_expr.type);
        fputc('\n', out);
      }
      print_indent(out, depth + 1);
      fputs("value:\n", out);
      print_expr_line(out, &stmt->as.var_expr.assigned_value, depth + 2);
      break;
    case STMT_DEFER:
      fputs("Defer\n", out);
      if (stmt->as.defer_stmt.is_block) {
        print_stmt_to(out, stmt->as.defer_stmt.block, depth + 1);
      } else {
        print_stmt_to(out, stmt->as.defer_stmt.stmt, depth + 1);
      }
      break;
    case STMT_IF: {
      u64 count = stmt->as.if_stmt.branches ? vec_len(stmt->as.if_stmt.branches) : 0;
      fprintf(out, "If (%llu branches)\n", (unsigned long long)count);
      for (u64 i = 0; i < count; ++i) {
        IfBranch* branch = vec_at_ptr(stmt->as.if_stmt.branches, i);
        if (!branch) {
          continue;
        }
        print_indent(out, depth + 1);
        fputs("condition: ", out);
        print_condition(out, branch->condition);
        fputc('\n', out);
        print_stmt_to(out, branch->body, depth + 1);
      }
      if (stmt->as.if_stmt.else_block) {
        print_indent(out, depth + 1);
        fputs("else:\n", out);
        print_stmt_to(out, stmt->as.if_stmt.else_block, depth + 2);
      }
      break;
    }
    case STMT_WHILE:
      fputs("While condition: ", out);
      print_condition(out, stmt->as.while_stmt.condition);
      fputc('\n', out);
      print_stmt_to(out, stmt->as.while_stmt.body, depth + 1);
      break;
    case STMT_FOR:
      fprintf(out, "For %s\n", stmt->as.for_stmt.for_stmt.is_each ? "each" : "c-style");
      if (stmt->as.for_stmt.for_stmt.is_each) {
        print_indent(out, depth + 1);
        fputs("pattern: ", out);
        print_pattern_to(out, stmt->as.for_stmt.for_stmt.pattern);
        fputc('\n', out);
        print_indent(out, depth + 1);
        fputs("iterable:\n", out);
        print_expr_line(out, stmt->as.for_stmt.for_stmt.iterable, depth + 2);
      } else {
        if (stmt->as.for_stmt.for_stmt.init) {
          print_stmt_to(out, stmt->as.for_stmt.for_stmt.init, depth + 1);
        }
        if (stmt->as.for_stmt.for_stmt.cond) {
          print_indent(out, depth + 1);
          fputs("cond:\n", out);
          print_expr_line(out, stmt->as.for_stmt.for_stmt.cond, depth + 2);
        }
        if (stmt->as.for_stmt.for_stmt.step) {
          print_stmt_to(out, stmt->as.for_stmt.for_stmt.step, depth + 1);
        }
      }
      print_stmt_to(out, stmt->as.for_stmt.body, depth + 1);
      break;
    case STMT_BREAK:
      fputs("Break", out);
      if (stmt->as.break_stmt.has_label) {
        fprintf(out, " %.*s", str8_fmt(stmt->as.break_stmt.label));
      }
      fputc('\n', out);
      break;
    case STMT_CONTINUE:
      fputs("Continue", out);
      if (stmt->as.continue_stmt.has_label) {
        fprintf(out, " %.*s", str8_fmt(stmt->as.continue_stmt.label));
      }
      fputc('\n', out);
      break;
    case STMT_RETURN:
      fputs("Return\n", out);
      if (stmt->as.return_stmt.has_value) {
        print_expr_line(out, stmt->as.return_stmt.value, depth + 1);
      }
      break;
    case STMT_LABEL:
      fprintf(out, "Label %.*s\n", str8_fmt(stmt->as.label_stmt.label));
      print_stmt_to(out, stmt->as.label_stmt.stmt, depth + 1);
      break;
    case STMT_ITEM:
      fputs("Item\n", out);
      print_item_to(out, stmt->as.item.item, depth + 1);
      break;
  }
}

static void print_params(FILE* out, Vector* params) {
  u64 count = params ? vec_len(params) : 0;
  for (u64 i = 0; i < count; ++i) {
    if (i > 0) {
      fputs(", ", out);
    }
    Param* param = vec_at_ptr(params, i);
    if (!param) {
      continue;
    }
    if (param->is_mut) {
      fputs("mut ", out);
    }
    print_type_to(out, param->type);
    fprintf(out, " %.*s", str8_fmt(param->name));
  }
}

static void print_fn_sig(FILE* out, FnSig* sig) {
  fprintf(out, "fn %.*s(", str8_fmt(sig->name));
  print_params(out, sig->params);
  fputs(")", out);
  print_effect_row(out, sig->effects);
  if (sig->return_type) {
    fputs(" -> ", out);
    print_type_to(out, sig->return_type);
  }
}

static void print_item_to(FILE* out, Item* item, u32 depth) {
  print_indent(out, depth);
  if (!item) {
    fputs("<null item>\n", out);
    return;
  }
  if (item->visibility != VIS_DEFAULT) {
    fputs(item->visibility == VIS_PUB ? "pub " : "private ", out);
  }
  switch (item->kind) {
    case ITEM_IMPORT:
      fputs("Import ", out);
      print_path(out, item->as.import.path);
      fputc('\n', out);
      break;
    case ITEM_FN:
      print_fn_sig(out, &item->as.fn.sig);
      fprintf(out, " %s\n", item->as.fn.has_body ? "definition" : "prototype");
      if (item->as.fn.body) {
        print_stmt_to(out, item->as.fn.body, depth + 1);
      }
      break;
    case ITEM_EXTERN:
      fprintf(out, "Extern %.*s\n", str8_fmt(item->as.extern_decl.sig.name));
      break;
    case ITEM_STRUCT:
      fprintf(out, "Struct %.*s (%llu fields)\n", str8_fmt(item->as.struct_decl.name),
              (unsigned long long)vec_len(item->as.struct_decl.fields));
      break;
    case ITEM_ENUM:
      fprintf(out, "Enum %.*s (%llu variants)\n", str8_fmt(item->as.enum_decl.name),
              (unsigned long long)vec_len(item->as.enum_decl.variants));
      break;
    case ITEM_EFFECT:
      fprintf(out, "Effect %.*s (%llu ops)\n", str8_fmt(item->as.effect_decl.name),
              (unsigned long long)vec_len(item->as.effect_decl.ops));
      break;
    case ITEM_HANDLER:
      fputs("Handler ", out);
      fprintf(out, "%.*s for ", str8_fmt(item->as.handler_decl.name));
      print_path(out, item->as.handler_decl.effect_path);
      fputc('\n', out);
      break;
    case ITEM_TRAIT:
      fprintf(out, "Trait %.*s (%llu methods)\n", str8_fmt(item->as.trait_decl.name),
              (unsigned long long)vec_len(item->as.trait_decl.methods));
      break;
    case ITEM_IMPL:
      fputs("Impl ", out);
      print_path(out, item->as.impl_decl.trait_path);
      fputs(" for ", out);
      print_type_to(out, item->as.impl_decl.for_type);
      fprintf(out, " (%llu methods)\n",
              (unsigned long long)vec_len(item->as.impl_decl.methods));
      break;
    case ITEM_TYPE_ALIAS:
      fprintf(out, "TypeAlias %.*s = ", str8_fmt(item->as.type_alias.name));
      print_type_to(out, item->as.type_alias.type);
      fputc('\n', out);
      break;
    case ITEM_GLOBAL:
      fputs("Global\n", out);
      if (item->as.global.declaration) {
        print_stmt_to(out, item->as.global.declaration, depth + 1);
      }
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
  fputc('\n', out);
}

void ast_print_expr(Expr* expr) {
  ast_fprint_expr(stdout, expr);
}
