#include "codegen/codegen.h"
#include "diagnostics/diagnostics.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct DeferScope {
  Vector* statements;
  bool is_loop;
} DeferScope;

typedef struct LoopContext {
  String8 label;
  bool has_label;
  char break_label[64];
  char continue_label[64];
  DeferScope* body_scope;
} LoopContext;

typedef struct ClosureSig {
  u64 id;
  Vector* params;
  Type* return_type;
} ClosureSig;

typedef struct LambdaInfo {
  Expr* expr;
  u64 id;
  u64 closure_id;
  bool has_captures;
  char env_name[64];
  char fn_name[64];
} LambdaInfo;

struct Codegen {
  MemArena* arena;
  Sema* sema;
  FILE* out;
  u32 errors;
  Vector* defer_scopes;
  Vector* loop_stack;
  u64 temp_counter;
  Vector* closure_sigs;
  Vector* lambdas;
  Map* lambda_map;
  u64 lambda_counter;
  const char* current_lambda_env_type;
};

static u64 next_temp(Codegen* codegen);

static void cg_error(Codegen* codegen, const char* fmt, ...) T_PRINTF(2, 3);

static void cg_error(Codegen* codegen, const char* fmt, ...) {
  va_list args;
  va_start(args, fmt);
  diag_vemit_format(DIAG_DOMAIN_CODEGEN, fmt, 0, 0, args);
  va_end(args);
  codegen->errors++;
}

static void emit_string(FILE* out, String8 value) {
  if (value.size > 0) {
    fwrite(value.data, 1, value.size, out);
  }
}

static void emit_prefixed(FILE* out, const char* prefix, String8 name) {
  fputs(prefix, out);
  emit_string(out, name);
}

static void emit_fn_name(FILE* out, String8 name) {
  emit_prefixed(out, "taka_", name);
}

static void emit_local_name(FILE* out, String8 name) {
  emit_prefixed(out, "v_", name);
}

static void emit_global_name(FILE* out, String8 name) {
  emit_prefixed(out, "taka_g_", name);
}

static void emit_field_name(FILE* out, String8 name) {
  emit_prefixed(out, "f_", name);
}

static bool row_has_name(EffectRow* row, const char* name) {
  if (!row || !row->items) {
    return FALSE;
  }
  String8 wanted = str8(name);
  for (u64 i = 0; i < vec_len(row->items); ++i) {
    EffectItem* item = vec_at_ptr(row->items, i);
    if (item && item->path.segments && vec_len(item->path.segments) > 0) {
      String8* first = vec_at(item->path.segments, 0);
      if (first && str8_eq(*first, wanted)) {
        return TRUE;
      }
    }
  }
  return FALSE;
}

static String8 path_first(Path path) {
  if (!path.segments || vec_len(path.segments) == 0) {
    return (String8){0};
  }
  String8* first = vec_at(path.segments, 0);
  return first ? *first : (String8){0};
}

static bool sig_needs_arena(FnSig* sig) {
  if (!sig->effects) {
    return TRUE;
  }
  return row_has_name(sig->effects, "alloc") || row_has_name(sig->effects, "transacts");
}

static bool sig_is_pure(FnSig* sig) {
  if (!sig->effects || !sig->effects->items || vec_len(sig->effects->items) == 0) {
    return FALSE;
  }
  if (sig->effects->has_tail) {
    return FALSE;
  }
  for (u64 i = 0; i < vec_len(sig->effects->items); ++i) {
    EffectItem* item = vec_at_ptr(sig->effects->items, i);
    if (!item || !item->path.segments || vec_len(item->path.segments) == 0) {
      return FALSE;
    }
    String8* first = vec_at(item->path.segments, 0);
    if (!first || !str8_eq(*first, str8_lit("pure"))) {
      return FALSE;
    }
  }
  return TRUE;
}

static bool sig_is_reads_only(FnSig* sig) {
  if (!sig->effects || !sig->effects->items || sig->effects->has_tail) {
    return FALSE;
  }
  bool reads = FALSE;
  for (u64 i = 0; i < vec_len(sig->effects->items); ++i) {
    EffectItem* item = vec_at_ptr(sig->effects->items, i);
    if (!item || !item->path.segments || vec_len(item->path.segments) != 1) {
      return FALSE;
    }
    String8* first = vec_at(item->path.segments, 0);
    if (!first) {
      return FALSE;
    }
    if (str8_eq(*first, str8_lit("reads"))) {
      reads = TRUE;
    } else if (!str8_eq(*first, str8_lit("pure"))) {
      return FALSE;
    }
  }
  return reads;
}

static void emit_fn_attrs(Codegen* codegen, FnSig* sig) {
  if (sig_is_pure(sig)) {
    fputs("TAKA_CONST ", codegen->out);
  } else if (sig_is_reads_only(sig)) {
    fputs("TAKA_PURE ", codegen->out);
  }
  if (sig->return_type && sig->return_type->kind == TYPE_PRIMITIVE &&
      str8_eq(sig->return_type->as.primitive.name, str8_lit("never"))) {
    fputs("_Noreturn ", codegen->out);
  }
}

static const char* primitive_c_name(String8 name) {
  if (str8_eq(name, str8_lit("i8"))) return "int8_t";
  if (str8_eq(name, str8_lit("i16"))) return "int16_t";
  if (str8_eq(name, str8_lit("i32"))) return "int32_t";
  if (str8_eq(name, str8_lit("i64"))) return "int64_t";
  if (str8_eq(name, str8_lit("u8"))) return "uint8_t";
  if (str8_eq(name, str8_lit("u16"))) return "uint16_t";
  if (str8_eq(name, str8_lit("u32"))) return "uint32_t";
  if (str8_eq(name, str8_lit("u64"))) return "uint64_t";
  if (str8_eq(name, str8_lit("f32"))) return "float";
  if (str8_eq(name, str8_lit("f64"))) return "double";
  if (str8_eq(name, str8_lit("char"))) return "char";
  if (str8_eq(name, str8_lit("bool"))) return "bool";
  if (str8_eq(name, str8_lit("sview"))) return "taka_sview";
  if (str8_eq(name, str8_lit("never"))) return "void";
  if (str8_eq(name, str8_lit("void"))) return "void";
  return NULL;
}

static void collect_type(Codegen* codegen, Type* type);

static bool same_type(Type* a, Type* b) {
  if (a == b) {
    return TRUE;
  }
  if (!a || !b) {
    return FALSE;
  }
  if (a->kind != b->kind || a->is_const != b->is_const ||
      a->pointer_depth != b->pointer_depth || a->is_reference != b->is_reference) {
    return FALSE;
  }
  switch (a->kind) {
    case TYPE_PRIMITIVE:
      return str8_eq(a->as.primitive.name, b->as.primitive.name);
    case TYPE_PATH: {
      u64 a_count = a->as.path.path.segments ? vec_len(a->as.path.path.segments) : 0;
      u64 b_count = b->as.path.path.segments ? vec_len(b->as.path.path.segments) : 0;
      if (a_count != b_count) return FALSE;
      for (u64 i = 0; i < a_count; ++i) {
        String8* a_seg = vec_at(a->as.path.path.segments, i);
        String8* b_seg = vec_at(b->as.path.path.segments, i);
        if (!a_seg || !b_seg || !str8_eq(*a_seg, *b_seg)) return FALSE;
      }
      u64 a_args = a->as.path.generic_args ? vec_len(a->as.path.generic_args) : 0;
      u64 b_args = b->as.path.generic_args ? vec_len(b->as.path.generic_args) : 0;
      if (a_args != b_args) return FALSE;
      for (u64 i = 0; i < a_args; ++i) {
        if (!same_type(vec_at_ptr(a->as.path.generic_args, i),
                       vec_at_ptr(b->as.path.generic_args, i))) return FALSE;
      }
      return TRUE;
    }
    case TYPE_FN: {
      u64 a_count = a->as.fn.params ? vec_len(a->as.fn.params) : 0;
      u64 b_count = b->as.fn.params ? vec_len(b->as.fn.params) : 0;
      if (a_count != b_count) return FALSE;
      for (u64 i = 0; i < a_count; ++i) {
        if (!same_type(vec_at_ptr(a->as.fn.params, i), vec_at_ptr(b->as.fn.params, i))) {
          return FALSE;
        }
      }
      return same_type(a->as.fn.return_type, b->as.fn.return_type);
    }
    case TYPE_DYN: {
      u64 a_args = a->as.dyn.generic_args ? vec_len(a->as.dyn.generic_args) : 0;
      u64 b_args = b->as.dyn.generic_args ? vec_len(b->as.dyn.generic_args) : 0;
      return a_args == b_args;
    }
    case TYPE_TUPLE: {
      u64 a_count = a->as.tuple.types ? vec_len(a->as.tuple.types) : 0;
      u64 b_count = b->as.tuple.types ? vec_len(b->as.tuple.types) : 0;
      if (a_count != b_count) return FALSE;
      for (u64 i = 0; i < a_count; ++i) {
        if (!same_type(vec_at_ptr(a->as.tuple.types, i),
                       vec_at_ptr(b->as.tuple.types, i))) return FALSE;
      }
      return TRUE;
    }
    case TYPE_ARRAY:
      return same_type(a->as.array.element, b->as.array.element);
    case TYPE_AUTO:
      return TRUE;
  }
  return FALSE;
}

static bool closure_id_for_type(Codegen* codegen, Type* type, bool create, u64* out_id) {
  if (!type || type->kind != TYPE_FN) {
    return FALSE;
  }
  for (u64 i = 0; i < vec_len(codegen->closure_sigs); ++i) {
    ClosureSig* sig = vec_at_ptr(codegen->closure_sigs, i);
    if (!sig) continue;
    u64 count = type->as.fn.params ? vec_len(type->as.fn.params) : 0;
    u64 sig_count = sig->params ? vec_len(sig->params) : 0;
    if (count != sig_count) continue;
    bool match = TRUE;
    for (u64 j = 0; j < count; ++j) {
      if (!same_type(vec_at_ptr(type->as.fn.params, j), vec_at_ptr(sig->params, j))) {
        match = FALSE;
        break;
      }
    }
    if (match && same_type(type->as.fn.return_type, sig->return_type)) {
      if (out_id) *out_id = sig->id;
      return TRUE;
    }
  }
  if (!create) {
    return FALSE;
  }
  if (type->as.fn.params) {
    for (u64 i = 0; i < vec_len(type->as.fn.params); ++i) {
      collect_type(codegen, vec_at_ptr(type->as.fn.params, i));
    }
  }
  collect_type(codegen, type->as.fn.return_type);
  ClosureSig* sig = PUSH_STRUCT_ZERO(codegen->arena, ClosureSig);
  sig->id = vec_len(codegen->closure_sigs);
  sig->params = type->as.fn.params;
  sig->return_type = type->as.fn.return_type;
  vec_push_ptr(codegen->closure_sigs, sig);
  if (out_id) *out_id = sig->id;
  return TRUE;
}

static void collect_type(Codegen* codegen, Type* type) {
  if (!type) {
    return;
  }
  switch (type->kind) {
    case TYPE_PATH:
      if (type->as.path.generic_args) {
        for (u64 i = 0; i < vec_len(type->as.path.generic_args); ++i) {
          collect_type(codegen, vec_at_ptr(type->as.path.generic_args, i));
        }
      }
      return;
    case TYPE_FN:
      closure_id_for_type(codegen, type, TRUE, NULL);
      return;
    case TYPE_DYN:
      if (type->as.dyn.generic_args) {
        for (u64 i = 0; i < vec_len(type->as.dyn.generic_args); ++i) {
          collect_type(codegen, vec_at_ptr(type->as.dyn.generic_args, i));
        }
      }
      return;
    case TYPE_TUPLE:
      if (type->as.tuple.types) {
        for (u64 i = 0; i < vec_len(type->as.tuple.types); ++i) {
          collect_type(codegen, vec_at_ptr(type->as.tuple.types, i));
        }
      }
      return;
    case TYPE_ARRAY:
      collect_type(codegen, type->as.array.element);
      return;
    case TYPE_PRIMITIVE:
    case TYPE_AUTO:
      return;
  }
}

static bool type_is_void(Type* type) {
  return !type || (type->kind == TYPE_PRIMITIVE &&
                   (str8_eq(type->as.primitive.name, str8_lit("void")) ||
                    str8_eq(type->as.primitive.name, str8_lit("never"))));
}

static void collect_expr(Codegen* codegen, Expr* expr);
static void collect_stmt(Codegen* codegen, Stmt* stmt);
static void collect_item(Codegen* codegen, Item* item);

static void collect_expr(Codegen* codegen, Expr* expr) {
  if (!expr) {
    return;
  }
  switch (expr->kind) {
    case EXPR_NUMBER:
    case EXPR_FLOAT:
    case EXPR_STRING:
    case EXPR_CHAR:
    case EXPR_BOOL:
    case EXPR_NULL:
    case EXPR_UNDEF:
      return;
    case EXPR_SYMBOL:
    case EXPR_PATH:
      collect_type(codegen, sema_expr_type(codegen->sema, expr));
      return;
    case EXPR_BINARY:
      collect_expr(codegen, expr->as.bin.lhs);
      collect_expr(codegen, expr->as.bin.rhs);
      return;
    case EXPR_TERNARY:
      collect_expr(codegen, expr->as.ternary.cond);
      collect_expr(codegen, expr->as.ternary.then_expr);
      collect_expr(codegen, expr->as.ternary.else_expr);
      return;
    case EXPR_PREFIX:
      collect_expr(codegen, expr->as.prefix.right_expr);
      return;
    case EXPR_POSTFIX:
      collect_expr(codegen, expr->as.postfix.expr);
      return;
    case EXPR_ASSIGNMENT:
      collect_expr(codegen, expr->as.assignment.assigne_expr);
      collect_expr(codegen, expr->as.assignment.rhs);
      return;
    case EXPR_CAST:
      collect_expr(codegen, expr->as.cast.expr);
      collect_type(codegen, expr->as.cast.type);
      return;
    case EXPR_CALL:
      collect_expr(codegen, expr->as.call.callee);
      if (expr->as.call.args) {
        for (u64 i = 0; i < vec_len(expr->as.call.args); ++i) {
          collect_expr(codegen, vec_at_ptr(expr->as.call.args, i));
        }
      }
      return;
    case EXPR_INDEX:
      collect_expr(codegen, expr->as.index.object);
      collect_expr(codegen, expr->as.index.index);
      return;
    case EXPR_FIELD:
      collect_expr(codegen, expr->as.field.object);
      return;
    case EXPR_STRUCT_LIT:
      if (expr->as.struct_lit.fields) {
        for (u64 i = 0; i < vec_len(expr->as.struct_lit.fields); ++i) {
          FieldInit* field = vec_at_ptr(expr->as.struct_lit.fields, i);
          if (field) collect_expr(codegen, field->value);
        }
      }
      return;
    case EXPR_ARRAY_LIT:
      if (expr->as.array_lit.items) {
        for (u64 i = 0; i < vec_len(expr->as.array_lit.items); ++i) {
          collect_expr(codegen, vec_at_ptr(expr->as.array_lit.items, i));
        }
      }
      collect_expr(codegen, expr->as.array_lit.fill);
      collect_expr(codegen, expr->as.array_lit.count);
      return;
    case EXPR_LAMBDA: {
      Type* type = sema_expr_type(codegen->sema, expr);
      if (type) {
        collect_type(codegen, type);
      }
      if (!map_get_ptr(codegen->lambda_map, (u64)(uintptr_t)expr)) {
        LambdaInfo* info = PUSH_STRUCT_ZERO(codegen->arena, LambdaInfo);
        info->expr = expr;
        info->id = codegen->lambda_counter++;
        if (type) {
          closure_id_for_type(codegen, type, TRUE, &info->closure_id);
        }
        info->has_captures = expr->as.lambda.captures &&
                             vec_len(expr->as.lambda.captures) > 0;
        snprintf(info->env_name, sizeof(info->env_name), "taka_lambda_env_%llu",
                 (unsigned long long)info->id);
        snprintf(info->fn_name, sizeof(info->fn_name), "taka_lambda_%llu",
                 (unsigned long long)info->id);
        map_set_ptr(codegen->lambda_map, (u64)(uintptr_t)expr, info);
        vec_push_ptr(codegen->lambdas, info);
      }
      if (expr->as.lambda.captures) {
        for (u64 i = 0; i < vec_len(expr->as.lambda.captures); ++i) {
          Capture* capture = vec_at_ptr(expr->as.lambda.captures, i);
          if (capture) collect_expr(codegen, capture->init);
        }
      }
      if (expr->as.lambda.params) {
        for (u64 i = 0; i < vec_len(expr->as.lambda.params); ++i) {
          Param* param = vec_at_ptr(expr->as.lambda.params, i);
          if (param) collect_type(codegen, param->type);
        }
      }
      collect_type(codegen, expr->as.lambda.return_type);
      if (expr->as.lambda.body_is_block) {
        collect_stmt(codegen, expr->as.lambda.block);
      } else {
        collect_expr(codegen, expr->as.lambda.expr);
      }
      return;
    }
    case EXPR_SIZEOF:
      if (expr->as.sizeof_expr.is_type) {
        collect_type(codegen, expr->as.sizeof_expr.type);
      } else {
        collect_expr(codegen, expr->as.sizeof_expr.expr);
      }
      return;
    case EXPR_OFFSETOF:
      collect_type(codegen, expr->as.offsetof_expr.type);
      return;
    case EXPR_IF:
      if (expr->as.if_expr.branches) {
        for (u64 i = 0; i < vec_len(expr->as.if_expr.branches); ++i) {
          IfBranch* branch = vec_at_ptr(expr->as.if_expr.branches, i);
          if (!branch) continue;
          collect_expr(codegen, branch->condition.expr);
          collect_stmt(codegen, branch->body);
        }
      }
      collect_stmt(codegen, expr->as.if_expr.else_block);
      return;
    case EXPR_MATCH:
      collect_expr(codegen, expr->as.match.subject);
      if (expr->as.match.arms) {
        for (u64 i = 0; i < vec_len(expr->as.match.arms); ++i) {
          MatchArm* arm = vec_at_ptr(expr->as.match.arms, i);
          if (!arm) continue;
          collect_expr(codegen, arm->guard);
          collect_expr(codegen, arm->expr);
          collect_stmt(codegen, arm->block);
        }
      }
      return;
    case EXPR_PERFORM:
      if (expr->as.perform.args) {
        for (u64 i = 0; i < vec_len(expr->as.perform.args); ++i) {
          collect_expr(codegen, vec_at_ptr(expr->as.perform.args, i));
        }
      }
      return;
    case EXPR_HANDLE:
      collect_stmt(codegen, expr->as.handle.block);
      collect_expr(codegen, expr->as.handle.subject);
      if (expr->as.handle.handler_ops) {
        for (u64 i = 0; i < vec_len(expr->as.handle.handler_ops); ++i) {
          HandlerOp* op = vec_at_ptr(expr->as.handle.handler_ops, i);
          if (!op) continue;
          if (op->params) {
            for (u64 j = 0; j < vec_len(op->params); ++j) {
              Param* param = vec_at_ptr(op->params, j);
              if (param) collect_type(codegen, param->type);
            }
          }
          collect_stmt(codegen, op->block);
          collect_expr(codegen, op->expr);
        }
      }
      return;
    case EXPR_SPAWN:
      collect_stmt(codegen, expr->as.spawn.block);
      collect_expr(codegen, expr->as.spawn.expr);
      return;
    case EXPR_RESUME:
      collect_expr(codegen, expr->as.resume.value);
      return;
    case EXPR_TRANSACT:
      collect_stmt(codegen, expr->as.transact.block);
      return;
    case EXPR_BLOCK:
      collect_stmt(codegen, expr->as.block.block);
      return;
  }
}

static void collect_stmt(Codegen* codegen, Stmt* stmt) {
  if (!stmt) {
    return;
  }
  switch (stmt->kind) {
    case STMT_BLOCK:
      if (stmt->as.block.body) {
        for (u64 i = 0; i < vec_len(stmt->as.block.body); ++i) {
          collect_stmt(codegen, vec_at_ptr(stmt->as.block.body, i));
        }
      }
      collect_expr(codegen, stmt->as.block.tail);
      return;
    case STMT_EXPRESION:
      collect_expr(codegen, &stmt->as.expresion.expr);
      return;
    case STMT_VAR:
      collect_type(codegen, stmt->as.var_expr.type);
      collect_expr(codegen, &stmt->as.var_expr.assigned_value);
      return;
    case STMT_DEFER:
      collect_stmt(codegen, stmt->as.defer_stmt.block);
      collect_stmt(codegen, stmt->as.defer_stmt.stmt);
      return;
    case STMT_IF:
      if (stmt->as.if_stmt.branches) {
        for (u64 i = 0; i < vec_len(stmt->as.if_stmt.branches); ++i) {
          IfBranch* branch = vec_at_ptr(stmt->as.if_stmt.branches, i);
          if (!branch) continue;
          collect_expr(codegen, branch->condition.expr);
          collect_stmt(codegen, branch->body);
        }
      }
      collect_stmt(codegen, stmt->as.if_stmt.else_block);
      return;
    case STMT_WHILE:
      collect_expr(codegen, stmt->as.while_stmt.condition.expr);
      collect_stmt(codegen, stmt->as.while_stmt.body);
      return;
    case STMT_FOR: {
      ForStmt* loop = &stmt->as.for_stmt.for_stmt;
      if (loop->is_each) {
        collect_expr(codegen, loop->iterable);
      } else {
        collect_stmt(codegen, loop->init);
        collect_expr(codegen, loop->cond);
        collect_stmt(codegen, loop->step);
      }
      collect_stmt(codegen, stmt->as.for_stmt.body);
      return;
    }
    case STMT_BREAK:
    case STMT_CONTINUE:
      return;
    case STMT_RETURN:
      collect_expr(codegen, stmt->as.return_stmt.value);
      return;
    case STMT_LABEL:
      collect_stmt(codegen, stmt->as.label_stmt.stmt);
      return;
    case STMT_ITEM:
      collect_item(codegen, stmt->as.item.item);
      return;
  }
}

static void collect_sig(Codegen* codegen, FnSig* sig) {
  if (!sig) return;
  if (sig->params) {
    for (u64 i = 0; i < vec_len(sig->params); ++i) {
      Param* param = vec_at_ptr(sig->params, i);
      if (param) collect_type(codegen, param->type);
    }
  }
  collect_type(codegen, sig->return_type);
}

static void collect_item(Codegen* codegen, Item* item) {
  if (!item) {
    return;
  }
  switch (item->kind) {
    case ITEM_IMPORT:
      return;
    case ITEM_FN: {
      Symbol* symbol = sema_lookup_global(codegen->sema, item->as.fn.sig.name);
      if (symbol && symbol->type) collect_type(codegen, symbol->type);
      collect_sig(codegen, &item->as.fn.sig);
      collect_stmt(codegen, item->as.fn.body);
      return;
    }
    case ITEM_EXTERN: {
      Symbol* symbol = sema_lookup_global(codegen->sema, item->as.extern_decl.sig.name);
      if (symbol && symbol->type) collect_type(codegen, symbol->type);
      collect_sig(codegen, &item->as.extern_decl.sig);
      return;
    }
    case ITEM_STRUCT:
      if (item->as.struct_decl.fields) {
        for (u64 i = 0; i < vec_len(item->as.struct_decl.fields); ++i) {
          StructField* field = vec_at_ptr(item->as.struct_decl.fields, i);
          if (field) collect_type(codegen, field->type);
        }
      }
      return;
    case ITEM_ENUM:
      if (item->as.enum_decl.variants) {
        for (u64 i = 0; i < vec_len(item->as.enum_decl.variants); ++i) {
          EnumVariant* variant = vec_at_ptr(item->as.enum_decl.variants, i);
          if (!variant) continue;
          if (variant->types) {
            for (u64 j = 0; j < vec_len(variant->types); ++j) {
              collect_type(codegen, vec_at_ptr(variant->types, j));
            }
          }
          if (variant->fields) {
            for (u64 j = 0; j < vec_len(variant->fields); ++j) {
              VariantField* field = vec_at_ptr(variant->fields, j);
              if (field) collect_type(codegen, field->type);
            }
          }
        }
      }
      return;
    case ITEM_EFFECT:
      if (item->as.effect_decl.ops) {
        for (u64 i = 0; i < vec_len(item->as.effect_decl.ops); ++i) {
          EffectOp* op = vec_at_ptr(item->as.effect_decl.ops, i);
          if (!op) continue;
          if (op->params) {
            for (u64 j = 0; j < vec_len(op->params); ++j) {
              Param* param = vec_at_ptr(op->params, j);
              if (param) collect_type(codegen, param->type);
            }
          }
          collect_type(codegen, op->return_type);
        }
      }
      return;
    case ITEM_HANDLER:
      if (item->as.handler_decl.ops) {
        for (u64 i = 0; i < vec_len(item->as.handler_decl.ops); ++i) {
          HandlerOp* op = vec_at_ptr(item->as.handler_decl.ops, i);
          if (!op) continue;
          if (op->params) {
            for (u64 j = 0; j < vec_len(op->params); ++j) {
              Param* param = vec_at_ptr(op->params, j);
              if (param) collect_type(codegen, param->type);
            }
          }
          collect_stmt(codegen, op->block);
          collect_expr(codegen, op->expr);
        }
      }
      return;
    case ITEM_TRAIT:
      if (item->as.trait_decl.methods) {
        for (u64 i = 0; i < vec_len(item->as.trait_decl.methods); ++i) {
          FnDecl* method = vec_at_ptr(item->as.trait_decl.methods, i);
          if (method) collect_sig(codegen, &method->sig);
        }
      }
      return;
    case ITEM_IMPL:
      collect_type(codegen, item->as.impl_decl.for_type);
      if (item->as.impl_decl.methods) {
        for (u64 i = 0; i < vec_len(item->as.impl_decl.methods); ++i) {
          FnDecl* method = vec_at_ptr(item->as.impl_decl.methods, i);
          if (!method) continue;
          collect_sig(codegen, &method->sig);
          collect_stmt(codegen, method->body);
        }
      }
      return;
    case ITEM_TYPE_ALIAS:
      collect_type(codegen, item->as.type_alias.type);
      return;
    case ITEM_GLOBAL:
      collect_stmt(codegen, item->as.global.declaration);
      return;
  }
}

static void collect_program(Codegen* codegen) {
  Stmt* program = sema_program(codegen->sema);
  if (!program || program->kind != STMT_BLOCK || !program->as.block.body) {
    return;
  }
  for (u64 i = 0; i < vec_len(program->as.block.body); ++i) {
    Stmt* stmt = vec_at_ptr(program->as.block.body, i);
    if (!stmt) continue;
    if (stmt->kind == STMT_ITEM) {
      collect_item(codegen, stmt->as.item.item);
    } else {
      collect_stmt(codegen, stmt);
    }
  }
}

static bool emit_type(Codegen* codegen, Type* type);

static void emit_closure_typedefs(Codegen* codegen) {
  for (u64 i = 0; i < vec_len(codegen->closure_sigs); ++i) {
    ClosureSig* sig = vec_at_ptr(codegen->closure_sigs, i);
    if (!sig) continue;
    fprintf(codegen->out, "typedef struct taka_closure_%llu { void* env; ",
            (unsigned long long)sig->id);
    if (type_is_void(sig->return_type)) {
      fputs("void", codegen->out);
    } else if (!emit_type(codegen, sig->return_type)) {
      return;
    }
    fprintf(codegen->out, " (*call)(void* env");
    if (sig->params) {
      for (u64 j = 0; j < vec_len(sig->params); ++j) {
        Type* param = vec_at_ptr(sig->params, j);
        if (param && param->kind == TYPE_ARRAY) {
          cg_error(codegen, TAKA_MSG_C_ARRAY_PARAMETERS_ARE_NOT_SUPPORTED_BY_THE_C_BACKEND);
          return;
        }
        fputs(", ", codegen->out);
        if (!emit_type(codegen, param)) return;
      }
    }
    fprintf(codegen->out, "); } taka_closure_%llu;\n\n",
            (unsigned long long)sig->id);
  }
}

static void emit_trampolines(Codegen* codegen) {
  Stmt* program = sema_program(codegen->sema);
  if (!program || program->kind != STMT_BLOCK || !program->as.block.body) {
    return;
  }
  for (u64 i = 0; i < vec_len(program->as.block.body); ++i) {
    Stmt* stmt = vec_at_ptr(program->as.block.body, i);
    if (!stmt || stmt->kind != STMT_ITEM || !stmt->as.item.item) continue;
    Item* item = stmt->as.item.item;
    FnSig* sig = NULL;
    if (item->kind == ITEM_FN) {
      if (item->as.fn.sig.generics && vec_len(item->as.fn.sig.generics) > 0) continue;
      sig = &item->as.fn.sig;
    } else if (item->kind == ITEM_EXTERN) {
      sig = &item->as.extern_decl.sig;
    } else {
      continue;
    }
    if (sig_needs_arena(sig)) {
      continue;
    }
    Symbol* symbol = sema_lookup_global(codegen->sema, sig->name);
    if (!symbol || !symbol->type) continue;
    u64 closure_id = 0;
    if (!closure_id_for_type(codegen, symbol->type, FALSE, &closure_id)) {
      cg_error(codegen, TAKA_MSG_C_CLOSURE_SIGNATURE_MISSING_FOR_S, str8_fmt(sig->name));
      continue;
    }
    fputs("static TAKA_UNUSED ", codegen->out);
    if (type_is_void(sig->return_type) ||
        (sig->return_type && sig->return_type->kind == TYPE_PRIMITIVE &&
         str8_eq(sig->return_type->as.primitive.name, str8_lit("never")))) {
      fputs("void", codegen->out);
    } else if (!emit_type(codegen, sig->return_type)) {
      continue;
    }
    fputs(" taka_trampoline_", codegen->out);
    emit_string(codegen->out, sig->name);
    fputs("(void* __taka_env", codegen->out);
    if (sig->params) {
      for (u64 j = 0; j < vec_len(sig->params); ++j) {
        Param* param = vec_at_ptr(sig->params, j);
        if (!param) continue;
        fputs(", ", codegen->out);
        if (!emit_type(codegen, param->type)) break;
        fputc(' ', codegen->out);
        emit_local_name(codegen->out, param->name);
      }
    }
    fputs(") { (void)__taka_env; ", codegen->out);
    bool returns_void = type_is_void(sig->return_type);
    if (!returns_void) {
      fputs("return ", codegen->out);
    }
    if (item->kind == ITEM_FN) {
      emit_fn_name(codegen->out, sig->name);
    } else {
      emit_string(codegen->out, sig->name);
    }
    fputc('(', codegen->out);
    if (sig->params) {
      for (u64 j = 0; j < vec_len(sig->params); ++j) {
        Param* param = vec_at_ptr(sig->params, j);
        if (!param) continue;
        if (j > 0) fputs(", ", codegen->out);
        emit_local_name(codegen->out, param->name);
      }
    }
    fputs("); }\n", codegen->out);
    (void)closure_id;
  }
  fputc('\n', codegen->out);
}

static bool emit_function_value(Codegen* codegen, Symbol* symbol) {
  if (!symbol || !symbol->type || symbol->type->kind != TYPE_FN) {
    cg_error(codegen, TAKA_MSG_C_CANNOT_CONVERT_THIS_SYMBOL_TO_A_FUNCTION_VALUE);
    return FALSE;
  }
  u64 closure_id = 0;
  if (!closure_id_for_type(codegen, symbol->type, FALSE, &closure_id)) {
    cg_error(codegen, TAKA_MSG_C_CLOSURE_SIGNATURE_MISSING_FOR_FUNCTION_VALUE);
    return FALSE;
  }
  if (symbol->kind == SYM_FN) {
    FnSig* sig = symbol->item ? &symbol->item->as.fn.sig : NULL;
    if (sig && sig_needs_arena(sig)) {
      cg_error(codegen, TAKA_MSG_C_ALLOCATING_FUNCTIONS_CANNOT_BE_CONVERTED_TO_CLOSURES_YET);
      return FALSE;
    }
  }
  fprintf(codegen->out, "(taka_closure_%llu){ .env = NULL, .call = taka_trampoline_",
          (unsigned long long)closure_id);
  emit_string(codegen->out, symbol->name);
  fputs(" }", codegen->out);
  return TRUE;
}

static bool emit_type(Codegen* codegen, Type* type);

static bool emit_array_length(Codegen* codegen, Expr* length) {
  if (!length) {
    cg_error(codegen, TAKA_MSG_C_ARRAY_LENGTH_IS_MISSING);
    return FALSE;
  }
  if (length->kind == EXPR_NUMBER) {
    fprintf(codegen->out, "%lld", (long long)length->as.number.value);
    return TRUE;
  }
  cg_error(codegen, TAKA_MSG_C_ARRAY_LENGTH_MUST_BE_AN_INTEGER_LITERAL_FOR_THE_C_BACKEND);
  return FALSE;
}

static bool emit_type_body(Codegen* codegen, Type* type) {
  switch (type->kind) {
    case TYPE_PRIMITIVE: {
      const char* name = primitive_c_name(type->as.primitive.name);
      if (!name) {
        cg_error(codegen, TAKA_MSG_C_UNKNOWN_PRIMITIVE_TYPE_S, str8_fmt(type->as.primitive.name));
        return FALSE;
      }
      fputs(name, codegen->out);
      return TRUE;
    }
    case TYPE_PATH: {
      if (!type->as.path.path.segments || vec_len(type->as.path.path.segments) == 0) {
        cg_error(codegen, TAKA_MSG_C_EMPTY_TYPE_PATH);
        return FALSE;
      }
      String8* first = vec_at(type->as.path.path.segments, 0);
      Symbol* symbol = first ? sema_lookup_global(codegen->sema, *first) : NULL;
      if (!symbol) {
        if (first) {
          cg_error(codegen, TAKA_MSG_C_UNKNOWN_TYPE_S, str8_fmt(*first));
        } else {
          cg_error(codegen, TAKA_MSG_C_UNKNOWN_TYPE);
        }
        return FALSE;
      }
      if (symbol->kind == SYM_TYPE_ALIAS) {
        return emit_type(codegen, symbol->as.type_alias->type);
      }
      if (symbol->kind == SYM_TYPE_PARAM) {
        cg_error(codegen, TAKA_MSG_C_GENERIC_TYPE_S_IS_NOT_SUPPORTED_BY_THE_C_BACKEND,
                 str8_fmt(*first));
        return FALSE;
      }
      if (symbol->kind != SYM_STRUCT && symbol->kind != SYM_ENUM) {
        cg_error(codegen, TAKA_MSG_C_S_IS_NOT_A_VALUE_TYPE, str8_fmt(*first));
        return FALSE;
      }
      emit_prefixed(codegen->out, "taka_", *first);
      return TRUE;
    }
    case TYPE_FN: {
      u64 id = 0;
      if (!closure_id_for_type(codegen, type, FALSE, &id)) {
        cg_error(codegen, TAKA_MSG_C_CLOSURE_TYPE_WAS_NOT_COLLECTED);
        return FALSE;
      }
      fprintf(codegen->out, "taka_closure_%llu", (unsigned long long)id);
      return TRUE;
    }
    case TYPE_ARRAY:
    case TYPE_DYN:
    case TYPE_TUPLE:
    case TYPE_AUTO:
      cg_error(codegen, TAKA_MSG_C_THIS_TYPE_IS_NOT_SUPPORTED_BY_THE_C_BACKEND_YET);
      return FALSE;
  }
  return FALSE;
}

static bool emit_type(Codegen* codegen, Type* type) {
  if (!type) {
    fputs("void", codegen->out);
    return TRUE;
  }
  if (type->is_const) {
    fputs("const ", codegen->out);
  }
  if (!emit_type_body(codegen, type)) {
    return FALSE;
  }
  u32 pointers = type->pointer_depth + (type->is_reference ? 1 : 0);
  for (u32 i = 0; i < pointers; ++i) {
    fputs("*", codegen->out);
  }
  return TRUE;
}

static void emit_param_name(FILE* out, Param* param) {
  emit_local_name(out, param->name);
}

static bool emit_fn_signature(Codegen* codegen, FnSig* sig, const char* c_name, bool is_static,
                              bool hidden_arena, bool is_extern) {
  emit_fn_attrs(codegen, sig);
  if (is_static) {
    fputs("static ", codegen->out);
  }
  Type* return_type = sig->return_type;
  if (!return_type ||
      (return_type->kind == TYPE_PRIMITIVE &&
       str8_eq(return_type->as.primitive.name, str8_lit("never")))) {
    fputs("void", codegen->out);
  } else if (!emit_type(codegen, return_type)) {
    return FALSE;
  }
  fprintf(codegen->out, " %s(", c_name);
  bool wrote = FALSE;
  if (hidden_arena) {
    fputs("taka_arena* __taka_arena", codegen->out);
    wrote = TRUE;
  }
  if (sig->params) {
    for (u64 i = 0; i < vec_len(sig->params); ++i) {
      Param* param = vec_at_ptr(sig->params, i);
      if (!param) {
        continue;
      }
      if (param->type && param->type->kind == TYPE_ARRAY) {
        cg_error(codegen, TAKA_MSG_C_ARRAY_PARAMETERS_ARE_NOT_SUPPORTED_BY_THE_C_BACKEND);
        return FALSE;
      }
      if (wrote) {
        fputs(", ", codegen->out);
      }
      if (!emit_type(codegen, param->type)) {
        return FALSE;
      }
      fputc(' ', codegen->out);
      emit_param_name(codegen->out, param);
      wrote = TRUE;
    }
  }
  if (!wrote) {
    fputs("void", codegen->out);
  }
  fputc(')', codegen->out);
  (void)is_extern;
  return TRUE;
}

static void emit_preamble(Codegen* codegen) {
  FILE* out = codegen->out;
  fputs("#include <stdint.h>\n", out);
  fputs("#include <stddef.h>\n", out);
  fputs("#include <stdbool.h>\n", out);
  fputs("#include <stdlib.h>\n", out);
  fputs("#include <string.h>\n", out);
  fputs("#include <stdalign.h>\n\n", out);
  fputs("#if defined(__clang__) || defined(__GNUC__)\n", out);
  fputs("#define TAKA_PURE __attribute__((pure))\n", out);
  fputs("#define TAKA_CONST __attribute__((const))\n", out);
  fputs("#define TAKA_UNUSED __attribute__((unused))\n", out);
  fputs("#else\n", out);
  fputs("#define TAKA_PURE\n", out);
  fputs("#define TAKA_CONST\n", out);
  fputs("#define TAKA_UNUSED\n", out);
  fputs("#endif\n\n", out);
  fputs("#if defined(__clang__)\n", out);
  fputs("#pragma clang diagnostic ignored \"-Wunused-label\"\n", out);
  fputs("#elif defined(__GNUC__)\n", out);
  fputs("#pragma GCC diagnostic ignored \"-Wunused-label\"\n", out);
  fputs("#pragma GCC diagnostic ignored \"-Wmaybe-uninitialized\"\n", out);
  fputs("#pragma GCC diagnostic ignored \"-Wuninitialized\"\n", out);
  fputs("#endif\n\n", out);
  fputs("typedef struct taka_sview { const char* data; uint64_t len; } taka_sview;\n", out);
  fputs("TAKA_UNUSED static inline taka_sview taka_sview_from_cstr(const char* s) {\n", out);
  fputs("  taka_sview view; view.data = s; view.len = s ? (uint64_t)strlen(s) : 0; return view;\n", out);
  fputs("}\n", out);
  fputs("TAKA_UNUSED static inline taka_sview taka_sview_make(const char* s, uint64_t n) {\n", out);
  fputs("  taka_sview view; view.data = s; view.len = n; return view;\n", out);
  fputs("}\n\n", out);
  fputs("typedef struct taka_arena_block taka_arena_block;\n", out);
  fputs("struct taka_arena_block { taka_arena_block* next; size_t used; size_t cap; max_align_t align; unsigned char data[]; };\n", out);
  fputs("typedef struct taka_arena { taka_arena_block* head; size_t default_cap; } taka_arena;\n", out);
  fputs("typedef struct taka_arena_mark { taka_arena_block* block; size_t used; } taka_arena_mark;\n", out);
  fputs("TAKA_UNUSED static taka_arena* taka_arena_new(size_t cap) {\n", out);
  fputs("  taka_arena* arena = (taka_arena*)calloc(1, sizeof(taka_arena));\n", out);
  fputs("  if (!arena) return NULL;\n", out);
  fputs("  arena->default_cap = cap < 4096 ? 4096 : cap;\n", out);
  fputs("  return arena;\n", out);
  fputs("}\n", out);
  fputs("TAKA_UNUSED static void taka_arena_free(taka_arena* arena) {\n", out);
  fputs("  if (!arena) return;\n", out);
  fputs("  taka_arena_block* block = arena->head;\n", out);
  fputs("  while (block) { taka_arena_block* next = block->next; free(block); block = next; }\n", out);
  fputs("  free(arena);\n", out);
  fputs("}\n", out);
  fputs("TAKA_UNUSED static void* taka_arena_alloc(taka_arena* arena, size_t size, size_t align) {\n", out);
  fputs("  if (!arena) return NULL;\n", out);
  fputs("  if (size == 0) size = 1;\n", out);
  fputs("  if (align == 0) align = alignof(max_align_t);\n", out);
  fputs("  taka_arena_block* block = arena->head;\n", out);
  fputs("  if (!block) {\n", out);
  fputs("    size_t cap = arena->default_cap < size ? size : arena->default_cap;\n", out);
  fputs("    block = (taka_arena_block*)malloc(sizeof(taka_arena_block) + cap);\n", out);
  fputs("    if (!block) return NULL;\n", out);
  fputs("    block->next = NULL; block->used = 0; block->cap = cap;\n", out);
  fputs("    arena->head = block;\n", out);
  fputs("  }\n", out);
  fputs("  size_t aligned = (block->used + align - 1) & ~(align - 1);\n", out);
  fputs("  if (aligned + size > block->cap) {\n", out);
  fputs("    size_t cap = arena->default_cap < size ? size : arena->default_cap;\n", out);
  fputs("    taka_arena_block* next = (taka_arena_block*)malloc(sizeof(taka_arena_block) + cap);\n", out);
  fputs("    if (!next) return NULL;\n", out);
  fputs("    next->next = arena->head; next->used = 0; next->cap = cap;\n", out);
  fputs("    arena->head = next; block = next; aligned = 0;\n", out);
  fputs("  }\n", out);
  fputs("  void* result = block->data + aligned;\n", out);
  fputs("  block->used = aligned + size;\n", out);
  fputs("  return result;\n", out);
  fputs("}\n", out);
  fputs("TAKA_UNUSED static taka_arena_mark taka_arena_save(taka_arena* arena) {\n", out);
  fputs("  taka_arena_mark mark; mark.block = arena ? arena->head : NULL; mark.used = mark.block ? mark.block->used : 0; return mark;\n", out);
  fputs("}\n", out);
  fputs("TAKA_UNUSED static void taka_arena_restore(taka_arena* arena, taka_arena_mark mark) {\n", out);
  fputs("  if (!arena) return;\n", out);
  fputs("  while (arena->head && arena->head != mark.block) {\n", out);
  fputs("    taka_arena_block* next = arena->head->next; free(arena->head); arena->head = next;\n", out);
  fputs("  }\n", out);
  fputs("  if (arena->head) arena->head->used = mark.used;\n", out);
  fputs("}\n\n", out);
}

static bool struct_value_dep(Sema* sema, Type* type, Symbol** out_symbol) {
  if (!type) {
    return FALSE;
  }
  if (type->pointer_depth > 0 || type->is_reference) {
    return FALSE;
  }
  if (type->kind == TYPE_ARRAY) {
    return struct_value_dep(sema, type->as.array.element, out_symbol);
  }
  if (type->kind != TYPE_PATH || !type->as.path.path.segments ||
      vec_len(type->as.path.path.segments) == 0) {
    return FALSE;
  }
  String8* first = vec_at(type->as.path.path.segments, 0);
  Symbol* symbol = first ? sema_lookup_global(sema, *first) : NULL;
  if (!symbol || symbol->kind != SYM_STRUCT) {
    return FALSE;
  }
  if (out_symbol) {
    *out_symbol = symbol;
  }
  return TRUE;
}

static void emit_forward_decls(Codegen* codegen) {
  Vector* body = sema_program(codegen->sema)->as.block.body;
  for (u64 i = 0; i < vec_len(body); ++i) {
    Stmt* stmt = vec_at_ptr(body, i);
    if (!stmt || stmt->kind != STMT_ITEM || !stmt->as.item.item) {
      continue;
    }
    Item* item = stmt->as.item.item;
    if (item->kind == ITEM_STRUCT) {
      String8 name = item->as.struct_decl.name;
      emit_prefixed(codegen->out, "typedef struct taka_", name);
      fputc(' ', codegen->out);
      emit_prefixed(codegen->out, "taka_", name);
      fputs(";\n", codegen->out);
    }
  }
  fputc('\n', codegen->out);
}

static bool enum_has_payload(EnumDecl* decl) {
  if (!decl || !decl->variants) {
    return FALSE;
  }
  for (u64 i = 0; i < vec_len(decl->variants); ++i) {
    EnumVariant* variant = vec_at_ptr(decl->variants, i);
    if (variant && (variant->kind == VARIANT_TUPLE || variant->kind == VARIANT_STRUCT)) {
      return TRUE;
    }
  }
  return FALSE;
}

static bool emit_payload_enum(Codegen* codegen, Item* item) {
  EnumDecl* decl = &item->as.enum_decl;
  if (decl->generics && vec_len(decl->generics) > 0) {
    cg_error(codegen, TAKA_MSG_C_GENERIC_ENUMS_ARE_NOT_SUPPORTED_BY_THE_C_BACKEND);
    return FALSE;
  }
  fputs("typedef enum {\n", codegen->out);
  for (u64 i = 0; i < vec_len(decl->variants); ++i) {
    EnumVariant* variant = vec_at_ptr(decl->variants, i);
    if (!variant) continue;
    fputs("  TAKA_", codegen->out);
    emit_string(codegen->out, decl->name);
    fputc('_', codegen->out);
    emit_string(codegen->out, variant->name);
    fprintf(codegen->out, " = %llu,\n", (unsigned long long)i);
  }
  fputs("} taka_", codegen->out);
  emit_string(codegen->out, decl->name);
  fputs("_Tag;\n\n", codegen->out);
  fputs("typedef struct taka_", codegen->out);
  emit_string(codegen->out, decl->name);
  fputs(" {\n  taka_", codegen->out);
  emit_string(codegen->out, decl->name);
  fputs("_Tag tag;\n  union {\n", codegen->out);
  for (u64 i = 0; i < vec_len(decl->variants); ++i) {
    EnumVariant* variant = vec_at_ptr(decl->variants, i);
    if (!variant || variant->kind == VARIANT_UNIT || variant->kind == VARIANT_VALUE) {
      continue;
    }
    fputs("    struct {\n", codegen->out);
    if (variant->kind == VARIANT_TUPLE && variant->types) {
      for (u64 j = 0; j < vec_len(variant->types); ++j) {
        Type* type = vec_at_ptr(variant->types, j);
        fputs("      ", codegen->out);
        if (!emit_type(codegen, type)) return FALSE;
        fprintf(codegen->out, " f%llu;\n", (unsigned long long)j);
      }
    } else if (variant->kind == VARIANT_STRUCT && variant->fields) {
      for (u64 j = 0; j < vec_len(variant->fields); ++j) {
        VariantField* field = vec_at_ptr(variant->fields, j);
        if (!field) continue;
        fputs("      ", codegen->out);
        if (!emit_type(codegen, field->type)) return FALSE;
        fputc(' ', codegen->out);
        emit_field_name(codegen->out, field->name);
        fputs(";\n", codegen->out);
      }
    }
    fputs("    } u_", codegen->out);
    emit_string(codegen->out, variant->name);
    fputs(";\n", codegen->out);
  }
  fputs("  } as;\n} taka_", codegen->out);
  emit_string(codegen->out, decl->name);
  fputs(";\n\n", codegen->out);
  return TRUE;
}

static bool emit_c_like_enum(Codegen* codegen, Item* item) {
  EnumDecl* decl = &item->as.enum_decl;
  if (decl->generics && vec_len(decl->generics) > 0) {
    cg_error(codegen, TAKA_MSG_C_GENERIC_ENUMS_ARE_NOT_SUPPORTED_BY_THE_C_BACKEND);
    return FALSE;
  }
  for (u64 i = 0; i < vec_len(decl->variants); ++i) {
    EnumVariant* variant = vec_at_ptr(decl->variants, i);
    if (!variant) {
      continue;
    }
    if (variant->kind != VARIANT_UNIT && variant->kind != VARIANT_VALUE) {
      cg_error(codegen, TAKA_MSG_C_PAYLOAD_ENUMS_ARE_NOT_SUPPORTED_BY_THE_C_BACKEND_YET);
      return FALSE;
    }
  }
  if (!decl->variants || vec_len(decl->variants) == 0) {
    fputs("typedef int32_t ", codegen->out);
    emit_prefixed(codegen->out, "taka_", decl->name);
    fputs(";\n\n", codegen->out);
    return TRUE;
  }
  fputs("typedef enum {\n", codegen->out);
  for (u64 i = 0; i < vec_len(decl->variants); ++i) {
    EnumVariant* variant = vec_at_ptr(decl->variants, i);
    if (!variant) {
      continue;
    }
    fputs("  TAKA_", codegen->out);
    emit_string(codegen->out, decl->name);
    fputc('_', codegen->out);
    emit_string(codegen->out, variant->name);
    if (variant->kind == VARIANT_VALUE && variant->value) {
      fputs(" = ", codegen->out);
      if (variant->value->kind == EXPR_NUMBER) {
        fprintf(codegen->out, "%lld", (long long)variant->value->as.number.value);
      } else {
        cg_error(codegen, TAKA_MSG_C_ENUM_VALUES_MUST_BE_INTEGER_LITERALS_FOR_THE_C_BACKEND);
      }
    }
    fputs(",\n", codegen->out);
  }
  fputs("} ", codegen->out);
  emit_prefixed(codegen->out, "taka_", decl->name);
  fputs(";\n\n", codegen->out);
  return TRUE;
}

static bool emit_enums(Codegen* codegen) {
  Vector* body = sema_program(codegen->sema)->as.block.body;
  for (u64 i = 0; i < vec_len(body); ++i) {
    Stmt* stmt = vec_at_ptr(body, i);
    if (!stmt || stmt->kind != STMT_ITEM || !stmt->as.item.item) {
      continue;
    }
    Item* item = stmt->as.item.item;
    if (item->kind == ITEM_ENUM) {
      bool ok = enum_has_payload(&item->as.enum_decl) ? emit_payload_enum(codegen, item)
                                                      : emit_c_like_enum(codegen, item);
      if (!ok) {
        return FALSE;
      }
    }
  }
  return TRUE;
}

static bool emit_struct_definition(Codegen* codegen, Item* item) {
  StructDecl* decl = &item->as.struct_decl;
  if (decl->generics && vec_len(decl->generics) > 0) {
    cg_error(codegen, TAKA_MSG_C_GENERIC_STRUCTS_ARE_NOT_SUPPORTED_BY_THE_C_BACKEND);
    return FALSE;
  }
  fputs("struct ", codegen->out);
  emit_prefixed(codegen->out, "taka_", decl->name);
  fputs(" {\n", codegen->out);
  if (!decl->fields || vec_len(decl->fields) == 0) {
    fputs("  uint8_t __taka_pad;\n", codegen->out);
  }
  if (decl->fields) {
    for (u64 i = 0; i < vec_len(decl->fields); ++i) {
      StructField* field = vec_at_ptr(decl->fields, i);
      if (!field) {
        continue;
      }
      if (field->type && field->type->kind == TYPE_ARRAY) {
        Type* element = field->type->as.array.element;
        if (field->type->pointer_depth > 0 || field->type->is_reference) {
          cg_error(codegen, TAKA_MSG_C_ARRAYS_OF_POINTERS_AS_STRUCT_FIELDS_ARE_NOT_SUPPORTED);
          return FALSE;
        }
        if (field->type->is_const) {
          fputs("const ", codegen->out);
        }
        if (!emit_type(codegen, element)) {
          return FALSE;
        }
        fputc(' ', codegen->out);
        emit_field_name(codegen->out, field->name);
        fputc('[', codegen->out);
        if (!emit_array_length(codegen, field->type->as.array.length)) {
          return FALSE;
        }
        fputs("];\n", codegen->out);
      } else {
        if (!emit_type(codegen, field->type)) {
          return FALSE;
        }
        fputc(' ', codegen->out);
        emit_field_name(codegen->out, field->name);
        fputs(";\n", codegen->out);
      }
    }
  }
  fputs("};\n\n", codegen->out);
  return TRUE;
}

static bool emit_structs(Codegen* codegen) {
  Vector* structs = vec_new(codegen->arena, sizeof(Item*), 0);
  Vector* body = sema_program(codegen->sema)->as.block.body;
  for (u64 i = 0; i < vec_len(body); ++i) {
    Stmt* stmt = vec_at_ptr(body, i);
    if (!stmt || stmt->kind != STMT_ITEM || !stmt->as.item.item) {
      continue;
    }
    Item* item = stmt->as.item.item;
    if (item->kind == ITEM_STRUCT) {
      vec_push_ptr(structs, item);
    }
  }
  Map* state = map_new(codegen->arena);
  for (u64 i = 0; i < vec_len(structs); ++i) {
    Item* root = vec_at_ptr(structs, i);
    u64 root_key = (u64)(uintptr_t)root;
    u64 root_state = 0;
    if (map_get(state, root_key, &root_state) && root_state == 2) {
      continue;
    }
    Vector* stack = vec_new(codegen->arena, sizeof(Item*), 0);
    vec_push_ptr(stack, root);
    while (vec_len(stack) > 0) {
      Item* item = vec_at_ptr(stack, vec_len(stack) - 1);
      u64 key = (u64)(uintptr_t)item;
      u64 item_state = 0;
      map_get(state, key, &item_state);
      if (item_state == 2) {
        vec_pop(stack);
        continue;
      }
      if (item_state == 0) {
        map_set(state, key, 1);
        bool pushed = FALSE;
        StructDecl* decl = &item->as.struct_decl;
        if (decl->fields) {
          for (u64 j = 0; j < vec_len(decl->fields); ++j) {
            StructField* field = vec_at_ptr(decl->fields, j);
            Symbol* dep = NULL;
            if (field && struct_value_dep(codegen->sema, field->type, &dep) && dep) {
              u64 dep_key = (u64)(uintptr_t)dep->item;
              u64 dep_state = 0;
              map_get(state, dep_key, &dep_state);
              if (dep_state == 1) {
                cg_error(codegen, TAKA_MSG_C_STRUCT_VALUE_CYCLE_THROUGH_S,
                         str8_fmt(dep->name));
                return FALSE;
              }
              if (dep_state == 0) {
                vec_push_ptr(stack, dep->item);
                pushed = TRUE;
                break;
              }
            }
          }
        }
        if (pushed) {
          continue;
        }
      }
      if (!emit_struct_definition(codegen, item)) {
        map_free(state);
        return FALSE;
      }
      map_set(state, key, 2);
      vec_pop(stack);
    }
  }
  map_free(state);
  return TRUE;
}

static bool emit_global_var(Codegen* codegen, Stmt* stmt);

static bool emit_global_declaration(Codegen* codegen, Stmt* stmt) {
  if (!stmt) {
    return TRUE;
  }
  if (stmt->kind == STMT_VAR) {
    return emit_global_var(codegen, stmt);
  }
  if (stmt->kind == STMT_BLOCK) {
    for (u64 i = 0; i < vec_len(stmt->as.block.body); ++i) {
      if (!emit_global_declaration(codegen, vec_at_ptr(stmt->as.block.body, i))) {
        return FALSE;
      }
    }
    return TRUE;
  }
  cg_error(codegen, TAKA_MSG_C_TOP_LEVEL_STATEMENT_IS_NOT_SUPPORTED_BY_THE_C_BACKEND);
  return FALSE;
}

static bool expr_is_lvalue(Expr* expr) {
  return expr && (expr->kind == EXPR_SYMBOL || expr->kind == EXPR_FIELD ||
                  expr->kind == EXPR_INDEX || expr->kind == EXPR_PREFIX);
}

static bool emit_expr(Codegen* codegen, Expr* expr);
static bool emit_expr_raw(Codegen* codegen, Expr* expr);
static bool emit_lambda_value(Codegen* codegen, Expr* expr);
static bool emit_lvalue(Codegen* codegen, Expr* expr);
static bool emit_ptr_value(Codegen* codegen, Expr* expr);
static bool emit_address_of(Codegen* codegen, Expr* expr);
static bool emit_initializer(Codegen* codegen, Type* type, Expr* expr);


static bool expression_is_reference(Sema* sema, Expr* expr) {
  Type* type = sema_expr_type(sema, expr);
  return type && type->is_reference;
}

static bool emit_symbol_raw_value(Codegen* codegen, Symbol* symbol);
static bool emit_symbol_value_from_symbol(Codegen* codegen, Symbol* symbol);

static bool emit_symbol_raw_value(Codegen* codegen, Symbol* symbol) {
  if (!symbol) {
    cg_error(codegen, TAKA_MSG_C_UNRESOLVED_SYMBOL);
    return FALSE;
  }
  if (symbol->kind == SYM_CAPTURE) {
    if (!codegen->current_lambda_env_type) {
      cg_error(codegen, TAKA_MSG_C_CAPTURE_USED_OUTSIDE_ITS_LAMBDA);
      return FALSE;
    }
    fprintf(codegen->out, "((%s*)__taka_env)->", codegen->current_lambda_env_type);
    emit_field_name(codegen->out, symbol->name);
    return TRUE;
  }
  if (symbol->kind == SYM_FN || symbol->kind == SYM_EXTERN_FN) {
    return emit_function_value(codegen, symbol);
  }
  if (symbol->kind == SYM_GLOBAL) {
    emit_global_name(codegen->out, symbol->name);
  } else {
    emit_local_name(codegen->out, symbol->name);
  }
  return TRUE;
}

static bool emit_symbol_value_from_symbol(Codegen* codegen, Symbol* symbol) {
  if (!symbol) {
    return FALSE;
  }
  if (symbol->type && symbol->type->is_reference) {
    fputs("(*", codegen->out);
    if (!emit_symbol_raw_value(codegen, symbol)) return FALSE;
    fputc(')', codegen->out);
    return TRUE;
  }
  return emit_symbol_raw_value(codegen, symbol);
}

static bool emit_symbol_name_value(Codegen* codegen, Expr* expr) {
  Symbol* symbol = sema_expr_symbol(codegen->sema, expr);
  if (!symbol) {
    cg_error(codegen, TAKA_MSG_C_UNRESOLVED_SYMBOL_IN_EXPRESSION);
    return FALSE;
  }
  return emit_symbol_value_from_symbol(codegen, symbol);
}

static bool emit_symbol_lvalue(Codegen* codegen, Expr* expr) {
  if (!emit_symbol_name_value(codegen, expr)) {
    return FALSE;
  }
  return TRUE;
}

static bool emit_ptr_value(Codegen* codegen, Expr* expr) {
  if (!expr) {
    return FALSE;
  }
  return emit_expr_raw(codegen, expr);
}

static bool emit_lvalue(Codegen* codegen, Expr* expr) {
  if (!expr) {
    return FALSE;
  }
  if (expression_is_reference(codegen->sema, expr)) {
    fputs("(*", codegen->out);
    if (!emit_ptr_value(codegen, expr)) {
      return FALSE;
    }
    fputc(')', codegen->out);
    return TRUE;
  }
  switch (expr->kind) {
    case EXPR_SYMBOL: {
      return emit_symbol_lvalue(codegen, expr);
    }
    case EXPR_FIELD: {
      Type* object_type = sema_expr_type(codegen->sema, expr->as.field.object);
      bool indirect = object_type && (object_type->pointer_depth > 0 ||
                                      object_type->is_reference);
      fputc('(', codegen->out);
      if (indirect) {
        if (!emit_ptr_value(codegen, expr->as.field.object)) {
          return FALSE;
        }
      } else if (!emit_expr(codegen, expr->as.field.object)) {
        return FALSE;
      }
      fputc(')', codegen->out);
      fputs(indirect ? "->" : ".", codegen->out);
      emit_field_name(codegen->out, expr->as.field.field);
      return TRUE;
    }
    case EXPR_INDEX: {
      Type* object_type = sema_expr_type(codegen->sema, expr->as.index.object);
      fputc('(', codegen->out);
      if (object_type && (object_type->pointer_depth > 0 || object_type->is_reference)) {
        if (!emit_ptr_value(codegen, expr->as.index.object)) {
          return FALSE;
        }
      } else if (!emit_lvalue(codegen, expr->as.index.object)) {
        return FALSE;
      }
      fputc(')', codegen->out);
      fputc('[', codegen->out);
      if (!emit_expr(codegen, expr->as.index.index)) {
        return FALSE;
      }
      fputc(']', codegen->out);
      return TRUE;
    }
    case EXPR_PREFIX: {
      if (expr->as.prefix.oper.kind == TOKEN_STAR) {
        fputs("(*", codegen->out);
        if (!emit_ptr_value(codegen, expr->as.prefix.right_expr)) {
          return FALSE;
        }
        fputc(')', codegen->out);
        return TRUE;
      }
      cg_error(codegen, TAKA_MSG_C_UNSUPPORTED_ASSIGNMENT_TARGET);
      return FALSE;
    }
    default:
      cg_error(codegen, TAKA_MSG_C_UNSUPPORTED_ASSIGNMENT_TARGET);
      return FALSE;
  }
}

static bool emit_address_of(Codegen* codegen, Expr* expr) {
  if (expression_is_reference(codegen->sema, expr)) {
    return emit_ptr_value(codegen, expr);
  }
  fputs("&(", codegen->out);
  if (!emit_lvalue(codegen, expr)) {
    return FALSE;
  }
  fputc(')', codegen->out);
  return TRUE;
}

static bool expr_type_is_struct_lit(Codegen* codegen, Expr* expr, Type* expected) {
  if (expr->kind != EXPR_STRUCT_LIT) {
    return FALSE;
  }
  Type* type = sema_expr_type(codegen->sema, expr);
  (void)expected;
  return type && type->kind == TYPE_PATH;
}

static bool emit_enum_constant(Codegen* codegen, Symbol* enum_symbol, EnumVariant* variant) {
  fputs("TAKA_", codegen->out);
  emit_string(codegen->out, enum_symbol->name);
  fputc('_', codegen->out);
  emit_string(codegen->out, variant->name);
  return TRUE;
}

static bool emit_enum_unit_value(Codegen* codegen, Symbol* enum_symbol, EnumVariant* variant) {
  EnumDecl* decl = enum_symbol->as.enum_decl;
  if (!enum_has_payload(decl)) {
    return emit_enum_constant(codegen, enum_symbol, variant);
  }
  fputs("((taka_", codegen->out);
  emit_string(codegen->out, enum_symbol->name);
  fputs("){ .tag = TAKA_", codegen->out);
  emit_string(codegen->out, enum_symbol->name);
  fputc('_', codegen->out);
  emit_string(codegen->out, variant->name);
  fputs(" })", codegen->out);
  return TRUE;
}

static bool emit_enum_tuple_constructor(Codegen* codegen, Symbol* enum_symbol,
                                        EnumVariant* variant, Vector* args) {
  EnumDecl* decl = enum_symbol->as.enum_decl;
  if (!enum_has_payload(decl)) {
    if (args && vec_len(args) > 0) {
      cg_error(codegen, TAKA_MSG_C_LIKE_ENUM_VARIANTS_DO_NOT_TAKE_ARGUMENTS);
      return FALSE;
    }
    return emit_enum_constant(codegen, enum_symbol, variant);
  }
  if (variant->kind == VARIANT_UNIT || variant->kind == VARIANT_VALUE) {
    if (args && vec_len(args) > 0) {
      cg_error(codegen, TAKA_MSG_C_UNIT_ENUM_VARIANTS_DO_NOT_TAKE_ARGUMENTS);
      return FALSE;
    }
    return emit_enum_unit_value(codegen, enum_symbol, variant);
  }
  if (variant->kind != VARIANT_TUPLE) {
    cg_error(codegen, TAKA_MSG_C_THIS_ENUM_VARIANT_USES_STRUCT_LITERAL_CONSTRUCTION);
    return FALSE;
  }
  fputs("((taka_", codegen->out);
  emit_string(codegen->out, enum_symbol->name);
  fputs("){ .tag = TAKA_", codegen->out);
  emit_string(codegen->out, enum_symbol->name);
  fputc('_', codegen->out);
  emit_string(codegen->out, variant->name);
  fputs(", .as.u_", codegen->out);
  emit_string(codegen->out, variant->name);
  fputs(" = { ", codegen->out);
  if (args) {
    for (u64 i = 0; i < vec_len(args); ++i) {
      Expr* arg = vec_at_ptr(args, i);
      Type* field_type = NULL;
      if (variant->types && i < vec_len(variant->types)) {
        field_type = vec_at_ptr(variant->types, i);
      }
      fprintf(codegen->out, ".f%llu = ", (unsigned long long)i);
      if (field_type && field_type->is_reference) {
        if (!emit_initializer(codegen, field_type, arg)) return FALSE;
      } else if (!emit_expr(codegen, arg)) {
        return FALSE;
      }
      fputs(", ", codegen->out);
    }
  }
  fputs("} })", codegen->out);
  return TRUE;
}

static bool emit_struct_lit_body(Codegen* codegen, Expr* expr, bool braced) {
  EnumVariant* enum_variant = NULL;
  Symbol* enum_symbol =
      sema_lookup_enum_variant(codegen->sema, expr->as.struct_lit.path, &enum_variant);
  if (enum_symbol && enum_variant) {
    if (enum_variant->kind != VARIANT_STRUCT || !enum_has_payload(enum_symbol->as.enum_decl)) {
      cg_error(codegen, TAKA_MSG_C_ENUM_VARIANT_S_IS_NOT_A_STRUCT_VARIANT,
               str8_fmt(enum_variant->name));
      return FALSE;
    }
    if (!braced) {
      fputs("(taka_", codegen->out);
      emit_string(codegen->out, enum_symbol->name);
      fputs(")", codegen->out);
    }
    fputs("{ .tag = TAKA_", codegen->out);
    emit_string(codegen->out, enum_symbol->name);
    fputc('_', codegen->out);
    emit_string(codegen->out, enum_variant->name);
    fputs(", .as.u_", codegen->out);
    emit_string(codegen->out, enum_variant->name);
    fputs(" = { ", codegen->out);
    if (expr->as.struct_lit.fields) {
      for (u64 i = 0; i < vec_len(expr->as.struct_lit.fields); ++i) {
        FieldInit* field = vec_at_ptr(expr->as.struct_lit.fields, i);
        if (!field) continue;
        Type* field_type = NULL;
        if (enum_variant->fields) {
          for (u64 j = 0; j < vec_len(enum_variant->fields); ++j) {
            VariantField* candidate = vec_at_ptr(enum_variant->fields, j);
            if (candidate && str8_eq(candidate->name, field->name)) {
              field_type = candidate->type;
              break;
            }
          }
        }
        fputs(".", codegen->out);
        emit_field_name(codegen->out, field->name);
        fputs(" = ", codegen->out);
        if (field_type && field_type->is_reference) {
          if (!emit_initializer(codegen, field_type, field->value)) return FALSE;
        } else if (!emit_expr(codegen, field->value)) {
          return FALSE;
        }
        fputs(", ", codegen->out);
      }
    }
    fputs("} }", codegen->out);
    return TRUE;
  }
  Type* type = sema_expr_type(codegen->sema, expr);
  if (!type || type->kind != TYPE_PATH) {
    cg_error(codegen, TAKA_MSG_C_CANNOT_RESOLVE_STRUCT_LITERAL_TYPE);
    return FALSE;
  }
  String8* name = vec_at(type->as.path.path.segments, 0);
  Symbol* symbol = name ? sema_lookup_global(codegen->sema, *name) : NULL;
  if (!symbol || symbol->kind != SYM_STRUCT) {
    if (name) {
      cg_error(codegen, TAKA_MSG_C_UNKNOWN_STRUCT_LITERAL_S, str8_fmt(*name));
    } else {
      cg_error(codegen, TAKA_MSG_C_UNKNOWN_STRUCT_LITERAL);
    }
    return FALSE;
  }
  if (!braced) {
    fputs("(taka_", codegen->out);
    emit_string(codegen->out, *name);
    fputs(")", codegen->out);
  }
  fputs("{ ", codegen->out);
  if (expr->as.struct_lit.fields) {
    for (u64 i = 0; i < vec_len(expr->as.struct_lit.fields); ++i) {
      FieldInit* field = vec_at_ptr(expr->as.struct_lit.fields, i);
      if (!field) {
        continue;
      }
      StructField* target = NULL;
      if (symbol->as.struct_decl->fields) {
        for (u64 j = 0; j < vec_len(symbol->as.struct_decl->fields); ++j) {
          StructField* candidate = vec_at_ptr(symbol->as.struct_decl->fields, j);
          if (candidate && str8_eq(candidate->name, field->name)) {
            target = candidate;
            break;
          }
        }
      }
      fputs(".", codegen->out);
      emit_field_name(codegen->out, field->name);
      fputs(" = ", codegen->out);
      if (!emit_initializer(codegen, target ? target->type : NULL, field->value)) {
        return FALSE;
      }
      fputs(", ", codegen->out);
    }
  }
  fputs("}", codegen->out);
  return TRUE;
}

static bool emit_array_literal_body(Codegen* codegen, Expr* expr, Type* expected) {
  if (expr->as.array_lit.is_fill) {
    if (expr->as.array_lit.items && vec_len(expr->as.array_lit.items) > 0) {
      cg_error(codegen, TAKA_MSG_C_INVALID_FILL_ARRAY_LITERAL);
      return FALSE;
    }
    Type* array_type = expected ? expected : sema_expr_type(codegen->sema, expr);
    if (!array_type || array_type->kind != TYPE_ARRAY || !array_type->as.array.length ||
        array_type->as.array.length->kind != EXPR_NUMBER) {
      cg_error(codegen, TAKA_MSG_C_FILL_ARRAY_LITERALS_REQUIRE_A_FIXED_INTEGER_LENGTH);
      return FALSE;
    }
    if (!expr->as.array_lit.fill) {
      cg_error(codegen, TAKA_MSG_C_FILL_ARRAY_LITERAL_HAS_NO_VALUE);
      return FALSE;
    }
    if (expr->as.array_lit.fill->kind == EXPR_NUMBER &&
        expr->as.array_lit.fill->as.number.value == 0) {
      fputs("{0}", codegen->out);
      return TRUE;
    }
    long long length = (long long)array_type->as.array.length->as.number.value;
    fprintf(codegen->out, "{[0 ... %lld] = ", length - 1);
    if (!emit_expr(codegen, expr->as.array_lit.fill)) {
      return FALSE;
    }
    fputs("}", codegen->out);
    return TRUE;
  }
  fputs("{ ", codegen->out);
  for (u64 i = 0; i < vec_len(expr->as.array_lit.items); ++i) {
    Expr* item = vec_at_ptr(expr->as.array_lit.items, i);
    if (!emit_expr(codegen, item)) {
      return FALSE;
    }
    fputs(", ", codegen->out);
  }
  fputs("}", codegen->out);
  return TRUE;
}

static bool emit_initializer(Codegen* codegen, Type* type, Expr* expr) {
  if (!expr) {
    return FALSE;
  }
  if (expr->kind == EXPR_UNDEF) {
    return TRUE;
  }
  if (type && type->is_reference) {
    if (expr->kind == EXPR_NULL) {
      fputs("NULL", codegen->out);
      return TRUE;
    }
    if (expression_is_reference(codegen->sema, expr)) {
      return emit_ptr_value(codegen, expr);
    }
    if (expr->kind == EXPR_PREFIX && expr->as.prefix.oper.kind == TOKEN_AMP) {
      return emit_expr_raw(codegen, expr);
    }
    if (!expr_is_lvalue(expr)) {
      cg_error(codegen, TAKA_MSG_C_REFERENCE_INITIALIZER_MUST_BE_AN_LVALUE);
      return FALSE;
    }
    return emit_address_of(codegen, expr);
  }
  if (type && type->kind == TYPE_ARRAY) {
    if (expr->kind == EXPR_ARRAY_LIT) {
      return emit_array_literal_body(codegen, expr, type);
    }
    if (expr->kind == EXPR_NULL || expr->kind == EXPR_UNDEF) {
      fputs("{0}", codegen->out);
      return TRUE;
    }
    cg_error(codegen, TAKA_MSG_C_ARRAY_INITIALIZER_MUST_BE_AN_ARRAY_LITERAL);
    return FALSE;
  }
  if (expr_type_is_struct_lit(codegen, expr, type)) {
    return emit_struct_lit_body(codegen, expr, true);
  }
  if (expr->kind == EXPR_NULL && type && (type->pointer_depth > 0 || type->is_reference)) {
    fputs("NULL", codegen->out);
    return TRUE;
  }
  return emit_expr(codegen, expr);
}

static bool emit_call_args(Codegen* codegen, Vector* args, Vector* params, u64 offset) {
  if (!args) {
    return TRUE;
  }
  for (u64 i = 0; i < vec_len(args); ++i) {
    Expr* arg = vec_at_ptr(args, i);
    Param* param = NULL;
    if (params && offset + i < vec_len(params)) {
      param = vec_at_ptr(params, offset + i);
    }
    Type* expected = param ? param->type : NULL;
    if (expected && expected->is_reference) {
      if (!emit_initializer(codegen, expected, arg)) {
        return FALSE;
      }
    } else if (!emit_expr(codegen, arg)) {
      return FALSE;
    }
    if (i + 1 < vec_len(args)) {
      fputs(", ", codegen->out);
    }
  }
  return TRUE;
}

static bool emit_call_args_types(Codegen* codegen, Vector* args, Vector* param_types,
                                 u64 offset) {
  if (!args) {
    return TRUE;
  }
  for (u64 i = 0; i < vec_len(args); ++i) {
    Expr* arg = vec_at_ptr(args, i);
    Type* expected = NULL;
    if (param_types && offset + i < vec_len(param_types)) {
      expected = vec_at_ptr(param_types, offset + i);
    }
    if (expected && expected->is_reference) {
      if (!emit_initializer(codegen, expected, arg)) {
        return FALSE;
      }
    } else if (!emit_expr(codegen, arg)) {
      return FALSE;
    }
    if (i + 1 < vec_len(args)) {
      fputs(", ", codegen->out);
    }
  }
  return TRUE;
}

static bool emit_closure_call(Codegen* codegen, Expr* callee, Type* fn_type, Vector* args) {
  if (!fn_type || fn_type->kind != TYPE_FN) {
    cg_error(codegen, TAKA_MSG_C_CALL_TARGET_IS_NOT_A_CLOSURE);
    return FALSE;
  }
  u64 id = next_temp(codegen);
  fputs("({ ", codegen->out);
  if (!emit_type(codegen, fn_type)) {
    return FALSE;
  }
  fprintf(codegen->out, " __taka_closure_%llu = ", (unsigned long long)id);
  if (!emit_expr(codegen, callee)) {
    return FALSE;
  }
  fprintf(codegen->out, "; __taka_closure_%llu.call(__taka_closure_%llu.env",
          (unsigned long long)id, (unsigned long long)id);
  if (args && vec_len(args) > 0) {
    fputs(", ", codegen->out);
    if (!emit_call_args_types(codegen, args, fn_type->as.fn.params, 0)) {
      return FALSE;
    }
  }
  fputs("); })", codegen->out);
  return TRUE;
}

static bool emit_call(Codegen* codegen, Expr* expr) {
  Expr* callee = expr->as.call.callee;
  if (callee && callee->kind == EXPR_PATH) {
    EnumVariant* variant = NULL;
    Symbol* enum_symbol = sema_lookup_enum_variant(codegen->sema, callee->as.path.path, &variant);
    if (enum_symbol && variant) {
      return emit_enum_tuple_constructor(codegen, enum_symbol, variant, expr->as.call.args);
    }
  }

  if (callee && callee->kind == EXPR_FIELD) {
    Type* object_type = sema_expr_type(codegen->sema, callee->as.field.object);
    FnDecl* method = NULL;
    Symbol* method_symbol =
        sema_lookup_method(codegen->sema, object_type, callee->as.field.field, &method);
    if (method && method_symbol) {
      Type* base = object_type;
      if (base && base->is_reference) {
        Type* copy = PUSH_STRUCT_ZERO(codegen->arena, Type);
        *copy = *base;
        copy->is_reference = FALSE;
        base = copy;
      } else if (base && base->pointer_depth > 0) {
        Type* copy = PUSH_STRUCT_ZERO(codegen->arena, Type);
        *copy = *base;
        copy->pointer_depth--;
        base = copy;
      }
      if (!base || base->kind != TYPE_PATH || vec_len(base->as.path.path.segments) == 0) {
        cg_error(codegen, TAKA_MSG_C_CANNOT_RESOLVE_METHOD_RECEIVER_TYPE);
        return FALSE;
      }
      String8* type_name = vec_at(base->as.path.path.segments, 0);
      fputs("taka_", codegen->out);
      emit_string(codegen->out, *type_name);
      fputc('_', codegen->out);
      emit_string(codegen->out, method->sig.name);
      fputc('(', codegen->out);
      bool wrote = FALSE;
      if (sig_needs_arena(&method->sig)) {
        fputs("__taka_arena", codegen->out);
        wrote = TRUE;
      }
      if (method->sig.params && vec_len(method->sig.params) > 0) {
        Param* self = vec_at_ptr(method->sig.params, 0);
        if (wrote) {
          fputs(", ", codegen->out);
        }
        if (self && self->type && self->type->is_reference) {
          bool object_pointer =
              expression_is_reference(codegen->sema, callee->as.field.object) ||
              (object_type && (object_type->pointer_depth > 0 || object_type->is_reference));
          if (object_pointer) {
            if (!emit_ptr_value(codegen, callee->as.field.object)) return FALSE;
          } else if (!emit_address_of(codegen, callee->as.field.object)) {
            return FALSE;
          }
        } else if (self && self->type && (self->type->pointer_depth > 0)) {
          if (expression_is_reference(codegen->sema, callee->as.field.object) ||
              (object_type && object_type->pointer_depth > 0)) {
            if (!emit_ptr_value(codegen, callee->as.field.object)) return FALSE;
          } else if (!emit_address_of(codegen, callee->as.field.object)) {
            return FALSE;
          }
        } else if (object_type &&
                   (object_type->pointer_depth > 0 || object_type->is_reference)) {
          fputs("(*", codegen->out);
          if (!emit_ptr_value(codegen, callee->as.field.object)) return FALSE;
          fputc(')', codegen->out);
        } else if (!emit_expr(codegen, callee->as.field.object)) {
          return FALSE;
        }
        wrote = TRUE;
      }
      if (expr->as.call.args && vec_len(expr->as.call.args) > 0) {
        if (wrote) {
          fputs(", ", codegen->out);
        }
        if (!emit_call_args(codegen, expr->as.call.args, method->sig.params, 1)) {
          return FALSE;
        }
      }
      fputc(')', codegen->out);
      return TRUE;
    }
    Type* callee_type = sema_expr_type(codegen->sema, callee);
    if (callee_type && callee_type->kind == TYPE_FN) {
      return emit_closure_call(codegen, callee, callee_type, expr->as.call.args);
    }
    cg_error(codegen, TAKA_MSG_C_UNSUPPORTED_METHOD_CALL_S, str8_fmt(callee->as.field.field));
    return FALSE;
  }

  Symbol* symbol = callee ? sema_expr_symbol(codegen->sema, callee) : NULL;
  FnSig* sig = NULL;
  bool hidden = FALSE;
  if (symbol && symbol->kind == SYM_FN) {
    sig = &symbol->item->as.fn.sig;
    hidden = sig_needs_arena(sig);
    emit_fn_name(codegen->out, symbol->name);
  } else if (symbol && symbol->kind == SYM_EXTERN_FN) {
    sig = &symbol->item->as.extern_decl.sig;
    emit_string(codegen->out, symbol->name);
  } else {
    Type* callee_type = sema_expr_type(codegen->sema, callee);
    if (!callee_type || callee_type->kind != TYPE_FN) {
      cg_error(codegen, TAKA_MSG_C_CALL_TARGET_IS_NOT_A_FUNCTION);
      return FALSE;
    }
    return emit_closure_call(codegen, callee, callee_type, expr->as.call.args);
  }
  fputc('(', codegen->out);
  bool wrote = FALSE;
  if (hidden) {
    fputs("__taka_arena", codegen->out);
    wrote = TRUE;
  }
  if (expr->as.call.args && vec_len(expr->as.call.args) > 0) {
    if (wrote) {
      fputs(", ", codegen->out);
    }
    if (!emit_call_args(codegen, expr->as.call.args, sig ? sig->params : NULL, 0)) {
      return FALSE;
    }
  }
  fputc(')', codegen->out);
  return TRUE;
}

static Type* find_variant_field_type(EnumVariant* variant, String8 name) {
  if (!variant || !variant->fields) {
    return NULL;
  }
  for (u64 i = 0; i < vec_len(variant->fields); ++i) {
    VariantField* field = vec_at_ptr(variant->fields, i);
    if (field && str8_eq(field->name, name)) {
      return field->type;
    }
  }
  return NULL;
}

static bool emit_pattern_binding(Codegen* codegen, Pattern* pattern, const char* value_expr,
                                 Type* fallback_type) {
  Symbol* symbol = sema_pattern_symbol(codegen->sema, pattern);
  if (!symbol) {
    cg_error(codegen, TAKA_MSG_C_PATTERN_BINDING_IS_NOT_RESOLVED);
    return FALSE;
  }
  Type* type = symbol->type ? symbol->type : fallback_type;
  fputs("TAKA_UNUSED ", codegen->out);
  if (pattern->kind == PAT_REF_BIND) {
    if (type) {
      if (!emit_type(codegen, type)) return FALSE;
    } else {
      fputs("__auto_type", codegen->out);
    }
    fputc(' ', codegen->out);
    emit_local_name(codegen->out, symbol->name);
    fputs(" = &(", codegen->out);
    fputs(value_expr, codegen->out);
    fputs(");\n", codegen->out);
    return TRUE;
  }
  if (type) {
    if (!emit_type(codegen, type)) return FALSE;
  } else {
    fputs("__auto_type", codegen->out);
  }
  fputc(' ', codegen->out);
  emit_local_name(codegen->out, symbol->name);
  fputs(" = ", codegen->out);
  fputs(value_expr, codegen->out);
  fputs(";\n", codegen->out);
  return TRUE;
}

static bool emit_named_binding(Codegen* codegen, String8 name, Type* type,
                               const char* value_expr) {
  fputs("TAKA_UNUSED ", codegen->out);
  if (type) {
    if (!emit_type(codegen, type)) return FALSE;
  } else {
    fputs("__auto_type", codegen->out);
  }
  fputc(' ', codegen->out);
  emit_local_name(codegen->out, name);
  fputs(" = ", codegen->out);
  fputs(value_expr, codegen->out);
  fputs(";\n", codegen->out);
  return TRUE;
}

static void emit_match_leaf(Codegen* codegen, MatchArm* arm, const char* result_name,
                            const char* done_label) {
  if (!arm) {
    return;
  }
  if (arm->body_is_block) {
    cg_error(codegen, TAKA_MSG_C_BLOCK_MATCH_ARMS_ARE_NOT_SUPPORTED_BY_THE_C_BACKEND_YET);
    return;
  }
  if (arm->guard) {
    fputs("if (", codegen->out);
    if (!emit_expr(codegen, arm->guard)) return;
    fputs(") { ", codegen->out);
  }
  fputs(result_name, codegen->out);
  fputs(" = ", codegen->out);
  if (!emit_expr(codegen, arm->expr)) {
    fputs(" = 0; ", codegen->out);
    return;
  }
  fprintf(codegen->out, "; goto %s; ", done_label);
  if (arm->guard) {
    fputs("}", codegen->out);
  }
  fputc('\n', codegen->out);
}

static void emit_match_pattern(Codegen* codegen, Pattern* pattern, const char* subject_expr,
                               Type* subject_type, MatchArm* arm, const char* result_name,
                               const char* done_label);

static void emit_match_pattern(Codegen* codegen, Pattern* pattern, const char* subject_expr,
                               Type* subject_type, MatchArm* arm, const char* result_name,
                               const char* done_label) {
  if (!pattern) {
    emit_match_leaf(codegen, arm, result_name, done_label);
    return;
  }
  switch (pattern->kind) {
    case PAT_WILDCARD:
      emit_match_leaf(codegen, arm, result_name, done_label);
      return;
    case PAT_BIND:
    case PAT_REF_BIND:
      emit_pattern_binding(codegen, pattern, subject_expr, subject_type);
      emit_match_leaf(codegen, arm, result_name, done_label);
      return;
    case PAT_LITERAL:
      fputs("if (", codegen->out);
      fputs(subject_expr, codegen->out);
      fputs(" == ", codegen->out);
      if (!emit_expr(codegen, pattern->as.literal.literal)) return;
      fputs(") {\n", codegen->out);
      emit_match_leaf(codegen, arm, result_name, done_label);
      fputs("}\n", codegen->out);
      return;
    case PAT_ALT:
      if (pattern->as.alt.alternatives) {
        for (u64 i = 0; i < vec_len(pattern->as.alt.alternatives); ++i) {
          emit_match_pattern(codegen, vec_at_ptr(pattern->as.alt.alternatives, i),
                             subject_expr, subject_type, arm, result_name, done_label);
        }
      }
      return;
    case PAT_PATH: {
      EnumVariant* variant = NULL;
      Symbol* enum_symbol =
          sema_lookup_enum_variant(codegen->sema, pattern->as.path.path, &variant);
      if (!enum_symbol || !variant) {
        cg_error(codegen, TAKA_MSG_C_MATCH_PATTERN_DOES_NOT_NAME_AN_ENUM_VARIANT);
        return;
      }
      bool payload = enum_has_payload(enum_symbol->as.enum_decl);
      if (payload) {
        fprintf(codegen->out, "if (%s.tag == TAKA_", subject_expr);
        emit_string(codegen->out, enum_symbol->name);
        fputc('_', codegen->out);
        emit_string(codegen->out, variant->name);
        fputs(") {\n", codegen->out);
      } else {
        fprintf(codegen->out, "if (%s == TAKA_", subject_expr);
        emit_string(codegen->out, enum_symbol->name);
        fputc('_', codegen->out);
        emit_string(codegen->out, variant->name);
        fputs(") {\n", codegen->out);
        if (pattern->as.path.has_tuple || pattern->as.path.has_struct) {
          cg_error(codegen, TAKA_MSG_C_LIKE_ENUM_VARIANTS_HAVE_NO_PAYLOAD_PATTERNS);
          fputs("}\n", codegen->out);
          return;
        }
        emit_match_leaf(codegen, arm, result_name, done_label);
        fputs("}\n", codegen->out);
        return;
      }

      Pattern* conditional = NULL;
      Type* conditional_type = NULL;
      char conditional_expr[1024] = {0};

      if (pattern->as.path.has_tuple && pattern->as.path.tuple) {
        for (u64 i = 0; i < vec_len(pattern->as.path.tuple); ++i) {
          Pattern* child = vec_at_ptr(pattern->as.path.tuple, i);
          if (!child) continue;
          Type* child_type = NULL;
          if (variant->types && i < vec_len(variant->types)) {
            child_type = vec_at_ptr(variant->types, i);
          }
          char value_expr[1024];
          snprintf(value_expr, sizeof(value_expr), "%s.as.u_%.*s.f%llu", subject_expr,
                   str8_fmt(variant->name), (unsigned long long)i);
          if (child->kind == PAT_BIND || child->kind == PAT_REF_BIND) {
            emit_pattern_binding(codegen, child, value_expr, child_type);
          } else if (child->kind != PAT_WILDCARD) {
            if (conditional) {
              cg_error(codegen, TAKA_MSG_C_MULTIPLE_CONDITIONAL_TUPLE_SUB_PATTERNS_ARE_NOT_SUPPORTED);
              fputs("}\n", codegen->out);
              return;
            }
            conditional = child;
            conditional_type = child_type;
            snprintf(conditional_expr, sizeof(conditional_expr), "%s", value_expr);
          }
        }
      } else if (pattern->as.path.has_struct && pattern->as.path.fields) {
        for (u64 i = 0; i < vec_len(pattern->as.path.fields); ++i) {
          FieldPat* field = vec_at_ptr(pattern->as.path.fields, i);
          if (!field || str8_eq(field->name, str8_lit(".."))) continue;
          Type* field_type = find_variant_field_type(variant, field->name);
          if (!field_type) {
            cg_error(codegen, TAKA_MSG_C_UNKNOWN_ENUM_VARIANT_FIELD_S, str8_fmt(field->name));
          }
          char value_expr[1024];
          snprintf(value_expr, sizeof(value_expr), "%s.as.u_%.*s.f_%.*s", subject_expr,
                   str8_fmt(variant->name), str8_fmt(field->name));
          if (!field->has_pattern) {
            emit_named_binding(codegen, field->name, field_type, value_expr);
          } else if (field->pattern && (field->pattern->kind == PAT_BIND ||
                                        field->pattern->kind == PAT_REF_BIND)) {
            emit_pattern_binding(codegen, field->pattern, value_expr, field_type);
          } else if (field->pattern && field->pattern->kind != PAT_WILDCARD) {
            if (conditional) {
              cg_error(codegen, TAKA_MSG_C_MULTIPLE_CONDITIONAL_STRUCT_SUB_PATTERNS_ARE_NOT_SUPPORTED);
              fputs("}\n", codegen->out);
              return;
            }
            conditional = field->pattern;
            conditional_type = field_type;
            snprintf(conditional_expr, sizeof(conditional_expr), "%s", value_expr);
          }
        }
      }

      if (conditional) {
        emit_match_pattern(codegen, conditional, conditional_expr, conditional_type, arm,
                           result_name, done_label);
      } else {
        emit_match_leaf(codegen, arm, result_name, done_label);
      }
      fputs("}\n", codegen->out);
      return;
    }
  }
}

static bool emit_match_expr(Codegen* codegen, Expr* expr) {
  Type* result_type = sema_expr_type(codegen->sema, expr);
  if (!result_type) {
    cg_error(codegen, TAKA_MSG_C_CANNOT_INFER_THE_MATCH_RESULT_TYPE);
    return FALSE;
  }
  u64 id = next_temp(codegen);
  char subject_name[64];
  char result_name[64];
  char done_label[64];
  snprintf(subject_name, sizeof(subject_name), "__taka_match_%llu",
           (unsigned long long)id);
  snprintf(result_name, sizeof(result_name), "__taka_result_%llu",
           (unsigned long long)id);
  snprintf(done_label, sizeof(done_label), "__taka_done_%llu",
           (unsigned long long)id);
  fputs("({ __auto_type ", codegen->out);
  fputs(subject_name, codegen->out);
  fputs(" = ", codegen->out);
  if (!emit_expr(codegen, expr->as.match.subject)) return FALSE;
  fputs("; ", codegen->out);
  if (!emit_type(codegen, result_type)) return FALSE;
  fputc(' ', codegen->out);
  fputs(result_name, codegen->out);
  if (result_type->kind == TYPE_PATH) {
    fputs(" = {0}", codegen->out);
  } else if (result_type->pointer_depth > 0 || result_type->is_reference) {
    fputs(" = NULL", codegen->out);
  } else {
    fputs(" = 0", codegen->out);
  }
  fputc(';', codegen->out);
  fputc('\n', codegen->out);
  if (expr->as.match.arms) {
    for (u64 i = 0; i < vec_len(expr->as.match.arms); ++i) {
      MatchArm* arm = vec_at_ptr(expr->as.match.arms, i);
      if (!arm) continue;
      emit_match_pattern(codegen, arm->pattern, subject_name,
                         sema_expr_type(codegen->sema, expr->as.match.subject), arm,
                         result_name, done_label);
    }
  }
  fprintf(codegen->out, "%s: ;\n", done_label);
  fputs(result_name, codegen->out);
  fputs("; })", codegen->out);
  return codegen->errors == 0;
}

static bool emit_expr_raw(Codegen* codegen, Expr* expr) {
  if (!expr) {
    fputs("0", codegen->out);
    return FALSE;
  }
  switch (expr->kind) {
    case EXPR_NUMBER:
      fprintf(codegen->out, "%lld", (long long)expr->as.number.value);
      return TRUE;
    case EXPR_FLOAT: {
      Type* type = sema_expr_type(codegen->sema, expr);
      bool is_f32 = type && type->kind == TYPE_PRIMITIVE &&
                    str8_eq(type->as.primitive.name, str8_lit("f32"));
      fprintf(codegen->out, "%.17g%s", expr->as.float_lit.value, is_f32 ? "f" : "");
      return TRUE;
    }
    case EXPR_STRING:
      fputs("taka_sview_from_cstr(", codegen->out);
      emit_string(codegen->out, expr->as.string.value);
      fputc(')', codegen->out);
      return TRUE;
    case EXPR_CHAR:
      emit_string(codegen->out, expr->as.char_lit.value);
      return TRUE;
    case EXPR_BOOL:
      fputs(expr->as.boolean.value ? "true" : "false", codegen->out);
      return TRUE;
    case EXPR_NULL:
      fputs("NULL", codegen->out);
      return TRUE;
    case EXPR_UNDEF:
      cg_error(codegen, TAKA_MSG_C_UNDEF_IS_ONLY_SUPPORTED_AS_A_DECLARATION_INITIALIZER);
      fputs("0", codegen->out);
      return FALSE;
    case EXPR_SYMBOL: {
      Symbol* symbol = sema_expr_symbol(codegen->sema, expr);
      return emit_symbol_raw_value(codegen, symbol);
    }
    case EXPR_PATH: {
      EnumVariant* variant = NULL;
      Symbol* enum_symbol =
          sema_lookup_enum_variant(codegen->sema, expr->as.path.path, &variant);
      if (enum_symbol && variant) {
        if (variant->kind == VARIANT_UNIT || variant->kind == VARIANT_VALUE) {
          return emit_enum_unit_value(codegen, enum_symbol, variant);
        }
        cg_error(codegen, TAKA_MSG_C_ENUM_VARIANT_S_REQUIRES_A_PAYLOAD, str8_fmt(variant->name));
        return FALSE;
      }
      return emit_symbol_name_value(codegen, expr);
    }
    case EXPR_BINARY: {
      fputc('(', codegen->out);
      if (!emit_expr(codegen, expr->as.bin.lhs)) return FALSE;
      fputc(' ', codegen->out);
      emit_string(codegen->out, expr->as.bin.oper.value);
      fputc(' ', codegen->out);
      if (!emit_expr(codegen, expr->as.bin.rhs)) return FALSE;
      fputc(')', codegen->out);
      return TRUE;
    }
    case EXPR_TERNARY:
      fputs("(", codegen->out);
      if (!emit_expr(codegen, expr->as.ternary.cond)) return FALSE;
      fputs(" ? ", codegen->out);
      if (!emit_expr(codegen, expr->as.ternary.then_expr)) return FALSE;
      fputs(" : ", codegen->out);
      if (!emit_expr(codegen, expr->as.ternary.else_expr)) return FALSE;
      fputc(')', codegen->out);
      return TRUE;
    case EXPR_PREFIX: {
      TokenKind kind = expr->as.prefix.oper.kind;
      if (kind == TOKEN_AMP) {
        return emit_address_of(codegen, expr->as.prefix.right_expr);
      }
      if (kind == TOKEN_STAR) {
        fputs("(*", codegen->out);
        if (!emit_ptr_value(codegen, expr->as.prefix.right_expr)) return FALSE;
        fputc(')', codegen->out);
        return TRUE;
      }
      if (kind == TOKEN_PLUS_PLUS || kind == TOKEN_MINUS_MINUS) {
        fputs(kind == TOKEN_PLUS_PLUS ? "++(" : "--(", codegen->out);
        if (!emit_lvalue(codegen, expr->as.prefix.right_expr)) return FALSE;
        fputc(')', codegen->out);
        return TRUE;
      }
      fputc('(', codegen->out);
      emit_string(codegen->out, expr->as.prefix.oper.value);
      fputc('(', codegen->out);
      if (!emit_expr(codegen, expr->as.prefix.right_expr)) return FALSE;
      fputs("))", codegen->out);
      return TRUE;
    }
    case EXPR_POSTFIX:
      fputs("((", codegen->out);
      if (!emit_lvalue(codegen, expr->as.postfix.expr)) return FALSE;
      fputs(")", codegen->out);
      emit_string(codegen->out, expr->as.postfix.oper.value);
      fputc(')', codegen->out);
      return TRUE;
    case EXPR_ASSIGNMENT:
      fputs("(", codegen->out);
      if (!emit_lvalue(codegen, expr->as.assignment.assigne_expr)) return FALSE;
      fputc(' ', codegen->out);
      emit_string(codegen->out, expr->as.assignment.oper.value);
      fputc(' ', codegen->out);
      if (!emit_expr(codegen, expr->as.assignment.rhs)) return FALSE;
      fputc(')', codegen->out);
      return TRUE;
    case EXPR_CAST:
      fputs("((", codegen->out);
      if (!emit_type(codegen, expr->as.cast.type)) return FALSE;
      fputs(")(", codegen->out);
      if (!emit_expr(codegen, expr->as.cast.expr)) return FALSE;
      fputs("))", codegen->out);
      return TRUE;
    case EXPR_CALL:
      return emit_call(codegen, expr);
    case EXPR_INDEX: {
      Type* object_type = sema_expr_type(codegen->sema, expr->as.index.object);
      fputc('(', codegen->out);
      if (object_type && (object_type->pointer_depth > 0 || object_type->is_reference)) {
        if (!emit_ptr_value(codegen, expr->as.index.object)) return FALSE;
      } else {
        if (!emit_lvalue(codegen, expr->as.index.object)) return FALSE;
      }
      fputc(')', codegen->out);
      fputc('[', codegen->out);
      if (!emit_expr(codegen, expr->as.index.index)) return FALSE;
      fputc(']', codegen->out);
      return TRUE;
    }
    case EXPR_FIELD: {
      Type* object_type = sema_expr_type(codegen->sema, expr->as.field.object);
      bool indirect = object_type && (object_type->pointer_depth > 0 ||
                                      object_type->is_reference);
      fputc('(', codegen->out);
      if (indirect) {
        if (!emit_ptr_value(codegen, expr->as.field.object)) return FALSE;
      } else if (!emit_expr(codegen, expr->as.field.object)) {
        return FALSE;
      }
      fputc(')', codegen->out);
      fputs(indirect ? "->" : ".", codegen->out);
      emit_field_name(codegen->out, expr->as.field.field);
      return TRUE;
    }
    case EXPR_STRUCT_LIT:
      return emit_struct_lit_body(codegen, expr, false);
    case EXPR_ARRAY_LIT: {
      Type* type = sema_expr_type(codegen->sema, expr);
      if (!type || type->kind != TYPE_ARRAY || !type->as.array.element) {
        cg_error(codegen, TAKA_MSG_C_CANNOT_INFER_ARRAY_LITERAL_TYPE);
        return FALSE;
      }
      fputc('(', codegen->out);
      if (!emit_type(codegen, type->as.array.element)) return FALSE;
      fputc('[', codegen->out);
      if (expr->as.array_lit.is_fill && type->as.array.length) {
        if (!emit_array_length(codegen, type->as.array.length)) return FALSE;
      } else {
        fprintf(codegen->out, "%llu", (unsigned long long)vec_len(expr->as.array_lit.items));
      }
      fputs("])", codegen->out);
      return emit_array_literal_body(codegen, expr, type);
    }
    case EXPR_SIZEOF:
      if (expr->as.sizeof_expr.is_type) {
        fputs("sizeof(", codegen->out);
        if (!emit_type(codegen, expr->as.sizeof_expr.type)) return FALSE;
        fputc(')', codegen->out);
      } else {
        fputs("sizeof(", codegen->out);
        if (!emit_expr(codegen, expr->as.sizeof_expr.expr)) return FALSE;
        fputc(')', codegen->out);
      }
      return TRUE;
    case EXPR_OFFSETOF:
      fputs("offsetof(", codegen->out);
      if (!emit_type(codegen, expr->as.offsetof_expr.type)) return FALSE;
      fputs(", ", codegen->out);
      emit_field_name(codegen->out, expr->as.offsetof_expr.field);
      fputc(')', codegen->out);
      return TRUE;
    case EXPR_LAMBDA:
      return emit_lambda_value(codegen, expr);
    case EXPR_IF:
      cg_error(codegen, TAKA_MSG_C_IF_EXPRESSIONS_ARE_NOT_SUPPORTED_BY_THE_C_BACKEND_YET);
      return FALSE;
    case EXPR_MATCH:
      return emit_match_expr(codegen, expr);
    case EXPR_PERFORM:
      cg_error(codegen, TAKA_MSG_C_EFFECT_OPERATIONS_ARE_NOT_SUPPORTED_BY_THE_C_BACKEND_YET);
      return FALSE;
    case EXPR_HANDLE:
      cg_error(codegen, TAKA_MSG_C_HANDLERS_ARE_NOT_SUPPORTED_BY_THE_C_BACKEND_YET);
      return FALSE;
    case EXPR_SPAWN:
      cg_error(codegen, TAKA_MSG_C_SPAWN_IS_NOT_SUPPORTED_BY_THE_C_BACKEND_YET);
      return FALSE;
    case EXPR_RESUME:
      cg_error(codegen, TAKA_MSG_C_RESUME_IS_NOT_SUPPORTED_BY_THE_C_BACKEND_YET);
      return FALSE;
    case EXPR_TRANSACT:
      cg_error(codegen, TAKA_MSG_C_TRANSACT_EXPRESSIONS_ARE_NOT_SUPPORTED_BY_THE_C_BACKEND_YET);
      return FALSE;
    case EXPR_BLOCK:
      cg_error(codegen, TAKA_MSG_C_BLOCK_EXPRESSIONS_ARE_NOT_SUPPORTED_BY_THE_C_BACKEND_YET);
      return FALSE;
  }
  return FALSE;
}

static bool emit_expr(Codegen* codegen, Expr* expr) {
  if (!expr) {
    return FALSE;
  }
  if (expression_is_reference(codegen->sema, expr)) {
    fputs("(*", codegen->out);
    if (!emit_expr_raw(codegen, expr)) {
      return FALSE;
    }
    fputc(')', codegen->out);
    return TRUE;
  }
  return emit_expr_raw(codegen, expr);
}

static bool emit_statement_expr(Codegen* codegen, Expr* expr) {
  if (!expr) {
    return FALSE;
  }
  if (expr->kind == EXPR_ASSIGNMENT || expr->kind == EXPR_POSTFIX ||
      (expr->kind == EXPR_PREFIX &&
       (expr->as.prefix.oper.kind == TOKEN_PLUS_PLUS ||
        expr->as.prefix.oper.kind == TOKEN_MINUS_MINUS))) {
    return emit_expr_raw(codegen, expr);
  }
  return emit_expr(codegen, expr);
}

static bool emit_lambda_value(Codegen* codegen, Expr* expr) {
  LambdaInfo* info = map_get_ptr(codegen->lambda_map, (u64)(uintptr_t)expr);
  if (!info) {
    cg_error(codegen, TAKA_MSG_C_LAMBDA_WAS_NOT_COLLECTED);
    return FALSE;
  }
  u64 closure_id = info->closure_id;
  fprintf(codegen->out, "(taka_closure_%llu){ .env = ",
          (unsigned long long)closure_id);
  if (!info->has_captures) {
    fputs("NULL", codegen->out);
  } else {
    fprintf(codegen->out, "&(%s){ ", info->env_name);
    if (expr->as.lambda.captures) {
      for (u64 i = 0; i < vec_len(expr->as.lambda.captures); ++i) {
        Capture* capture = vec_at_ptr(expr->as.lambda.captures, i);
        if (!capture) continue;
        Symbol* symbol = sema_capture_symbol(codegen->sema, capture);
        Symbol* outer = symbol ? symbol->capture_outer : NULL;
        fputs(".", codegen->out);
        emit_field_name(codegen->out, capture->name);
        fputs(" = ", codegen->out);
        if (capture->by_ref) {
          if (!outer) {
            cg_error(codegen, TAKA_MSG_C_CAPTURE_S_HAS_NO_OUTER_BINDING, str8_fmt(capture->name));
            return FALSE;
          }
          if (outer->type && outer->type->is_reference) {
            if (!emit_symbol_raw_value(codegen, outer)) return FALSE;
          } else {
            fputs("&(", codegen->out);
            if (!emit_symbol_raw_value(codegen, outer)) return FALSE;
            fputc(')', codegen->out);
          }
        } else if (capture->has_init) {
          if (!emit_expr(codegen, capture->init)) return FALSE;
        } else if (outer) {
          if (outer->type && outer->type->is_reference) {
            fputs("(*", codegen->out);
            if (!emit_symbol_raw_value(codegen, outer)) return FALSE;
            fputc(')', codegen->out);
          } else if (!emit_symbol_raw_value(codegen, outer)) {
            return FALSE;
          }
        } else {
          cg_error(codegen, TAKA_MSG_C_CAPTURE_S_HAS_NO_OUTER_BINDING, str8_fmt(capture->name));
          return FALSE;
        }
        fputs(", ", codegen->out);
      }
    }
    fputc('}', codegen->out);
  }
  fprintf(codegen->out, ", .call = %s }", info->fn_name);
  return TRUE;
}

static bool expr_has_own_parens(Expr* expr) {
  if (!expr) {
    return FALSE;
  }
  switch (expr->kind) {
    case EXPR_BINARY:
    case EXPR_TERNARY:
    case EXPR_PREFIX:
    case EXPR_POSTFIX:
    case EXPR_ASSIGNMENT:
    case EXPR_CAST:
      return TRUE;
    default:
      return FALSE;
  }
}

static bool emit_condition_expr(Codegen* codegen, Expr* expr) {
  if (expr_has_own_parens(expr)) {
    return emit_expr(codegen, expr);
  }
  fputc('(', codegen->out);
  if (!emit_expr(codegen, expr)) {
    return FALSE;
  }
  fputc(')', codegen->out);
  return TRUE;
}

static void emit_expr_statement(Codegen* codegen, Expr* expr) {
  if (emit_statement_expr(codegen, expr)) {
    fputs(";\n", codegen->out);
  }
}

static void emit_var_decl(Codegen* codegen, Stmt* stmt, bool emit_semicolon);

static void emit_defer_scope(Codegen* codegen, bool is_loop) {
  DeferScope* scope = PUSH_STRUCT_ZERO(codegen->arena, DeferScope);
  scope->statements = vec_new(codegen->arena, sizeof(Stmt*), 0);
  scope->is_loop = is_loop;
  vec_push_ptr(codegen->defer_scopes, scope);
}

static void emit_block(Codegen* codegen, Stmt* block, bool function_body, Type* return_type,
                       bool loop_body);
static void emit_stmt(Codegen* codegen, Stmt* stmt, Type* return_type);
static void emit_scope_cleanup(Codegen* codegen, DeferScope* scope);

static u64 next_temp(Codegen* codegen) {
  return codegen->temp_counter++;
}

static LoopContext* find_loop(Codegen* codegen, String8 label) {
  for (u64 i = vec_len(codegen->loop_stack); i > 0; --i) {
    LoopContext* context = vec_at_ptr(codegen->loop_stack, i - 1);
    if (context && context->has_label && str8_eq(context->label, label)) {
      return context;
    }
  }
  return NULL;
}

static void emit_cleanup_through(Codegen* codegen, DeferScope* target) {
  for (u64 i = vec_len(codegen->defer_scopes); i > 0; --i) {
    DeferScope* scope = vec_at_ptr(codegen->defer_scopes, i - 1);
    emit_scope_cleanup(codegen, scope);
    if (scope == target) {
      return;
    }
  }
}

static void emit_loop_body(Codegen* codegen, Stmt* body, Type* return_type,
                           LoopContext* context) {
  fputs("{\n", codegen->out);
  emit_defer_scope(codegen, true);
  context->body_scope = vec_at_ptr(codegen->defer_scopes,
                                   vec_len(codegen->defer_scopes) - 1);
  if (body && body->kind == STMT_BLOCK) {
    if (body->as.block.body) {
      for (u64 i = 0; i < vec_len(body->as.block.body); ++i) {
        emit_stmt(codegen, vec_at_ptr(body->as.block.body, i), return_type);
      }
    }
    if (body->as.block.tail) {
      emit_expr_statement(codegen, body->as.block.tail);
    }
  } else if (body) {
    emit_stmt(codegen, body, return_type);
  }
  DeferScope* scope = vec_at_ptr(codegen->defer_scopes,
                                 vec_len(codegen->defer_scopes) - 1);
  emit_scope_cleanup(codegen, scope);
  if (context->has_label) {
    fprintf(codegen->out, "%s: ;\n", context->continue_label);
  }
  vec_pop(codegen->defer_scopes);
  fputs("}\n", codegen->out);
}

static bool emit_for_each_binding(Codegen* codegen, Pattern* pattern, Type* element,
                                  const char* iterator_name, const char* index_name) {
  if (!pattern) {
    return TRUE;
  }
  if (pattern->kind == PAT_WILDCARD) {
    return TRUE;
  }
  if (!element) {
    cg_error(codegen, TAKA_MSG_C_CANNOT_INFER_THE_FOR_EACH_ELEMENT_TYPE);
    return FALSE;
  }
  if (pattern->kind == PAT_BIND) {
    fputs("TAKA_UNUSED ", codegen->out);
    if (!emit_type(codegen, element)) return FALSE;
    fputc(' ', codegen->out);
    emit_local_name(codegen->out, pattern->as.bind.name);
    fprintf(codegen->out, " = %s[%s];\n", iterator_name, index_name);
    return TRUE;
  }
  if (pattern->kind == PAT_REF_BIND) {
    fputs("TAKA_UNUSED ", codegen->out);
    if (!emit_type(codegen, element)) return FALSE;
    fputc('*', codegen->out);
    fputc(' ', codegen->out);
    emit_local_name(codegen->out, pattern->as.bind.name);
    fprintf(codegen->out, " = &%s[%s];\n", iterator_name, index_name);
    return TRUE;
  }
  cg_error(codegen, TAKA_MSG_C_COMPLEX_FOR_EACH_PATTERNS_ARE_NOT_SUPPORTED_BY_THE_C_BACKEND_YET);
  return FALSE;
}

static void emit_defer_body(Codegen* codegen, Stmt* deferred) {
  if (!deferred) {
    return;
  }
  if (deferred->as.defer_stmt.is_block) {
    emit_block(codegen, deferred->as.defer_stmt.block, false, NULL, false);
  } else {
    emit_stmt(codegen, deferred->as.defer_stmt.stmt, NULL);
  }
}

static void emit_scope_cleanup(Codegen* codegen, DeferScope* scope) {
  if (!scope) {
    return;
  }
  for (u64 i = vec_len(scope->statements); i > 0; --i) {
    emit_defer_body(codegen, vec_at_ptr(scope->statements, i - 1));
  }
}

static void emit_cleanup_all(Codegen* codegen, bool stop_at_loop) {
  for (u64 i = vec_len(codegen->defer_scopes); i > 0; --i) {
    DeferScope* scope = vec_at_ptr(codegen->defer_scopes, i - 1);
    emit_scope_cleanup(codegen, scope);
    if (stop_at_loop && scope && scope->is_loop) {
      return;
    }
  }
}

static void emit_block(Codegen* codegen, Stmt* block, bool function_body, Type* return_type,
                       bool loop_body);

static void emit_stmt(Codegen* codegen, Stmt* stmt, Type* return_type);

static void emit_var_decl(Codegen* codegen, Stmt* stmt, bool emit_semicolon) {
  Type* type = stmt->as.var_expr.type;
  if (!type) {
    cg_error(codegen, TAKA_MSG_C_VARIABLE_S_HAS_NO_RESOLVED_TYPE,
             str8_fmt(stmt->as.var_expr.var_name));
    return;
  }
  fputs("TAKA_UNUSED ", codegen->out);
  if (type->kind == TYPE_ARRAY) {
    Type* element = type->as.array.element;
    if (type->is_reference || type->pointer_depth > 0) {
      cg_error(codegen, TAKA_MSG_C_ARRAYS_OF_POINTERS_ARE_NOT_SUPPORTED_BY_THE_C_BACKEND);
      return;
    }
    if (type->is_const) {
      fputs("const ", codegen->out);
    }
    if (!emit_type(codegen, element)) {
      return;
    }
    fputc(' ', codegen->out);
    emit_local_name(codegen->out, stmt->as.var_expr.var_name);
    fputc('[', codegen->out);
    if (!emit_array_length(codegen, type->as.array.length)) {
      return;
    }
    fputc(']', codegen->out);
    if (stmt->as.var_expr.assigned_value.kind != EXPR_UNDEF) {
      fputs(" = ", codegen->out);
      if (!emit_initializer(codegen, type, &stmt->as.var_expr.assigned_value)) {
        return;
      }
    }
  } else {
    if (!emit_type(codegen, type)) {
      return;
    }
    fputc(' ', codegen->out);
    emit_local_name(codegen->out, stmt->as.var_expr.var_name);
    if (stmt->as.var_expr.assigned_value.kind != EXPR_UNDEF) {
      fputs(" = ", codegen->out);
      if (!emit_initializer(codegen, type, &stmt->as.var_expr.assigned_value)) {
        return;
      }
    }
  }
  if (emit_semicolon) {
    fputs(";\n", codegen->out);
  }
}

static bool emit_global_var(Codegen* codegen, Stmt* stmt) {
  Type* type = stmt->as.var_expr.type;
  if (!type) {
    cg_error(codegen, TAKA_MSG_C_GLOBAL_S_HAS_NO_RESOLVED_TYPE,
             str8_fmt(stmt->as.var_expr.var_name));
    return FALSE;
  }
  if (type->kind == TYPE_ARRAY) {
    Type* element = type->as.array.element;
    if (!emit_type(codegen, element)) return FALSE;
    fputc(' ', codegen->out);
    emit_global_name(codegen->out, stmt->as.var_expr.var_name);
    fputc('[', codegen->out);
    if (!emit_array_length(codegen, type->as.array.length)) return FALSE;
    fputc(']', codegen->out);
    if (stmt->as.var_expr.assigned_value.kind != EXPR_UNDEF) {
      fputs(" = ", codegen->out);
      if (!emit_initializer(codegen, type, &stmt->as.var_expr.assigned_value)) return FALSE;
    }
    fputs(";\n", codegen->out);
    return TRUE;
  }
  if (!emit_type(codegen, type)) return FALSE;
  fputc(' ', codegen->out);
  emit_global_name(codegen->out, stmt->as.var_expr.var_name);
  if (stmt->as.var_expr.assigned_value.kind != EXPR_UNDEF) {
    fputs(" = ", codegen->out);
    if (!emit_initializer(codegen, type, &stmt->as.var_expr.assigned_value)) return FALSE;
  }
  fputs(";\n", codegen->out);
  return TRUE;
}

static void emit_globals(Codegen* codegen) {
  Vector* body = sema_program(codegen->sema)->as.block.body;
  for (u64 i = 0; i < vec_len(body); ++i) {
    Stmt* stmt = vec_at_ptr(body, i);
    if (!stmt) {
      continue;
    }
    if (stmt->kind == STMT_VAR) {
      emit_global_var(codegen, stmt);
    } else if (stmt->kind == STMT_ITEM && stmt->as.item.item &&
               stmt->as.item.item->kind == ITEM_GLOBAL) {
      emit_global_declaration(codegen, stmt->as.item.item->as.global.declaration);
    }
  }
  fputc('\n', codegen->out);
}

static void emit_if_stmt(Codegen* codegen, Stmt* stmt, Type* return_type) {
  Vector* branches = stmt->as.if_stmt.branches;
  for (u64 i = 0; branches && i < vec_len(branches); ++i) {
    IfBranch* branch = vec_at_ptr(branches, i);
    if (!branch) {
      continue;
    }
    if (branch->condition.is_binding) {
      cg_error(codegen, TAKA_MSG_C_FAILABLE_BINDINGS_ARE_NOT_SUPPORTED_BY_THE_C_BACKEND_YET);
      return;
    }
    fputs(i == 0 ? "if " : "else if ", codegen->out);
    if (!emit_condition_expr(codegen, branch->condition.expr)) return;
    fputc(' ', codegen->out);
    emit_block(codegen, branch->body, false, return_type, false);
    fputc(' ', codegen->out);
  }
  if (stmt->as.if_stmt.else_block) {
    fputs("else ", codegen->out);
    emit_block(codegen, stmt->as.if_stmt.else_block, false, return_type, false);
  }
  fputc('\n', codegen->out);
}

static void begin_loop_context(Codegen* codegen, LoopContext* context, String8 label,
                               bool has_label) {
  memset(context, 0, sizeof(*context));
  context->label = label;
  context->has_label = has_label;
  if (has_label) {
    u64 id = next_temp(codegen);
    snprintf(context->break_label, sizeof(context->break_label), "taka_break_%llu",
             (unsigned long long)id);
    snprintf(context->continue_label, sizeof(context->continue_label),
             "taka_continue_%llu", (unsigned long long)id);
  }
  vec_push_ptr(codegen->loop_stack, context);
}

static void end_loop_context(Codegen* codegen, LoopContext* context) {
  vec_pop(codegen->loop_stack);
  if (context->has_label) {
    fprintf(codegen->out, "%s: ;\n", context->break_label);
  }
}

static void emit_while_stmt(Codegen* codegen, Stmt* stmt, Type* return_type, String8 label,
                            bool has_label) {
  if (stmt->as.while_stmt.condition.is_binding) {
    cg_error(codegen, TAKA_MSG_C_FAILABLE_BINDINGS_ARE_NOT_SUPPORTED_BY_THE_C_BACKEND_YET);
    return;
  }
  LoopContext context;
  begin_loop_context(codegen, &context, label, has_label);
  fputs("while ", codegen->out);
  if (!emit_condition_expr(codegen, stmt->as.while_stmt.condition.expr)) {
    end_loop_context(codegen, &context);
    return;
  }
  fputc(' ', codegen->out);
  emit_loop_body(codegen, stmt->as.while_stmt.body, return_type, &context);
  fputs("\n", codegen->out);
  end_loop_context(codegen, &context);
}

static void emit_for_each_stmt(Codegen* codegen, Stmt* stmt, Type* return_type, String8 label,
                               bool has_label) {
  ForStmt* loop = &stmt->as.for_stmt.for_stmt;
  Type* iterable_type = sema_expr_type(codegen->sema, loop->iterable);
  if (!iterable_type || iterable_type->kind != TYPE_ARRAY) {
    cg_error(codegen, TAKA_MSG_C_FOR_EACH_CURRENTLY_REQUIRES_A_FIXED_ARRAY);
    return;
  }
  Type* element = iterable_type->as.array.element;
  if (!element) {
    cg_error(codegen, TAKA_MSG_C_FOR_EACH_CANNOT_INFER_THE_ARRAY_ELEMENT_TYPE);
    return;
  }
  u64 id = next_temp(codegen);
  char iterator_name[64];
  char index_name[64];
  snprintf(iterator_name, sizeof(iterator_name), "__taka_iter_%llu",
           (unsigned long long)id);
  snprintf(index_name, sizeof(index_name), "__taka_i_%llu", (unsigned long long)id);

  fputs("{\n", codegen->out);
  fputs("TAKA_UNUSED ", codegen->out);
  if (!emit_type(codegen, element)) {
    fputs("}\n", codegen->out);
    return;
  }
  fputs("* ", codegen->out);
  fputs(iterator_name, codegen->out);
  fputs(" = ", codegen->out);
  if (!emit_expr(codegen, loop->iterable)) {
    fputs("}\n", codegen->out);
    return;
  }
  fputs(";\n", codegen->out);
  fprintf(codegen->out, "for (uint64_t %s = 0; %s < ", index_name, index_name);
  if (!emit_expr(codegen, iterable_type->as.array.length)) {
    fputs(") {}\n}\n", codegen->out);
    return;
  }
  fprintf(codegen->out, "; ++%s) ", index_name);

  LoopContext context;
  begin_loop_context(codegen, &context, label, has_label);
  fputs("{\n", codegen->out);
  emit_defer_scope(codegen, true);
  context.body_scope = vec_at_ptr(codegen->defer_scopes,
                                  vec_len(codegen->defer_scopes) - 1);
  if (!emit_for_each_binding(codegen, loop->pattern, element, iterator_name, index_name)) {
    cg_error(codegen, TAKA_MSG_C_FAILED_TO_LOWER_FOR_EACH_BINDING);
  }
  Stmt* body = stmt->as.for_stmt.body;
  if (body && body->kind == STMT_BLOCK) {
    if (body->as.block.body) {
      for (u64 i = 0; i < vec_len(body->as.block.body); ++i) {
        emit_stmt(codegen, vec_at_ptr(body->as.block.body, i), return_type);
      }
    }
    if (body->as.block.tail) {
      emit_expr_statement(codegen, body->as.block.tail);
    }
  } else if (body) {
    emit_stmt(codegen, body, return_type);
  }
  DeferScope* scope = vec_at_ptr(codegen->defer_scopes,
                                 vec_len(codegen->defer_scopes) - 1);
  emit_scope_cleanup(codegen, scope);
  if (context.has_label) {
    fprintf(codegen->out, "%s: ;\n", context.continue_label);
  }
  vec_pop(codegen->defer_scopes);
  fputs("}\n", codegen->out);
  end_loop_context(codegen, &context);
  fputs("}\n", codegen->out);
}

static void emit_for_stmt(Codegen* codegen, Stmt* stmt, Type* return_type, String8 label,
                          bool has_label) {
  ForStmt* loop = &stmt->as.for_stmt.for_stmt;
  if (loop->is_each) {
    emit_for_each_stmt(codegen, stmt, return_type, label, has_label);
    return;
  }
  LoopContext context;
  begin_loop_context(codegen, &context, label, has_label);
  fputs("for (", codegen->out);
  if (loop->init) {
    if (loop->init->kind == STMT_VAR) {
      emit_var_decl(codegen, loop->init, false);
    } else if (loop->init->kind == STMT_EXPRESION) {
      if (!emit_statement_expr(codegen, &loop->init->as.expresion.expr)) {
        end_loop_context(codegen, &context);
        return;
      }
    }
  }
  fputs("; ", codegen->out);
  if (loop->cond) {
    if (!emit_expr(codegen, loop->cond)) {
      end_loop_context(codegen, &context);
      return;
    }
  }
  fputs("; ", codegen->out);
  if (loop->step && loop->step->kind == STMT_EXPRESION) {
    if (!emit_statement_expr(codegen, &loop->step->as.expresion.expr)) {
      end_loop_context(codegen, &context);
      return;
    }
  }
  fputs(") ", codegen->out);
  emit_loop_body(codegen, stmt->as.for_stmt.body, return_type, &context);
  end_loop_context(codegen, &context);
}

static void emit_stmt(Codegen* codegen, Stmt* stmt, Type* return_type) {
  if (!stmt) {
    return;
  }
  switch (stmt->kind) {
    case STMT_VAR:
      emit_var_decl(codegen, stmt, true);
      return;
    case STMT_EXPRESION:
      emit_expr_statement(codegen, &stmt->as.expresion.expr);
      return;
    case STMT_BLOCK:
      emit_block(codegen, stmt, false, return_type, false);
      return;
    case STMT_DEFER: {
      if (vec_len(codegen->defer_scopes) == 0) {
        cg_error(codegen, TAKA_MSG_C_DEFER_OUTSIDE_A_BLOCK);
        return;
      }
      DeferScope* scope = vec_at_ptr(codegen->defer_scopes, vec_len(codegen->defer_scopes) - 1);
      if (scope) {
        vec_push_ptr(scope->statements, stmt);
      }
      return;
    }
    case STMT_IF:
      emit_if_stmt(codegen, stmt, return_type);
      return;
    case STMT_WHILE:
      emit_while_stmt(codegen, stmt, return_type, (String8){0}, FALSE);
      return;
    case STMT_FOR:
      emit_for_stmt(codegen, stmt, return_type, (String8){0}, FALSE);
      return;
    case STMT_BREAK:
      if (stmt->as.break_stmt.has_label) {
        LoopContext* target = find_loop(codegen, stmt->as.break_stmt.label);
        if (!target) {
          cg_error(codegen, TAKA_MSG_C_UNKNOWN_BREAK_LABEL_S,
                   str8_fmt(stmt->as.break_stmt.label));
          return;
        }
        emit_cleanup_through(codegen, target->body_scope);
        fprintf(codegen->out, "goto %s;\n", target->break_label);
        return;
      }
      emit_cleanup_all(codegen, true);
      fputs("break;\n", codegen->out);
      return;
    case STMT_CONTINUE:
      if (stmt->as.continue_stmt.has_label) {
        LoopContext* target = find_loop(codegen, stmt->as.continue_stmt.label);
        if (!target) {
          cg_error(codegen, TAKA_MSG_C_UNKNOWN_CONTINUE_LABEL_S,
                   str8_fmt(stmt->as.continue_stmt.label));
          return;
        }
        emit_cleanup_through(codegen, target->body_scope);
        fprintf(codegen->out, "goto %s;\n", target->continue_label);
        return;
      }
      emit_cleanup_all(codegen, true);
      fputs("continue;\n", codegen->out);
      return;
    case STMT_RETURN:
      emit_cleanup_all(codegen, false);
      if (stmt->as.return_stmt.has_value) {
        fputs("return ", codegen->out);
        if (!emit_expr(codegen, stmt->as.return_stmt.value)) return;
        fputs(";\n", codegen->out);
      } else {
        fputs("return;\n", codegen->out);
      }
      return;
    case STMT_LABEL: {
      Stmt* inner = stmt->as.label_stmt.stmt;
      if (inner && inner->kind == STMT_FOR) {
        emit_for_stmt(codegen, inner, return_type, stmt->as.label_stmt.label, TRUE);
        return;
      }
      if (inner && inner->kind == STMT_WHILE) {
        emit_while_stmt(codegen, inner, return_type, stmt->as.label_stmt.label, TRUE);
        return;
      }
      cg_error(codegen, TAKA_MSG_C_LABELS_ARE_ONLY_SUPPORTED_ON_LOOPS_BY_THE_C_BACKEND);
      return;
    }
    case STMT_ITEM:
      cg_error(codegen, TAKA_MSG_C_NESTED_ITEMS_ARE_NOT_SUPPORTED_BY_THE_C_BACKEND);
      return;
  }
}

static void emit_block(Codegen* codegen, Stmt* block, bool function_body, Type* return_type,
                       bool loop_body) {
  if (!block || block->kind != STMT_BLOCK) {
    cg_error(codegen, TAKA_MSG_C_EXPECTED_A_BLOCK);
    return;
  }
  fputs("{\n", codegen->out);
  emit_defer_scope(codegen, loop_body);
  if (block->as.block.body) {
    for (u64 i = 0; i < vec_len(block->as.block.body); ++i) {
      emit_stmt(codegen, vec_at_ptr(block->as.block.body, i), return_type);
    }
  }
  if (block->as.block.tail) {
    if (function_body && return_type) {
      fputs("return ", codegen->out);
      emit_expr(codegen, block->as.block.tail);
      fputs(";\n", codegen->out);
    } else {
      emit_expr_statement(codegen, block->as.block.tail);
    }
  }
  DeferScope* scope = vec_at_ptr(codegen->defer_scopes, vec_len(codegen->defer_scopes) - 1);
  emit_scope_cleanup(codegen, scope);
  vec_pop(codegen->defer_scopes);
  fputs("}\n", codegen->out);
}

static bool emit_extern_prototype(Codegen* codegen, Item* item) {
  FnSig* sig = &item->as.extern_decl.sig;
  char name[512];
  snprintf(name, sizeof(name), "%.*s", str8_fmt(sig->name));
  if (!emit_fn_signature(codegen, sig, name, FALSE, FALSE, TRUE)) {
    return FALSE;
  }
  fputs(";\n", codegen->out);
  return TRUE;
}

static bool emit_c_named_prototype(Codegen* codegen, FnSig* sig, const char* name) {
  bool hidden = sig_needs_arena(sig);
  fputs("static TAKA_UNUSED ", codegen->out);
  if (!emit_fn_signature(codegen, sig, name, FALSE, hidden, FALSE)) {
    return FALSE;
  }
  fputs(";\n", codegen->out);
  return TRUE;
}

static bool emit_prototypes_v2(Codegen* codegen) {
  Vector* body = sema_program(codegen->sema)->as.block.body;
  for (u64 i = 0; i < vec_len(body); ++i) {
    Stmt* stmt = vec_at_ptr(body, i);
    if (!stmt || stmt->kind != STMT_ITEM || !stmt->as.item.item) {
      continue;
    }
    Item* item = stmt->as.item.item;
    if (item->kind == ITEM_EXTERN) {
      if (!emit_extern_prototype(codegen, item)) return FALSE;
    }
  }
  for (u64 i = 0; i < vec_len(body); ++i) {
    Stmt* stmt = vec_at_ptr(body, i);
    if (!stmt || stmt->kind != STMT_ITEM || !stmt->as.item.item) {
      continue;
    }
    Item* item = stmt->as.item.item;
    if (item->kind == ITEM_FN) {
      FnSig* sig = &item->as.fn.sig;
      bool hidden = sig_needs_arena(sig);
      char name[512];
      snprintf(name, sizeof(name), "taka_%.*s", str8_fmt(sig->name));
      if (item->visibility != VIS_PUB) {
        fputs("static ", codegen->out);
      }
      if (!emit_fn_signature(codegen, sig, name, FALSE, hidden, FALSE)) return FALSE;
      fputs(";\n", codegen->out);
    }
  }
  for (u64 i = 0; i < vec_len(body); ++i) {
    Stmt* stmt = vec_at_ptr(body, i);
    if (!stmt || stmt->kind != STMT_ITEM || !stmt->as.item.item) {
      continue;
    }
    Item* item = stmt->as.item.item;
    if (item->kind == ITEM_IMPL && item->as.impl_decl.methods) {
      ImplDecl* impl = &item->as.impl_decl;
      String8 type_name = path_first(impl->for_type && impl->for_type->kind == TYPE_PATH
                                         ? impl->for_type->as.path.path
                                         : (Path){0});
      for (u64 j = 0; j < vec_len(impl->methods); ++j) {
        FnDecl* method = vec_at_ptr(impl->methods, j);
        if (!method) continue;
        char name[512];
        snprintf(name, sizeof(name), "taka_%.*s_%.*s", str8_fmt(type_name),
                 str8_fmt(method->sig.name));
        if (!emit_c_named_prototype(codegen, &method->sig, name)) return FALSE;
      }
    }
  }
  fputc('\n', codegen->out);
  return TRUE;
}

static void emit_function_body_from_sig(Codegen* codegen, FnDecl* fn);

static bool emit_lambda_signature(Codegen* codegen, LambdaInfo* info, bool prototype) {
  Type* type = sema_expr_type(codegen->sema, info->expr);
  if (!type || type->kind != TYPE_FN) {
    cg_error(codegen, TAKA_MSG_C_LAMBDA_HAS_NO_FUNCTION_TYPE);
    return FALSE;
  }
  if (row_has_name(type->as.fn.effects, "alloc") ||
      row_has_name(type->as.fn.effects, "transacts")) {
    cg_error(codegen, TAKA_MSG_C_ALLOCATING_CLOSURES_ARE_NOT_SUPPORTED_BY_THE_C_BACKEND_YET);
    return FALSE;
  }
  fputs("static TAKA_UNUSED ", codegen->out);
  if (type_is_void(type->as.fn.return_type)) {
    fputs("void", codegen->out);
  } else if (!emit_type(codegen, type->as.fn.return_type)) {
    return FALSE;
  }
  fprintf(codegen->out, " %s(void* __taka_env", info->fn_name);
  if (info->expr->as.lambda.params) {
    for (u64 i = 0; i < vec_len(info->expr->as.lambda.params); ++i) {
      Param* param = vec_at_ptr(info->expr->as.lambda.params, i);
      if (!param) continue;
      fputs(", ", codegen->out);
      if (!emit_type(codegen, param->type)) return FALSE;
      fputc(' ', codegen->out);
      emit_local_name(codegen->out, param->name);
    }
  }
  fputc(')', codegen->out);
  if (prototype) {
    fputs(";\n", codegen->out);
  }
  return TRUE;
}

static void emit_lambda_prototypes(Codegen* codegen) {
  for (u64 i = 0; i < vec_len(codegen->lambdas); ++i) {
    LambdaInfo* info = vec_at_ptr(codegen->lambdas, i);
    if (info) {
      emit_lambda_signature(codegen, info, TRUE);
    }
  }
  fputc('\n', codegen->out);
}

static void emit_lambda_env_structs(Codegen* codegen) {
  for (u64 i = 0; i < vec_len(codegen->lambdas); ++i) {
    LambdaInfo* info = vec_at_ptr(codegen->lambdas, i);
    if (!info || !info->has_captures) continue;
    fprintf(codegen->out, "typedef struct %s {\n", info->env_name);
    Vector* captures = info->expr->as.lambda.captures;
    if (captures) {
      for (u64 j = 0; j < vec_len(captures); ++j) {
        Capture* capture = vec_at_ptr(captures, j);
        if (!capture) continue;
        Symbol* symbol = sema_capture_symbol(codegen->sema, capture);
        if (!symbol || !symbol->type) {
          cg_error(codegen, TAKA_MSG_C_CAPTURE_S_HAS_NO_RESOLVED_TYPE,
                   str8_fmt(capture->name));
          continue;
        }
        fputs("  ", codegen->out);
        if (!emit_type(codegen, symbol->type)) continue;
        fputc(' ', codegen->out);
        emit_field_name(codegen->out, capture->name);
        fputs(";\n", codegen->out);
      }
    }
    fprintf(codegen->out, "} %s;\n\n", info->env_name);
  }
}

static void emit_lambda_functions(Codegen* codegen) {
  for (u64 i = 0; i < vec_len(codegen->lambdas); ++i) {
    LambdaInfo* info = vec_at_ptr(codegen->lambdas, i);
    if (!info) continue;
    if (!emit_lambda_signature(codegen, info, FALSE)) {
      fputs(" {\n}\n", codegen->out);
      continue;
    }
    Type* type = sema_expr_type(codegen->sema, info->expr);
    if (!type) continue;
    fputs(" {\n  (void)__taka_env;\n", codegen->out);
    if (info->expr->as.lambda.params) {
      for (u64 j = 0; j < vec_len(info->expr->as.lambda.params); ++j) {
        Param* param = vec_at_ptr(info->expr->as.lambda.params, j);
        if (!param) continue;
        fputs("  (void)", codegen->out);
        emit_local_name(codegen->out, param->name);
        fputs(";\n", codegen->out);
      }
    }
    const char* saved_env = codegen->current_lambda_env_type;
    codegen->current_lambda_env_type = info->has_captures ? info->env_name : NULL;
    emit_defer_scope(codegen, false);
    if (info->expr->as.lambda.body_is_block) {
      Stmt* body = info->expr->as.lambda.block;
      if (body && body->as.block.body) {
        for (u64 j = 0; j < vec_len(body->as.block.body); ++j) {
          emit_stmt(codegen, vec_at_ptr(body->as.block.body, j),
                    type->as.fn.return_type);
        }
      }
      if (body && body->as.block.tail) {
        if (type_is_void(type->as.fn.return_type)) {
          emit_expr_statement(codegen, body->as.block.tail);
        } else {
          fputs("return ", codegen->out);
          emit_expr(codegen, body->as.block.tail);
          fputs(";\n", codegen->out);
        }
      }
    } else {
      if (type_is_void(type->as.fn.return_type)) {
        emit_expr_statement(codegen, info->expr->as.lambda.expr);
      } else {
        fputs("return ", codegen->out);
        emit_expr(codegen, info->expr->as.lambda.expr);
        fputs(";\n", codegen->out);
      }
    }
    DeferScope* scope = vec_at_ptr(codegen->defer_scopes,
                                   vec_len(codegen->defer_scopes) - 1);
    emit_scope_cleanup(codegen, scope);
    vec_pop(codegen->defer_scopes);
    codegen->current_lambda_env_type = saved_env;
    fputs("}\n\n", codegen->out);
  }
}

static bool emit_function_definitions(Codegen* codegen) {
  Vector* body = sema_program(codegen->sema)->as.block.body;
  for (u64 i = 0; i < vec_len(body); ++i) {
    Stmt* stmt = vec_at_ptr(body, i);
    if (!stmt || stmt->kind != STMT_ITEM || !stmt->as.item.item) {
      continue;
    }
    Item* item = stmt->as.item.item;
    if (item->kind == ITEM_FN) {
      FnDecl* fn = &item->as.fn;
      bool hidden = sig_needs_arena(&fn->sig);
      bool is_static = item->visibility != VIS_PUB;
      char name[512];
      snprintf(name, sizeof(name), "taka_%.*s", str8_fmt(fn->sig.name));
      if (is_static) {
        fputs("static TAKA_UNUSED ", codegen->out);
      }
      if (!emit_fn_signature(codegen, &fn->sig, name, FALSE, hidden, FALSE)) {
        return FALSE;
      }
      emit_function_body_from_sig(codegen, fn);
      fputs("\n", codegen->out);
    } else if (item->kind == ITEM_IMPL && item->as.impl_decl.methods) {
      ImplDecl* impl = &item->as.impl_decl;
      String8 type_name = path_first(impl->for_type && impl->for_type->kind == TYPE_PATH
                                         ? impl->for_type->as.path.path
                                         : (Path){0});
      for (u64 j = 0; j < vec_len(impl->methods); ++j) {
        FnDecl* method = vec_at_ptr(impl->methods, j);
        if (!method) continue;
        char name[512];
        snprintf(name, sizeof(name), "taka_%.*s_%.*s", str8_fmt(type_name),
                 str8_fmt(method->sig.name));
        fputs("static TAKA_UNUSED ", codegen->out);
        if (!emit_fn_signature(codegen, &method->sig, name, FALSE,
                               sig_needs_arena(&method->sig), FALSE)) {
          return FALSE;
        }
        emit_function_body_from_sig(codegen, method);
        fputs("\n", codegen->out);
      }
    }
  }
  return TRUE;
}

static void emit_function_body_from_sig(Codegen* codegen, FnDecl* fn) {
  fputs(" {\n", codegen->out);
  if (sig_needs_arena(&fn->sig)) {
    fputs("  (void)__taka_arena;\n", codegen->out);
  }
  if (fn->sig.params) {
    for (u64 i = 0; i < vec_len(fn->sig.params); ++i) {
      Param* param = vec_at_ptr(fn->sig.params, i);
      if (!param) {
        continue;
      }
      fputs("  (void)", codegen->out);
      emit_local_name(codegen->out, param->name);
      fputs(";\n", codegen->out);
    }
  }
  emit_defer_scope(codegen, false);
  if (fn->body && fn->body->as.block.body) {
    for (u64 i = 0; i < vec_len(fn->body->as.block.body); ++i) {
      emit_stmt(codegen, vec_at_ptr(fn->body->as.block.body, i), fn->sig.return_type);
    }
  }
  if (fn->body && fn->body->as.block.tail) {
    fputs("return ", codegen->out);
    emit_expr(codegen, fn->body->as.block.tail);
    fputs(";\n", codegen->out);
  }
  DeferScope* scope = vec_at_ptr(codegen->defer_scopes, vec_len(codegen->defer_scopes) - 1);
  emit_scope_cleanup(codegen, scope);
  vec_pop(codegen->defer_scopes);
  fputs("}\n", codegen->out);
}

static bool emit_main_wrapper(Codegen* codegen) {
  Symbol* main_symbol = sema_lookup_global(codegen->sema, str8_lit("main"));
  if (!main_symbol || main_symbol->kind != SYM_FN) {
    return TRUE;
  }
  Item* item = main_symbol->item;
  FnSig* sig = &item->as.fn.sig;
  if (sig->params && vec_len(sig->params) > 0) {
    cg_error(codegen, TAKA_MSG_C_MAIN_WITH_PARAMETERS_IS_NOT_SUPPORTED_BY_THE_C_BACKEND_YET);
    return FALSE;
  }
  bool hidden = sig_needs_arena(sig);
  bool returns_value = sig->return_type && !(sig->return_type->kind == TYPE_PRIMITIVE &&
                                             str8_eq(sig->return_type->as.primitive.name,
                                                     str8_lit("never")));
  bool returns_void = !sig->return_type ||
                      (sig->return_type->kind == TYPE_PRIMITIVE &&
                       (str8_eq(sig->return_type->as.primitive.name, str8_lit("never")) ||
                        str8_eq(sig->return_type->as.primitive.name, str8_lit("void"))));
  fputs("int main(void) {\n", codegen->out);
  if (hidden) {
    fputs("  taka_arena* __taka_arena = taka_arena_new(1u << 20);\n", codegen->out);
    fputs("  if (!__taka_arena) return 1;\n", codegen->out);
  }
  fputs("  ", codegen->out);
  if (returns_value && !returns_void) {
    fputs("int result = ", codegen->out);
  }
  fputs("taka_main(", codegen->out);
  if (hidden) {
    fputs("__taka_arena", codegen->out);
  }
  fputs(");\n", codegen->out);
  if (hidden) {
    fputs("  taka_arena_free(__taka_arena);\n", codegen->out);
  }
  if (returns_value && !returns_void) {
    fputs("  return result;\n", codegen->out);
  } else {
    fputs("  return 0;\n", codegen->out);
  }
  fputs("}\n", codegen->out);
  return TRUE;
}

Codegen* codegen_create(MemArena* arena, Sema* sema) {
  Codegen* codegen = PUSH_STRUCT_ZERO(arena, Codegen);
  codegen->arena = arena;
  codegen->sema = sema;
  codegen->defer_scopes = vec_new(arena, sizeof(DeferScope*), 0);
  codegen->loop_stack = vec_new(arena, sizeof(LoopContext*), 0);
  codegen->temp_counter = 0;
  codegen->closure_sigs = vec_new(arena, sizeof(ClosureSig*), 0);
  codegen->lambdas = vec_new(arena, sizeof(LambdaInfo*), 0);
  codegen->lambda_map = map_new(arena);
  codegen->lambda_counter = 0;
  codegen->current_lambda_env_type = NULL;
  return codegen;
}

bool codegen_generate(Codegen* codegen, FILE* out, const char* source_name) {
  if (!codegen || !out || !codegen->sema) {
    return FALSE;
  }
  codegen->out = out;
  fprintf(out, "/* generated from %s */\n\n", source_name ? source_name : "<input>");
  collect_program(codegen);
  emit_preamble(codegen);
  emit_forward_decls(codegen);
  if (!emit_enums(codegen)) {
    return FALSE;
  }
  if (!emit_structs(codegen)) {
    return FALSE;
  }
  emit_closure_typedefs(codegen);
  emit_lambda_prototypes(codegen);
  if (!emit_prototypes_v2(codegen)) {
    return FALSE;
  }
  emit_trampolines(codegen);
  emit_globals(codegen);
  emit_lambda_env_structs(codegen);
  emit_lambda_functions(codegen);
  if (!emit_function_definitions(codegen)) {
    return FALSE;
  }
  if (!emit_main_wrapper(codegen)) {
    return FALSE;
  }
  return codegen->errors == 0;
}

void codegen_free(Codegen* codegen) {
  if (codegen) {
    map_free(codegen->lambda_map);
  }
}

u32 codegen_error_count(Codegen* codegen) {
  return codegen ? codegen->errors : 0;
}
