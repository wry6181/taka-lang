#include "parser.h"
#include "lookup.h"
#include "diagnostics/diagnostics.h"

#include <stdlib.h>
#include <string.h>

typedef struct TypeScan {
  Parser* p;
  u64 i;
} TypeScan;

static void parser_enter(Parser* p);
static void parser_leave(Parser* p);
#if defined(__GNUC__)
__attribute__((noreturn))
#endif
static void parser_diag_fatal(DiagDomain domain, const char* fmt, u64 line, u64 column, ...)
    T_PRINTF(2, 5);

#define PARSER_FATAL(fmt, line, column, ...) \
  parser_diag_fatal(DIAG_DOMAIN_PARSER, fmt, line, column, ##__VA_ARGS__)

static Expr* parse_expr_impl(Parser* p);
static Expr* parse_primary(Parser* p);
static Expr* parse_postfix(Parser* p);
static Expr* parse_unary(Parser* p);
static Expr* parse_unary_impl(Parser* p);
static Expr* parse_ternary(Parser* p);
static Expr* parse_ternary_impl(Parser* p);
static Stmt* parse_statement_impl(Parser* p, bool allow_tail, Expr** tail);
static Pattern* parse_pattern_impl(Parser* p);
static Type* parse_type(Parser* p);
static Stmt* parse_block(Parser* p);
static Stmt* parse_statement(Parser* p, bool allow_tail, Expr** tail);
static Pattern* parse_pattern(Parser* p);
static Vector* parse_params(Parser* p);
static Vector* parse_handler_ops(Parser* p);

static Expr* expr_new(Parser* p, ExprKind kind) {
  Expr* e = PUSH_STRUCT_ZERO(p->arena, Expr);
  e->kind = kind;
  return e;
}

static Stmt* stmt_new(Parser* p, StmtKind kind) {
  Stmt* s = PUSH_STRUCT_ZERO(p->arena, Stmt);
  s->kind = kind;
  return s;
}

static Type* type_new(Parser* p, TypeKind kind) {
  Type* t = PUSH_STRUCT_ZERO(p->arena, Type);
  t->kind = kind;
  return t;
}

static Item* item_new(Parser* p, ItemKind kind) {
  Item* i = PUSH_STRUCT_ZERO(p->arena, Item);
  i->kind = kind;
  return i;
}

static Vector* ptr_vec(MemArena* arena) {
  return vec_new(arena, sizeof(void*), 0);
}

static Vector* str_vec(MemArena* arena) {
  return vec_new(arena, sizeof(String8), 0);
}

static Token parser_peek(const Parser* p, u64 offset) {
  if (p->pos + offset < p->count) {
    return p->tokens[p->pos + offset];
  }
  return p->tokens[p->count - 1];
}

static TokenKind parser_peek_kind(const Parser* p, u64 offset) {
  return parser_peek(p, offset).kind;
}

static bool parser_check(const Parser* p, TokenKind kind) {
  return parser_current_kind(p) == kind;
}

static bool parser_match(Parser* p, TokenKind kind) {
  if (parser_check(p, kind)) {
    parser_advance(p);
    return TRUE;
  }
  return FALSE;
}


static bool parser_is_gt(TokenKind kind) {
  return kind == TOKEN_GREATER || kind == TOKEN_GREATER_GREATER ||
         kind == TOKEN_GREATER_GREATER_EQUALS;
}

static bool parser_check_gt(const Parser* p) {
  return parser_is_gt(parser_current_kind(p));
}

static Token parser_take_gt(Parser* p) {
  Token t = parser_current(p);
  if (t.kind == TOKEN_GREATER) {
    return parser_advance(p);
  }
  if (t.kind == TOKEN_GREATER_GREATER) {
    p->pos++;
    p->has_pending = true;
    p->pending_token = token_new(TOKEN_GREATER, t.value, (unsigned long long)t.line, (unsigned long long)t.column);
    return token_new(TOKEN_GREATER, t.value, (unsigned long long)t.line, (unsigned long long)t.column);
  }
  if (t.kind == TOKEN_GREATER_GREATER_EQUALS) {
    p->pos++;
    p->has_pending = true;
    p->pending_token = token_new(TOKEN_GREATER_EQUALS, t.value, (unsigned long long)t.line, (unsigned long long)t.column);
    return token_new(TOKEN_GREATER, t.value, (unsigned long long)t.line, (unsigned long long)t.column);
  }
  PARSER_FATAL(TAKA_MSG_P_EXPECTED_GREATER, t.line, t.column);
}

static Token parser_split_amp(Parser* p) {
  Token t = parser_current(p);
  p->pos++;
  p->has_pending = true;
  p->pending_token = token_new(TOKEN_AMP, t.value, (unsigned long long)t.line, (unsigned long long)t.column);
  return token_new(TOKEN_AMP, t.value, (unsigned long long)t.line, (unsigned long long)t.column);
}

Token parser_current(const Parser* p) {
  if (!p) {
    PARSER_FATAL(TAKA_MSG_P_UNEXPECTED_END_OF_TOKEN_STREAM, 0, 0);
  }
  if (p->has_pending) {
    return p->pending_token;
  }
  if (p->pos >= p->count) {
    PARSER_FATAL(TAKA_MSG_P_UNEXPECTED_END_OF_TOKEN_STREAM, 0, 0);
  }
  return p->tokens[p->pos];
}

TokenKind parser_current_kind(const Parser* p) {
  return parser_current(p).kind;
}

Token parser_advance(Parser* p) {
  if (p->has_pending) {
    p->has_pending = false;
    return p->pending_token;
  }
  Token tk = parser_current(p);
  p->pos++;
  return tk;
}

bool parser_has_tokens(const Parser* p) {
  if (!p) {
    return FALSE;
  }
  if (p->has_pending) {
    return p->pending_token.kind != TOKEN_EOF;
  }
  return p->pos < p->count && p->tokens[p->pos].kind != TOKEN_EOF;
}

Token parser_expect_error(Parser* p, TokenKind expected, const char* err) {
  Token token = parser_current(p);
  if (token.kind != expected) {
    if (err) {
      PARSER_FATAL(TAKA_MSG_P_CUSTOM_ERROR, token.line, token.column, err);
    }
    String8 want = tokenkind_to_string8(expected);
    String8 got = tokenkind_to_string8(token.kind);
    PARSER_FATAL(TAKA_MSG_P_EXPECTED_TOKEN, token.line, token.column, str8_fmt(want),
                 str8_fmt(got));
  }
  return parser_advance(p);
}

Token parser_expect(Parser* p, TokenKind expected) {
  return parser_expect_error(p, expected, NULL);
}

static String8 parser_ident(Parser* p) {
  return parser_expect_error(p, TOKEN_IDENTIFIER, "expected an identifier").value;
}

static bool is_primitive_kind(TokenKind kind) {
  switch (kind) {
    case TOKEN_I8:
    case TOKEN_I16:
    case TOKEN_I32:
    case TOKEN_I64:
    case TOKEN_U8:
    case TOKEN_U16:
    case TOKEN_U32:
    case TOKEN_U64:
    case TOKEN_F32:
    case TOKEN_F64:
    case TOKEN_CHAR_TYPE:
    case TOKEN_BOOL:
    case TOKEN_SVIEW:
    case TOKEN_NEVER:
      return TRUE;
    default:
      return FALSE;
  }
}

static bool is_assign_op(TokenKind kind) {
  switch (kind) {
    case TOKEN_ASSIGNMENT:
    case TOKEN_PLUS_EQUALS:
    case TOKEN_MINUS_EQUALS:
    case TOKEN_STAR_EQUALS:
    case TOKEN_SLASH_EQUALS:
    case TOKEN_PERCENT_EQUALS:
    case TOKEN_LESS_LESS_EQUALS:
    case TOKEN_GREATER_GREATER_EQUALS:
    case TOKEN_AMP_EQUALS:
    case TOKEN_PIPE_EQUALS:
    case TOKEN_CARET_EQUALS:
      return TRUE;
    default:
      return FALSE;
  }
}

static int binop_precedence(TokenKind kind) {
  switch (kind) {
    case TOKEN_PIPE_PIPE:
      return 1;
    case TOKEN_AMP_AMP:
      return 2;
    case TOKEN_PIPE:
      return 3;
    case TOKEN_CARET:
      return 4;
    case TOKEN_AMP:
      return 5;
    case TOKEN_EQUALS:
    case TOKEN_NOT_EQUALS:
    case TOKEN_LESS:
    case TOKEN_GREATER:
    case TOKEN_LESS_EQUALS:
    case TOKEN_GREATER_EQUALS:
      return 6;
    case TOKEN_LESS_LESS:
    case TOKEN_GREATER_GREATER:
      return 7;
    case TOKEN_PLUS:
    case TOKEN_MINUS:
      return 8;
    case TOKEN_STAR:
    case TOKEN_SLASH:
    case TOKEN_PERCENT:
      return 9;
    default:
      return 0;
  }
}

static Path parse_path(Parser* p) {
  Path path = {0};
  path.segments = str_vec(p->arena);
  Token first = parser_current(p);
  if (first.kind != TOKEN_IDENTIFIER && first.kind != TOKEN_SELF) {
    PARSER_FATAL(TAKA_MSG_P_EXPECTED_PATH, first.line, first.column);
  }
  parser_advance(p);
  vec_push(path.segments, &first.value);
  while (parser_check(p, TOKEN_COLON_COLON) && parser_peek_kind(p, 1) != TOKEN_LESS) {
    parser_advance(p);
    String8 seg = parser_ident(p);
    vec_push(path.segments, &seg);
  }
  return path;
}

static Vector* parse_generic_args(Parser* p) {
  parser_expect(p, TOKEN_LESS);
  Vector* args = ptr_vec(p->arena);
  if (parser_check_gt(p)) {
    PARSER_FATAL(TAKA_MSG_P_EMPTY_GENERIC_ARGUMENT_LIST, 0, 0);
  }
  while (!parser_check_gt(p)) {
    Type* t = parse_type(p);
    vec_push_ptr(args, t);
    if (!parser_match(p, TOKEN_COMMA)) {
      break;
    }
    if (parser_check_gt(p)) {
      break;
    }
  }
  parser_take_gt(p);
  return args;
}

static Bound* parse_bound(Parser* p) {
  Bound* bound = PUSH_STRUCT_ZERO(p->arena, Bound);
  bound->traits = ptr_vec(p->arena);
  do {
    BoundTrait bt = {0};
    bt.path = parse_path(p);
    if (parser_check(p, TOKEN_LESS)) {
      bt.generic_args = parse_generic_args(p);
    }
    vec_push(bound->traits, &bt);
  } while (parser_match(p, TOKEN_PLUS));
  return bound;
}

static Vector* parse_generics(Parser* p) {
  if (!parser_match(p, TOKEN_LESS)) {
    return NULL;
  }
  Vector* params = ptr_vec(p->arena);
  if (parser_check_gt(p)) {
    PARSER_FATAL(TAKA_MSG_P_EMPTY_GENERIC_PARAMETER_LIST, 0, 0);
  }
  while (!parser_check_gt(p)) {
    GenericParam* gp = PUSH_STRUCT_ZERO(p->arena, GenericParam);
    if (parser_match(p, TOKEN_EFFECT)) {
      gp->is_effect = TRUE;
      gp->name = parser_ident(p);
    } else {
      gp->name = parser_ident(p);
      if (parser_match(p, TOKEN_COLON)) {
        gp->bound = parse_bound(p);
      }
    }
    vec_push_ptr(params, gp);
    if (!parser_match(p, TOKEN_COMMA)) {
      break;
    }
    if (parser_check_gt(p)) {
      break;
    }
  }
  parser_take_gt(p);
  return params;
}

static WhereClause* parse_where(Parser* p) {
  if (!parser_match(p, TOKEN_WHERE)) {
    return NULL;
  }
  WhereClause* where = PUSH_STRUCT_ZERO(p->arena, WhereClause);
  where->constraints = ptr_vec(p->arena);
  do {
    WhereConstraint* wc = PUSH_STRUCT_ZERO(p->arena, WhereConstraint);
    wc->name = parser_ident(p);
    parser_expect(p, TOKEN_COLON);
    wc->bound = parse_bound(p);
    vec_push_ptr(where->constraints, wc);
  } while (parser_match(p, TOKEN_COMMA) && parser_check(p, TOKEN_IDENTIFIER));
  return where;
}

static String8 effect_item_name(EffectItem* item) {
  if (!item->path.segments || vec_len(item->path.segments) == 0) {
    return (String8){0};
  }
  return *(String8*)vec_at(item->path.segments, 0);
}

static void validate_effect_row(EffectRow* row) {
  if (!row) {
    return;
  }
  u64 count = row->items ? vec_len(row->items) : 0;
  bool pure = FALSE;
  bool transacts = FALSE;
  bool suspends = FALSE;
  bool decides = FALSE;
  bool transactional_part = FALSE;
  for (u64 i = 0; i < count; ++i) {
    EffectItem* item = vec_at_ptr(row->items, i);
    String8 name = effect_item_name(item);
    if (str8_eq(name, str8_lit("pure"))) {
      pure = TRUE;
    } else if (str8_eq(name, str8_lit("transacts"))) {
      transacts = TRUE;
    } else if (str8_eq(name, str8_lit("suspends"))) {
      suspends = TRUE;
    } else if (str8_eq(name, str8_lit("decides"))) {
      decides = TRUE;
    } else if (str8_eq(name, str8_lit("reads")) || str8_eq(name, str8_lit("writes")) ||
               str8_eq(name, str8_lit("alloc"))) {
      transactional_part = TRUE;
    }
  }
  if (pure && (count > 1 || row->has_tail)) {
    PARSER_FATAL(TAKA_MSG_P_PURE_CANNOT_COMPOSE_WITH_OTHER_EFFECTS, 0, 0);
  }
  if (transacts && (transactional_part || pure)) {
    PARSER_FATAL(TAKA_MSG_P_TRANSACTS_CANNOT_MIX_WITH_READS_WRITES_ALLOC_PURE, 0, 0);
  }
  if (suspends && decides) {
    PARSER_FATAL(TAKA_MSG_P_SUSPENDS_AND_DECIDES_CANNOT_APPEAR_TOGETHER, 0, 0);
  }
}

static EffectRow* parse_effect_row(Parser* p) {
  if (!parser_match(p, TOKEN_LESS)) {
    return NULL;
  }
  EffectRow* row = PUSH_STRUCT_ZERO(p->arena, EffectRow);
  row->items = ptr_vec(p->arena);
  if (parser_check_gt(p)) {
    PARSER_FATAL(TAKA_MSG_P_EMPTY_EFFECT_ROW, 0, 0);
  }
  while (!parser_check_gt(p)) {
    if (parser_match(p, TOKEN_PIPE)) {
      row->has_tail = TRUE;
      row->tail = parser_ident(p);
      break;
    }
    EffectItem* item = PUSH_STRUCT_ZERO(p->arena, EffectItem);
    item->path = parse_path(p);
    if (parser_check(p, TOKEN_LESS)) {
      item->generic_args = parse_generic_args(p);
    }
    if (item->path.segments && vec_len(item->path.segments) == 1) {
      String8 name = *(String8*)vec_at(item->path.segments, 0);
      static const char* builtins[] = {
          "io", "alloc", "reads", "writes", "decides", "suspends",
          "transacts", "pure", "ownership",
      };
      for (u64 i = 0; i < sizeof(builtins) / sizeof(builtins[0]); ++i) {
        if (str8_eq(name, str8(builtins[i]))) {
          item->is_builtin = TRUE;
          break;
        }
      }
    }
    vec_push_ptr(row->items, item);
    if (parser_check(p, TOKEN_PIPE)) {
      continue;
    }
    if (!parser_match(p, TOKEN_COMMA)) {
      break;
    }
    if (parser_check_gt(p)) {
      break;
    }
  }
  parser_take_gt(p);
  validate_effect_row(row);
  return row;
}

static Type* parse_auto_type(Parser* p) {
  bool is_const = parser_match(p, TOKEN_CONST);
  parser_expect(p, TOKEN_AUTO);
  if (parser_check(p, TOKEN_STAR)) {
    Token t = parser_current(p);
    PARSER_FATAL(TAKA_MSG_P_AUTO_CANNOT_BE_A_POINTER_TYPE, t.line, t.column);
  }
  Type* type = type_new(p, TYPE_AUTO);
  type->is_const = is_const;
  type->as.auto_.is_auto = TRUE;
  if (parser_match(p, TOKEN_AMP)) {
    type->is_reference = TRUE;
  }
  return type;
}

static bool starts_auto_type(Parser* p) {
  return parser_check(p, TOKEN_AUTO) ||
         (parser_check(p, TOKEN_CONST) && parser_peek_kind(p, 1) == TOKEN_AUTO);
}

static Vector* parse_params_ex(Parser* p, bool allow_auto) {
  parser_expect(p, TOKEN_OPEN_PAREN);
  Vector* params = ptr_vec(p->arena);
  if (!parser_check(p, TOKEN_CLOSE_PAREN)) {
    while (TRUE) {
      Param* param = PUSH_STRUCT_ZERO(p->arena, Param);
      param->is_mut = parser_match(p, TOKEN_MUT);
      param->type = allow_auto && starts_auto_type(p) ? parse_auto_type(p) : parse_type(p);
      param->name = parser_ident(p);
      parser_declare_symbol(p, param->name, param->is_mut);
      vec_push_ptr(params, param);
      if (!parser_match(p, TOKEN_COMMA)) {
        break;
      }
      if (parser_check(p, TOKEN_CLOSE_PAREN)) {
        break;
      }
    }
  }
  parser_expect(p, TOKEN_CLOSE_PAREN);
  return params;
}

static Vector* parse_params(Parser* p) {
  return parse_params_ex(p, FALSE);
}

static Type* parse_type_body(Parser* p) {
  Token t = parser_current(p);
  if (is_primitive_kind(t.kind)) {
    parser_advance(p);
    Type* type = type_new(p, TYPE_PRIMITIVE);
    type->as.primitive.name = t.value;
    return type;
  }
  if (t.kind == TOKEN_IDENTIFIER || t.kind == TOKEN_SELF) {
    Type* type = type_new(p, TYPE_PATH);
    type->as.path.path = parse_path(p);
    if (parser_check(p, TOKEN_LESS)) {
      type->as.path.generic_args = parse_generic_args(p);
    }
    return type;
  }
  if (t.kind == TOKEN_EXTERN) {
    parser_advance(p);
    parser_expect(p, TOKEN_FN);
    Type* type = type_new(p, TYPE_FN);
    type->as.fn.is_extern = TRUE;
    parser_expect(p, TOKEN_OPEN_PAREN);
    type->as.fn.params = ptr_vec(p->arena);
    if (!parser_check(p, TOKEN_CLOSE_PAREN)) {
      while (TRUE) {
        Type* param = parse_type(p);
        vec_push_ptr(type->as.fn.params, param);
        if (!parser_match(p, TOKEN_COMMA)) {
          break;
        }
        if (parser_check(p, TOKEN_CLOSE_PAREN)) {
          break;
        }
      }
    }
    parser_expect(p, TOKEN_CLOSE_PAREN);
    type->as.fn.effects = parse_effect_row(p);
    if (parser_match(p, TOKEN_ARROW)) {
      type->as.fn.return_type = parse_type(p);
    }
    return type;
  }
  if (t.kind == TOKEN_FN) {
    parser_advance(p);
    Type* type = type_new(p, TYPE_FN);
    parser_expect(p, TOKEN_OPEN_PAREN);
    type->as.fn.params = ptr_vec(p->arena);
    if (!parser_check(p, TOKEN_CLOSE_PAREN)) {
      while (TRUE) {
        Type* param = parse_type(p);
        vec_push_ptr(type->as.fn.params, param);
        if (!parser_match(p, TOKEN_COMMA)) {
          break;
        }
        if (parser_check(p, TOKEN_CLOSE_PAREN)) {
          break;
        }
      }
    }
    parser_expect(p, TOKEN_CLOSE_PAREN);
    type->as.fn.effects = parse_effect_row(p);
    if (parser_match(p, TOKEN_ARROW)) {
      type->as.fn.return_type = parse_type(p);
    }
    return type;
  }
  if (t.kind == TOKEN_DYN) {
    parser_advance(p);
    Type* type = type_new(p, TYPE_DYN);
    type->as.dyn.path = parse_path(p);
    if (parser_check(p, TOKEN_LESS)) {
      type->as.dyn.generic_args = parse_generic_args(p);
    }
    return type;
  }
  if (t.kind == TOKEN_OPEN_PAREN) {
    parser_advance(p);
    Type* type = type_new(p, TYPE_TUPLE);
    type->as.tuple.types = ptr_vec(p->arena);
    if (parser_check(p, TOKEN_CLOSE_PAREN)) {
      PARSER_FATAL(TAKA_MSG_P_AN_EMPTY_TUPLE_TYPE_IS_NOT_ALLOWED, 0, 0);
    }
    if (!parser_check(p, TOKEN_CLOSE_PAREN)) {
      while (TRUE) {
        Type* elem = parse_type(p);
        vec_push_ptr(type->as.tuple.types, elem);
        if (!parser_match(p, TOKEN_COMMA)) {
          break;
        }
        if (parser_check(p, TOKEN_CLOSE_PAREN)) {
          break;
        }
      }
    }
    parser_expect(p, TOKEN_CLOSE_PAREN);
    return type;
  }
  if (t.kind == TOKEN_OPEN_BRACKET) {
    parser_advance(p);
    Type* type = type_new(p, TYPE_ARRAY);
    type->as.array.element = parse_type(p);
    parser_expect(p, TOKEN_SEMICOLON);
    type->as.array.length = parse_expr_impl(p);
    parser_expect(p, TOKEN_CLOSE_BRACKET);
    return type;
  }
  PARSER_FATAL(TAKA_MSG_P_EXPECTED_TYPE, t.line, t.column);
}

static Type* parse_type(Parser* p) {
  parser_enter(p);
  bool is_const = parser_match(p, TOKEN_CONST);
  Type* type = parse_type_body(p);
  type->is_const = is_const;
  while (parser_match(p, TOKEN_STAR)) {
    type->pointer_depth++;
  }
  if (parser_check(p, TOKEN_AMP)) {
    parser_advance(p);
    type->is_reference = TRUE;
  }
  if (parser_check(p, TOKEN_AMP_AMP)) {
    Token t = parser_current(p);
    PARSER_FATAL(TAKA_MSG_P_INVALID_REFERENCE_TYPE, t.line, t.column);
  }
  parser_leave(p);
  return type;
}

static TokenKind scan_kind(TypeScan* s, u64 off) {
  u64 i = s->i + off;
  if (i < s->p->count) {
    return s->p->tokens[i].kind;
  }
  return TOKEN_EOF;
}

static void scan_advance(TypeScan* s) {
  s->i++;
}

static bool scan_match(TypeScan* s, TokenKind kind) {
  if (scan_kind(s, 0) == kind) {
    scan_advance(s);
    return TRUE;
  }
  return FALSE;
}

static bool scan_skip_angles(TypeScan* s) {
  if (!scan_match(s, TOKEN_LESS)) {
    return FALSE;
  }
  i32 depth = 1;
  while (depth > 0 && scan_kind(s, 0) != TOKEN_EOF) {
    TokenKind kind = scan_kind(s, 0);
    if (kind == TOKEN_LESS) {
      depth++;
      scan_advance(s);
    } else if (kind == TOKEN_GREATER) {
      depth--;
      scan_advance(s);
    } else if (kind == TOKEN_GREATER_GREATER || kind == TOKEN_GREATER_GREATER_EQUALS) {
      depth -= 2;
      scan_advance(s);
    } else {
      scan_advance(s);
    }
  }
  return depth <= 0;
}

static bool scan_skip_to_close_bracket(TypeScan* s) {
  i32 depth = 0;
  while (scan_kind(s, 0) != TOKEN_EOF) {
    TokenKind kind = scan_kind(s, 0);
    if (kind == TOKEN_OPEN_BRACKET) {
      depth++;
    } else if (kind == TOKEN_CLOSE_BRACKET) {
      if (depth == 0) {
        scan_advance(s);
        return TRUE;
      }
      depth--;
    }
    scan_advance(s);
  }
  return FALSE;
}

static bool scan_type(TypeScan* s);

static bool scan_path(TypeScan* s) {
  TokenKind kind = scan_kind(s, 0);
  if (kind != TOKEN_IDENTIFIER && kind != TOKEN_SELF) {
    return FALSE;
  }
  scan_advance(s);
  while (scan_kind(s, 0) == TOKEN_COLON_COLON && scan_kind(s, 1) == TOKEN_IDENTIFIER) {
    scan_advance(s);
    scan_advance(s);
  }
  return TRUE;
}

static bool scan_type_body(TypeScan* s) {
  TokenKind kind = scan_kind(s, 0);
  if (is_primitive_kind(kind)) {
    scan_advance(s);
    return TRUE;
  }
  if (kind == TOKEN_AUTO) {
    scan_advance(s);
    scan_match(s, TOKEN_AMP);
    return TRUE;
  }
  if (kind == TOKEN_EXTERN) {
    scan_advance(s);
    if (!scan_match(s, TOKEN_FN)) {
      return FALSE;
    }
    kind = TOKEN_FN;
  }
  if (kind == TOKEN_FN) {
    if (!scan_match(s, TOKEN_FN)) {
      return FALSE;
    }
    if (!scan_match(s, TOKEN_OPEN_PAREN)) {
      return FALSE;
    }
    if (scan_kind(s, 0) != TOKEN_CLOSE_PAREN) {
      if (!scan_type(s)) {
        return FALSE;
      }
      while (scan_match(s, TOKEN_COMMA)) {
        if (scan_kind(s, 0) == TOKEN_CLOSE_PAREN) {
          break;
        }
        if (!scan_type(s)) {
          return FALSE;
        }
      }
    }
    if (!scan_match(s, TOKEN_CLOSE_PAREN)) {
      return FALSE;
    }
    if (scan_kind(s, 0) == TOKEN_LESS) {
      if (!scan_skip_angles(s)) {
        return FALSE;
      }
    }
    if (scan_match(s, TOKEN_ARROW)) {
      if (!scan_type(s)) {
        return FALSE;
      }
    }
    return TRUE;
  }
  if (kind == TOKEN_DYN) {
    scan_advance(s);
    if (!scan_path(s)) {
      return FALSE;
    }
    if (scan_kind(s, 0) == TOKEN_LESS) {
      return scan_skip_angles(s);
    }
    return TRUE;
  }
  if (kind == TOKEN_OPEN_PAREN) {
    scan_advance(s);
    if (scan_kind(s, 0) != TOKEN_CLOSE_PAREN) {
      if (!scan_type(s)) {
        return FALSE;
      }
      while (scan_match(s, TOKEN_COMMA)) {
        if (scan_kind(s, 0) == TOKEN_CLOSE_PAREN) {
          break;
        }
        if (!scan_type(s)) {
          return FALSE;
        }
      }
    }
    return scan_match(s, TOKEN_CLOSE_PAREN);
  }
  if (kind == TOKEN_OPEN_BRACKET) {
    scan_advance(s);
    if (!scan_type(s)) {
      return FALSE;
    }
    if (!scan_match(s, TOKEN_SEMICOLON)) {
      return FALSE;
    }
    if (!scan_skip_to_close_bracket(s)) {
      return FALSE;
    }
    return TRUE;
  }
  if (kind == TOKEN_IDENTIFIER || kind == TOKEN_SELF) {
    if (!scan_path(s)) {
      return FALSE;
    }
    if (scan_kind(s, 0) == TOKEN_LESS) {
      return scan_skip_angles(s);
    }
    return TRUE;
  }
  return FALSE;
}

static bool scan_type(TypeScan* s) {
  scan_match(s, TOKEN_CONST);
  if (!scan_type_body(s)) {
    return FALSE;
  }
  while (scan_match(s, TOKEN_STAR)) {
  }
  scan_match(s, TOKEN_AMP);
  return TRUE;
}

static bool looks_like_declaration(Parser* p) {
  TypeScan scan = {.p = p, .i = p->pos};
  scan_match(&scan, TOKEN_MUT);
  if (!scan_type(&scan)) {
    return FALSE;
  }
  if (scan_kind(&scan, 0) != TOKEN_IDENTIFIER) {
    return FALSE;
  }
  scan_advance(&scan);
  return scan_kind(&scan, 0) == TOKEN_ASSIGNMENT;
}

static Stmt* make_var_stmt(Parser* p, bool is_mut, Type* type, String8 name, Expr* value) {
  Stmt* stmt = stmt_new(p, STMT_VAR);
  stmt->as.var_expr.var_name = name;
  stmt->as.var_expr.is_const = !is_mut;
  stmt->as.var_expr.assigned_value = *value;
  stmt->as.var_expr.type = type;
  stmt->as.var_expr.is_auto = type->kind == TYPE_AUTO;
  stmt->as.var_expr.is_mut = is_mut;
  parser_declare_symbol(p, name, is_mut);
  return stmt;
}

static Stmt* parse_variable_declaration(Parser* p, bool consume_semicolon) {
  bool is_mut = parser_match(p, TOKEN_MUT);
  Type* type = starts_auto_type(p) ? parse_auto_type(p) : parse_type(p);
  String8 name = parser_ident(p);
  parser_expect(p, TOKEN_ASSIGNMENT);
  Expr* value = parse_expr_impl(p);
  Stmt* first = make_var_stmt(p, is_mut, type, name, value);
  if (consume_semicolon) {
    if (parser_check(p, TOKEN_COMMA)) {
      Stmt* block = stmt_new(p, STMT_BLOCK);
      block->as.block.body = ptr_vec(p->arena);
      vec_push_ptr(block->as.block.body, first);
      while (parser_match(p, TOKEN_COMMA)) {
        String8 next_name = parser_ident(p);
        parser_expect(p, TOKEN_ASSIGNMENT);
        Expr* next_value = parse_expr_impl(p);
        Stmt* next = make_var_stmt(p, is_mut, type, next_name, next_value);
        vec_push_ptr(block->as.block.body, next);
      }
      parser_expect(p, TOKEN_SEMICOLON);
      return block;
    }
    parser_expect(p, TOKEN_SEMICOLON);
  }
  return first;
}

static Stmt* parse_legacy_var(Parser* p) {
  bool is_const = FALSE;
  bool is_mut = FALSE;
  if (parser_match(p, TOKEN_CONST)) {
    is_const = TRUE;
  } else if (parser_match(p, TOKEN_MUT)) {
    is_mut = TRUE;
  }
  String8 name = parser_ident(p);
  parser_expect(p, TOKEN_ASSIGNMENT);
  Expr* value = parse_expr_impl(p);
  parser_expect(p, TOKEN_SEMICOLON);
  Stmt* stmt = stmt_new(p, STMT_VAR);
  stmt->as.var_expr.var_name = name;
  stmt->as.var_expr.is_const = is_const;
  stmt->as.var_expr.assigned_value = *value;
  stmt->as.var_expr.is_auto = TRUE;
  stmt->as.var_expr.is_mut = is_mut;
  parser_declare_symbol(p, name, !is_const);
  return stmt;
}

static Expr* parse_assignment_tail(Parser* p, Expr* left, bool consume_semicolon) {
  if (is_assign_op(parser_current_kind(p))) {
    Token op = parser_advance(p);
    Expr* rhs = parse_expr_impl(p);
    Expr* e = expr_new(p, EXPR_ASSIGNMENT);
    e->as.assignment.assigne_expr = left;
    e->as.assignment.oper = op;
    e->as.assignment.rhs = rhs;
    left = e;
  }
  if (consume_semicolon) {
    parser_expect(p, TOKEN_SEMICOLON);
  }
  return left;
}

static Stmt* expression_stmt(Parser* p, Expr* e) {
  Stmt* stmt = stmt_new(p, STMT_EXPRESION);
  stmt->as.expresion.expr = *e;
  stmt->as.expresion.is_tail = FALSE;
  return stmt;
}

static Condition parse_condition(Parser* p) {
  Condition cond = {0};
  if (parser_check(p, TOKEN_AUTO) || parser_check(p, TOKEN_MUT)) {
    cond.is_binding = TRUE;
    cond.is_auto = parser_match(p, TOKEN_AUTO);
    cond.is_mut = parser_match(p, TOKEN_MUT);
    cond.pattern = parse_pattern(p);
    parser_expect(p, TOKEN_ASSIGNMENT);
    cond.expr = parse_expr_impl(p);
  } else {
    cond.expr = parse_expr_impl(p);
  }
  return cond;
}

static Stmt* parse_if_stmt(Parser* p) {
  parser_expect(p, TOKEN_IF);
  parser_expect(p, TOKEN_OPEN_PAREN);
  Condition cond = parse_condition(p);
  parser_expect(p, TOKEN_CLOSE_PAREN);
  Stmt* body = parse_block(p);

  Stmt* stmt = stmt_new(p, STMT_IF);
  stmt->as.if_stmt.branches = ptr_vec(p->arena);
  IfBranch* first = PUSH_STRUCT_ZERO(p->arena, IfBranch);
  first->condition = cond;
  first->body = body;
  vec_push_ptr(stmt->as.if_stmt.branches, first);

  while (parser_match(p, TOKEN_ELSE)) {
    if (parser_match(p, TOKEN_IF)) {
      parser_expect(p, TOKEN_OPEN_PAREN);
      Condition next_cond = parse_condition(p);
      parser_expect(p, TOKEN_CLOSE_PAREN);
      IfBranch* branch = PUSH_STRUCT_ZERO(p->arena, IfBranch);
      branch->condition = next_cond;
      branch->body = parse_block(p);
      vec_push_ptr(stmt->as.if_stmt.branches, branch);
    } else {
      stmt->as.if_stmt.else_block = parse_block(p);
      break;
    }
  }
  return stmt;
}

static Stmt* parse_while_stmt(Parser* p) {
  parser_expect(p, TOKEN_WHILE);
  parser_expect(p, TOKEN_OPEN_PAREN);
  Condition cond = parse_condition(p);
  parser_expect(p, TOKEN_CLOSE_PAREN);
  Stmt* stmt = stmt_new(p, STMT_WHILE);
  stmt->as.while_stmt.condition = cond;
  stmt->as.while_stmt.body = parse_block(p);
  return stmt;
}

static bool for_is_each(Parser* p) {
  u64 i = p->pos;
  i32 depth = 0;
  while (i < p->count) {
    TokenKind kind = p->tokens[i].kind;
    if (kind == TOKEN_EOF || (kind == TOKEN_CLOSE_PAREN && depth == 0)) {
      return FALSE;
    }
    if (kind == TOKEN_IN && depth == 0) {
      return TRUE;
    }
    if (kind == TOKEN_SEMICOLON && depth == 0) {
      return FALSE;
    }
    if (kind == TOKEN_OPEN_PAREN || kind == TOKEN_OPEN_BRACKET || kind == TOKEN_OPEN_CURLY) {
      depth++;
    } else if (kind == TOKEN_CLOSE_PAREN || kind == TOKEN_CLOSE_BRACKET ||
               kind == TOKEN_CLOSE_CURLY) {
      depth--;
    }
    i++;
  }
  return FALSE;
}

static Stmt* parse_for_stmt(Parser* p) {
  parser_expect(p, TOKEN_FOR);
  parser_expect(p, TOKEN_OPEN_PAREN);
  Stmt* stmt = stmt_new(p, STMT_FOR);
  if (for_is_each(p)) {
    stmt->as.for_stmt.for_stmt.is_each = TRUE;
    if (parser_check(p, TOKEN_AUTO)) {
      parser_advance(p);
    }
    stmt->as.for_stmt.for_stmt.pattern = parse_pattern(p);
    parser_expect(p, TOKEN_IN);
    stmt->as.for_stmt.for_stmt.iterable = parse_expr_impl(p);
  } else {
    if (!parser_check(p, TOKEN_SEMICOLON)) {
      if (looks_like_declaration(p)) {
        stmt->as.for_stmt.for_stmt.init = parse_variable_declaration(p, FALSE);
      } else {
        Expr* init = parse_expr_impl(p);
        init = parse_assignment_tail(p, init, FALSE);
        stmt->as.for_stmt.for_stmt.init = expression_stmt(p, init);
      }
    }
    parser_expect(p, TOKEN_SEMICOLON);
    if (!parser_check(p, TOKEN_SEMICOLON)) {
      stmt->as.for_stmt.for_stmt.cond = parse_expr_impl(p);
    }
    parser_expect(p, TOKEN_SEMICOLON);
    if (!parser_check(p, TOKEN_CLOSE_PAREN)) {
      Expr* step = parse_expr_impl(p);
      step = parse_assignment_tail(p, step, FALSE);
      stmt->as.for_stmt.for_stmt.step = expression_stmt(p, step);
    }
  }
  parser_expect(p, TOKEN_CLOSE_PAREN);
  stmt->as.for_stmt.body = parse_block(p);
  return stmt;
}

static Stmt* parse_defer_stmt(Parser* p) {
  parser_expect(p, TOKEN_DEFER);
  Stmt* stmt = stmt_new(p, STMT_DEFER);
  if (parser_check(p, TOKEN_OPEN_CURLY)) {
    stmt->as.defer_stmt.is_block = TRUE;
    stmt->as.defer_stmt.block = parse_block(p);
  } else {
    Expr* e = parse_expr_impl(p);
    e = parse_assignment_tail(p, e, FALSE);
    parser_expect(p, TOKEN_SEMICOLON);
    stmt->as.defer_stmt.stmt = expression_stmt(p, e);
  }
  return stmt;
}

static Stmt* parse_break_or_continue(Parser* p, bool is_break) {
  parser_advance(p);
  bool has_label = FALSE;
  String8 label = {0};
  if (parser_check(p, TOKEN_IDENTIFIER)) {
    label = parser_advance(p).value;
    has_label = TRUE;
  }
  parser_expect(p, TOKEN_SEMICOLON);
  Stmt* stmt = stmt_new(p, is_break ? STMT_BREAK : STMT_CONTINUE);
  if (is_break) {
    stmt->as.break_stmt.label = label;
    stmt->as.break_stmt.has_label = has_label;
  } else {
    stmt->as.continue_stmt.label = label;
    stmt->as.continue_stmt.has_label = has_label;
  }
  return stmt;
}

static Stmt* parse_return_stmt(Parser* p) {
  parser_expect(p, TOKEN_RETURN);
  Stmt* stmt = stmt_new(p, STMT_RETURN);
  if (!parser_check(p, TOKEN_SEMICOLON)) {
    stmt->as.return_stmt.has_value = TRUE;
    stmt->as.return_stmt.value = parse_expr_impl(p);
  }
  parser_expect(p, TOKEN_SEMICOLON);
  return stmt;
}

static Stmt* parse_label_stmt(Parser* p, bool allow_tail, Expr** tail) {
  String8 label = parser_advance(p).value;
  parser_expect(p, TOKEN_COLON);
  Stmt* stmt = stmt_new(p, STMT_LABEL);
  stmt->as.label_stmt.label = label;
  stmt->as.label_stmt.stmt = parse_statement(p, allow_tail, tail);
  return stmt;
}

static Stmt* parse_statement_impl(Parser* p, bool allow_tail, Expr** tail) {
  TokenKind kind = parser_current_kind(p);
  switch (kind) {
    case TOKEN_OPEN_CURLY:
      return parse_block(p);
    case TOKEN_IF:
      return parse_if_stmt(p);
    case TOKEN_WHILE:
      return parse_while_stmt(p);
    case TOKEN_FOR:
      return parse_for_stmt(p);
    case TOKEN_DEFER:
      return parse_defer_stmt(p);
    case TOKEN_BREAK:
      return parse_break_or_continue(p, TRUE);
    case TOKEN_CONTINUE:
      return parse_break_or_continue(p, FALSE);
    case TOKEN_RETURN:
      return parse_return_stmt(p);
    case TOKEN_IDENTIFIER:
      if (parser_peek_kind(p, 1) == TOKEN_COLON) {
        return parse_label_stmt(p, allow_tail, tail);
      }
      break;
    default:
      break;
  }

  if (looks_like_declaration(p)) {
    return parse_variable_declaration(p, TRUE);
  }

  if ((kind == TOKEN_CONST || kind == TOKEN_MUT) &&
      parser_peek_kind(p, 1) == TOKEN_IDENTIFIER &&
      parser_peek_kind(p, 2) == TOKEN_ASSIGNMENT) {
    return parse_legacy_var(p);
  }
  if (kind == TOKEN_IDENTIFIER && parser_peek_kind(p, 1) == TOKEN_ASSIGNMENT &&
      !parser_find_symbol(p, parser_current(p).value)) {
    return parse_legacy_var(p);
  }

  Expr* e = parse_expr_impl(p);
  e = parse_assignment_tail(p, e, FALSE);
  if (parser_match(p, TOKEN_SEMICOLON)) {
    return expression_stmt(p, e);
  }
  if (allow_tail && parser_check(p, TOKEN_CLOSE_CURLY)) {
    if (tail) {
      *tail = e;
    }
    return NULL;
  }
  Token t = parser_current(p);
  PARSER_FATAL(TAKA_MSG_P_EXPECTED_SEMICOLON, t.line, t.column);
}

static Stmt* parse_statement(Parser* p, bool allow_tail, Expr** tail) {
  parser_enter(p);
  Stmt* stmt = parse_statement_impl(p, allow_tail, tail);
  parser_leave(p);
  return stmt;
}

static Expr* parse_number_literal(Parser* p, Token t) {
  String8 raw = t.value;
  bool hex_or_bin = raw.size >= 2 && raw.data[0] == '0' &&
                    (raw.data[1] == 'x' || raw.data[1] == 'X' ||
                     raw.data[1] == 'b' || raw.data[1] == 'B');
  bool is_float = FALSE;
  if (!hex_or_bin) {
    for (u64 i = 0; i < raw.size; ++i) {
      u8 c = raw.data[i];
      if (c == '.' || c == 'e' || c == 'E') {
        is_float = TRUE;
        break;
      }
      if (c == 'f' && i + 2 < raw.size) {
        String8 suffix = {raw.data + i, 3};
        if (str8_eq(suffix, str8_lit("f32")) || str8_eq(suffix, str8_lit("f64"))) {
          is_float = TRUE;
          break;
        }
      }
    }
  }

  if (is_float) {
    u8* buf = PUSH_ARRAY(p->arena, u8, raw.size + 1);
    u64 n = 0;
    for (u64 i = 0; i < raw.size; ++i) {
      u8 c = raw.data[i];
      if (c == '_') {
        continue;
      }
      if (c == 'f' && i + 2 < raw.size) {
        String8 suffix = {raw.data + i, 3};
        if (str8_eq(suffix, str8_lit("f32")) || str8_eq(suffix, str8_lit("f64"))) {
          break;
        }
      }
      buf[n++] = c;
    }
    buf[n] = '\0';
    Expr* e = expr_new(p, EXPR_FLOAT);
    e->as.float_lit.value = strtod((char*)buf, NULL);
    e->as.float_lit.raw = raw;
    return e;
  }

  u64 base = 10;
  u64 i = 0;
  if (raw.size >= 2 && raw.data[0] == '0' && (raw.data[1] == 'x' || raw.data[1] == 'X')) {
    base = 16;
    i = 2;
  } else if (raw.size >= 2 && raw.data[0] == '0' &&
             (raw.data[1] == 'b' || raw.data[1] == 'B')) {
    base = 2;
    i = 2;
  }
  u64 value = 0;
  for (; i < raw.size; ++i) {
    u8 c = raw.data[i];
    if (c == '_') {
      continue;
    }
    u64 digit = 0;
    if (c >= '0' && c <= '9') {
      digit = (u64)(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      digit = (u64)(c - 'a') + 10;
    } else if (c >= 'A' && c <= 'F') {
      digit = (u64)(c - 'A') + 10;
    } else {
      break;
    }
    if (digit >= base) {
      break;
    }
    value = value * base + digit;
  }
  Expr* e = expr_new(p, EXPR_NUMBER);
  e->as.number.value = (i64)value;
  e->as.number.raw = raw;
  return e;
}

static Expr* parse_literal(Parser* p) {
  Token t = parser_current(p);
  switch (t.kind) {
    case TOKEN_NUMBER:
      parser_advance(p);
      return parse_number_literal(p, t);
    case TOKEN_STRING: {
      parser_advance(p);
      Expr* e = expr_new(p, EXPR_STRING);
      e->as.string.value = t.value;
      return e;
    }
    case TOKEN_CHAR: {
      parser_advance(p);
      Expr* e = expr_new(p, EXPR_CHAR);
      e->as.char_lit.value = t.value;
      return e;
    }
    case TOKEN_TRUE:
    case TOKEN_FALSE: {
      parser_advance(p);
      Expr* e = expr_new(p, EXPR_BOOL);
      e->as.boolean.value = t.kind == TOKEN_TRUE;
      return e;
    }
    case TOKEN_NULL: {
      parser_advance(p);
      Expr* e = expr_new(p, EXPR_NULL);
      e->as.null_lit.present = TRUE;
      return e;
    }
    case TOKEN_UNDEF: {
      parser_advance(p);
      Expr* e = expr_new(p, EXPR_UNDEF);
      e->as.undef_lit.present = TRUE;
      return e;
    }
    default:
      PARSER_FATAL(TAKA_MSG_P_EXPECTED_LITERAL, t.line, t.column);
  }
}


static bool is_lambda_start(Parser* p) {
  if (!parser_check(p, TOKEN_OPEN_BRACKET)) {
    return FALSE;
  }
  u64 i = p->pos;
  i32 depth = 0;
  while (i < p->count) {
    TokenKind kind = p->tokens[i].kind;
    if (kind == TOKEN_EOF) {
      return FALSE;
    }
    if (kind == TOKEN_OPEN_BRACKET || kind == TOKEN_OPEN_PAREN || kind == TOKEN_OPEN_CURLY) {
      depth++;
    } else if (kind == TOKEN_CLOSE_BRACKET || kind == TOKEN_CLOSE_PAREN ||
               kind == TOKEN_CLOSE_CURLY) {
      depth--;
      if (depth == 0 && kind == TOKEN_CLOSE_BRACKET) {
        i++;
        return i < p->count && p->tokens[i].kind == TOKEN_OPEN_PAREN;
      }
    }
    i++;
  }
  return FALSE;
}

static Expr* parse_lambda(Parser* p) {
  parser_expect(p, TOKEN_OPEN_BRACKET);
  Vector* captures = ptr_vec(p->arena);
  if (!parser_check(p, TOKEN_CLOSE_BRACKET)) {
    while (TRUE) {
      Capture* capture = PUSH_STRUCT_ZERO(p->arena, Capture);
      if (parser_match(p, TOKEN_AMP)) {
        capture->by_ref = TRUE;
        capture->name = parser_ident(p);
      } else {
        capture->is_mut = parser_match(p, TOKEN_MUT);
        capture->name = parser_ident(p);
        if (parser_match(p, TOKEN_ASSIGNMENT)) {
          capture->has_init = TRUE;
          capture->init = parse_expr_impl(p);
        }
      }
      vec_push_ptr(captures, capture);
      if (!parser_match(p, TOKEN_COMMA)) {
        break;
      }
      if (parser_check(p, TOKEN_CLOSE_BRACKET)) {
        break;
      }
    }
  }
  parser_expect(p, TOKEN_CLOSE_BRACKET);
  Vector* params = parse_params_ex(p, TRUE);
  EffectRow* effects = parse_effect_row(p);
  Type* return_type = NULL;
  if (parser_match(p, TOKEN_ARROW)) {
    return_type = parse_type(p);
  }
  Expr* e = expr_new(p, EXPR_LAMBDA);
  e->as.lambda.captures = captures;
  e->as.lambda.params = params;
  e->as.lambda.effects = effects;
  e->as.lambda.return_type = return_type;
  if (parser_check(p, TOKEN_OPEN_CURLY)) {
    e->as.lambda.body_is_block = TRUE;
    e->as.lambda.block = parse_block(p);
  } else {
    parser_expect(p, TOKEN_FAT_ARROW);
    e->as.lambda.expr = parse_expr_impl(p);
  }
  return e;
}

static Expr* parse_array_literal(Parser* p) {
  TokenKind open = parser_current_kind(p);
  TokenKind close = open == TOKEN_OPEN_BRACKET ? TOKEN_CLOSE_BRACKET : TOKEN_CLOSE_CURLY;
  parser_advance(p);
  Expr* e = expr_new(p, EXPR_ARRAY_LIT);
  e->as.array_lit.items = ptr_vec(p->arena);
  if (parser_match(p, close)) {
    return e;
  }
  Expr* first = parse_expr_impl(p);
  if (parser_match(p, TOKEN_SEMICOLON)) {
    e->as.array_lit.is_fill = TRUE;
    e->as.array_lit.fill = first;
    e->as.array_lit.count = parse_expr_impl(p);
    parser_expect(p, close);
    return e;
  }
  vec_push_ptr(e->as.array_lit.items, first);
  while (parser_match(p, TOKEN_COMMA)) {
    if (parser_check(p, close)) {
      break;
    }
    Expr* item = parse_expr_impl(p);
    vec_push_ptr(e->as.array_lit.items, item);
  }
  parser_expect(p, close);
  return e;
}

static Expr* parse_struct_literal(Parser* p, Path path) {
  parser_expect(p, TOKEN_OPEN_CURLY);
  Expr* e = expr_new(p, EXPR_STRUCT_LIT);
  e->as.struct_lit.path = path;
  e->as.struct_lit.fields = ptr_vec(p->arena);
  if (!parser_check(p, TOKEN_CLOSE_CURLY)) {
    while (TRUE) {
      FieldInit* field = PUSH_STRUCT_ZERO(p->arena, FieldInit);
      field->name = parser_ident(p);
      parser_expect(p, TOKEN_COLON);
      field->value = parse_expr_impl(p);
      vec_push_ptr(e->as.struct_lit.fields, field);
      if (!parser_match(p, TOKEN_COMMA)) {
        break;
      }
      if (parser_check(p, TOKEN_CLOSE_CURLY)) {
        break;
      }
    }
  }
  parser_expect(p, TOKEN_CLOSE_CURLY);
  return e;
}

static Expr* parse_if_expr(Parser* p) {
  parser_expect(p, TOKEN_IF);
  parser_expect(p, TOKEN_OPEN_PAREN);
  Condition cond = parse_condition(p);
  parser_expect(p, TOKEN_CLOSE_PAREN);
  Stmt* body = parse_block(p);
  Expr* e = expr_new(p, EXPR_IF);
  e->as.if_expr.branches = ptr_vec(p->arena);
  IfBranch* first = PUSH_STRUCT_ZERO(p->arena, IfBranch);
  first->condition = cond;
  first->body = body;
  vec_push_ptr(e->as.if_expr.branches, first);
  while (parser_match(p, TOKEN_ELSE)) {
    if (parser_match(p, TOKEN_IF)) {
      parser_expect(p, TOKEN_OPEN_PAREN);
      Condition next_cond = parse_condition(p);
      parser_expect(p, TOKEN_CLOSE_PAREN);
      IfBranch* branch = PUSH_STRUCT_ZERO(p->arena, IfBranch);
      branch->condition = next_cond;
      branch->body = parse_block(p);
      vec_push_ptr(e->as.if_expr.branches, branch);
    } else {
      e->as.if_expr.else_block = parse_block(p);
      return e;
    }
  }
  Token t = parser_current(p);
  PARSER_FATAL(TAKA_MSG_P_IF_EXPRESSION_REQUIRES_AN_ELSE_BRANCH, t.line, t.column);
}

static Expr* parse_match_expr(Parser* p) {
  parser_expect(p, TOKEN_MATCH);
  Expr* subject = NULL;
  if (parser_match(p, TOKEN_OPEN_PAREN)) {
    subject = parse_expr_impl(p);
    parser_expect(p, TOKEN_CLOSE_PAREN);
  } else {
    bool saved = p->suppress_struct_lit;
    p->suppress_struct_lit = TRUE;
    subject = parse_expr_impl(p);
    p->suppress_struct_lit = saved;
  }
  parser_expect(p, TOKEN_OPEN_CURLY);
  Expr* e = expr_new(p, EXPR_MATCH);
  e->as.match.subject = subject;
  e->as.match.arms = ptr_vec(p->arena);
  while (!parser_check(p, TOKEN_CLOSE_CURLY)) {
    MatchArm* arm = PUSH_STRUCT_ZERO(p->arena, MatchArm);
    arm->pattern = parse_pattern(p);
    if (parser_match(p, TOKEN_IF)) {
      arm->guard = parse_expr_impl(p);
    }
    parser_expect(p, TOKEN_FAT_ARROW);
    if (parser_check(p, TOKEN_OPEN_CURLY)) {
      arm->body_is_block = TRUE;
      arm->block = parse_block(p);
    } else {
      arm->expr = parse_expr_impl(p);
    }
    vec_push_ptr(e->as.match.arms, arm);
    if (!parser_match(p, TOKEN_COMMA)) {
      break;
    }
  }
  parser_expect(p, TOKEN_CLOSE_CURLY);
  return e;
}

static Expr* parse_perform_expr(Parser* p) {
  parser_expect(p, TOKEN_PERFORM);
  Expr* e = expr_new(p, EXPR_PERFORM);
  e->as.perform.path = parse_path(p);
  parser_expect(p, TOKEN_OPEN_PAREN);
  e->as.perform.args = ptr_vec(p->arena);
  if (!parser_check(p, TOKEN_CLOSE_PAREN)) {
    while (TRUE) {
      Expr* arg = parse_expr_impl(p);
      vec_push_ptr(e->as.perform.args, arg);
      if (!parser_match(p, TOKEN_COMMA)) {
        break;
      }
      if (parser_check(p, TOKEN_CLOSE_PAREN)) {
        break;
      }
    }
  }
  parser_expect(p, TOKEN_CLOSE_PAREN);
  return e;
}

static Expr* parse_handle_expr(Parser* p) {
  parser_expect(p, TOKEN_HANDLE);
  Expr* e = expr_new(p, EXPR_HANDLE);
  if (parser_check(p, TOKEN_OPEN_CURLY)) {
    e->as.handle.subject_is_block = TRUE;
    e->as.handle.block = parse_block(p);
  } else {
    e->as.handle.subject = parse_expr_impl(p);
  }
  parser_expect(p, TOKEN_WITH);
  if (parser_check(p, TOKEN_OPEN_CURLY)) {
    e->as.handle.handler_ops = parse_handler_ops(p);
  } else {
    e->as.handle.handler_is_path = TRUE;
    e->as.handle.handler_path = parse_path(p);
  }
  return e;
}

static Expr* parse_spawn_expr(Parser* p) {
  parser_expect(p, TOKEN_SPAWN);
  Expr* e = expr_new(p, EXPR_SPAWN);
  if (parser_check(p, TOKEN_OPEN_CURLY)) {
    e->as.spawn.is_block = TRUE;
    e->as.spawn.block = parse_block(p);
  } else {
    Expr* call = parse_postfix(p);
    if (call->kind != EXPR_CALL) {
      Token t = parser_current(p);
      PARSER_FATAL(TAKA_MSG_P_SPAWN_REQUIRES_A_CALL_EXPRESSION, t.line, t.column);
    }
    e->as.spawn.expr = call;
  }
  return e;
}

static Expr* parse_resume_expr(Parser* p) {
  if (!p->in_handler_op) {
    Token t = parser_current(p);
    PARSER_FATAL(TAKA_MSG_P_RESUME_IS_ONLY_VALID_INSIDE_A_HANDLER_OPERATION, t.line,
                 t.column);
  }
  parser_expect(p, TOKEN_RESUME);
  parser_expect(p, TOKEN_OPEN_PAREN);
  Expr* e = expr_new(p, EXPR_RESUME);
  if (!parser_check(p, TOKEN_CLOSE_PAREN)) {
    e->as.resume.has_value = TRUE;
    e->as.resume.value = parse_expr_impl(p);
  }
  parser_expect(p, TOKEN_CLOSE_PAREN);
  return e;
}

static Expr* parse_transact_expr(Parser* p) {
  parser_expect(p, TOKEN_TRANSACT);
  Expr* e = expr_new(p, EXPR_TRANSACT);
  e->as.transact.block = parse_block(p);
  return e;
}

static Expr* parse_sizeof_expr(Parser* p) {
  parser_expect(p, TOKEN_SIZEOF);
  Expr* e = expr_new(p, EXPR_SIZEOF);
  if (parser_match(p, TOKEN_OPEN_PAREN)) {
    TypeScan scan = {.p = p, .i = p->pos};
    if (scan_type(&scan) && scan_kind(&scan, 0) == TOKEN_CLOSE_PAREN) {
      e->as.sizeof_expr.is_type = TRUE;
      e->as.sizeof_expr.type = parse_type(p);
    } else {
      e->as.sizeof_expr.expr = parse_expr_impl(p);
    }
    parser_expect(p, TOKEN_CLOSE_PAREN);
    return e;
  }
  e->as.sizeof_expr.expr = parse_unary(p);
  return e;
}

static Expr* parse_offsetof_expr(Parser* p) {
  parser_expect(p, TOKEN_OFFSETOF);
  Expr* e = expr_new(p, EXPR_OFFSETOF);
  if (!parser_check(p, TOKEN_OPEN_PAREN)) {
    Expr* target = parse_unary(p);
    if (target->kind == EXPR_SYMBOL) {
      e->as.offsetof_expr.field = target->as.symbol.value;
    }
    return e;
  }
  parser_advance(p);
  e->as.offsetof_expr.type = parse_type(p);
  parser_expect(p, TOKEN_COMMA);
  e->as.offsetof_expr.field = parser_ident(p);
  parser_expect(p, TOKEN_CLOSE_PAREN);
  return e;
}

static Expr* parse_path_primary(Parser* p) {
  Path path = parse_path(p);
  Vector* args = NULL;
  bool has_turbofish = FALSE;
  if (parser_check(p, TOKEN_COLON_COLON) && parser_peek_kind(p, 1) == TOKEN_LESS) {
    parser_advance(p);
    args = parse_generic_args(p);
    has_turbofish = TRUE;
  }
  if (!p->suppress_struct_lit && parser_check(p, TOKEN_OPEN_CURLY)) {
    return parse_struct_literal(p, path);
  }
  if (vec_len(path.segments) == 1 && !has_turbofish) {
    Expr* e = expr_new(p, EXPR_SYMBOL);
    e->as.symbol.value = *(String8*)vec_at(path.segments, 0);
    return e;
  }
  Expr* e = expr_new(p, EXPR_PATH);
  e->as.path.path = path;
  e->as.path.generic_args = args;
  e->as.path.has_turbofish = has_turbofish;
  return e;
}

static bool is_block_expr_start(Parser* p) {
  if (!parser_check(p, TOKEN_OPEN_CURLY)) {
    return FALSE;
  }
  u64 i = p->pos;
  i32 depth = 0;
  while (i < p->count) {
    TokenKind kind = p->tokens[i].kind;
    if (kind == TOKEN_EOF) {
      return FALSE;
    }
    if (kind == TOKEN_OPEN_CURLY) {
      depth++;
    } else if (kind == TOKEN_CLOSE_CURLY) {
      depth--;
      if (depth == 0) {
        return FALSE;
      }
    } else if (kind == TOKEN_SEMICOLON && depth == 1) {
      return TRUE;
    }
    i++;
  }
  return FALSE;
}

static Expr* parse_block_expr(Parser* p) {
  Expr* e = expr_new(p, EXPR_BLOCK);
  e->as.block.block = parse_block(p);
  return e;
}

static Expr* parse_primary(Parser* p) {
  TokenKind kind = parser_current_kind(p);
  switch (kind) {
    case TOKEN_NUMBER:
    case TOKEN_STRING:
    case TOKEN_CHAR:
    case TOKEN_TRUE:
    case TOKEN_FALSE:
    case TOKEN_NULL:
    case TOKEN_UNDEF:
      return parse_literal(p);
    case TOKEN_IDENTIFIER:
    case TOKEN_SELF:
      return parse_path_primary(p);
    case TOKEN_OPEN_PAREN: {
      parser_advance(p);
      Expr* e = parse_expr_impl(p);
      parser_expect(p, TOKEN_CLOSE_PAREN);
      return e;
    }
    case TOKEN_OPEN_BRACKET:
      if (is_lambda_start(p)) {
        return parse_lambda(p);
      }
      return parse_array_literal(p);
    case TOKEN_OPEN_CURLY:
      if (is_block_expr_start(p)) {
        return parse_block_expr(p);
      }
      return parse_array_literal(p);
    case TOKEN_SIZEOF:
      return parse_sizeof_expr(p);
    case TOKEN_OFFSETOF:
      return parse_offsetof_expr(p);
    case TOKEN_IF:
      return parse_if_expr(p);
    case TOKEN_MATCH:
      return parse_match_expr(p);
    case TOKEN_PERFORM:
      return parse_perform_expr(p);
    case TOKEN_HANDLE:
      return parse_handle_expr(p);
    case TOKEN_SPAWN:
      return parse_spawn_expr(p);
    case TOKEN_RESUME:
      return parse_resume_expr(p);
    case TOKEN_TRANSACT:
      return parse_transact_expr(p);
    default: {
      Token t = parser_current(p);
      PARSER_FATAL(TAKA_MSG_P_CANNOT_CREATE_PRIMARY_EXPRESSION, t.line, t.column);
    }
  }
}

static Expr* parse_postfix(Parser* p) {
  Expr* left = parse_primary(p);
  while (TRUE) {
    if (parser_match(p, TOKEN_OPEN_PAREN)) {
      Expr* e = expr_new(p, EXPR_CALL);
      e->as.call.callee = left;
      e->as.call.args = ptr_vec(p->arena);
      if (!parser_check(p, TOKEN_CLOSE_PAREN)) {
        while (TRUE) {
          Expr* arg = parse_expr_impl(p);
          vec_push_ptr(e->as.call.args, arg);
          if (!parser_match(p, TOKEN_COMMA)) {
            break;
          }
          if (parser_check(p, TOKEN_CLOSE_PAREN)) {
            break;
          }
        }
      }
      parser_expect(p, TOKEN_CLOSE_PAREN);
      left = e;
    } else if (parser_match(p, TOKEN_OPEN_BRACKET)) {
      Expr* e = expr_new(p, EXPR_INDEX);
      e->as.index.object = left;
      e->as.index.index = parse_expr_impl(p);
      parser_expect(p, TOKEN_CLOSE_BRACKET);
      left = e;
    } else if (parser_match(p, TOKEN_DOT)) {
      Expr* e = expr_new(p, EXPR_FIELD);
      e->as.field.object = left;
      e->as.field.field = parser_ident(p);
      left = e;
    } else if (parser_check(p, TOKEN_PLUS_PLUS) || parser_check(p, TOKEN_MINUS_MINUS)) {
      Token op = parser_advance(p);
      Expr* e = expr_new(p, EXPR_POSTFIX);
      e->as.postfix.expr = left;
      e->as.postfix.oper = op;
      left = e;
    } else {
      break;
    }
  }
  return left;
}

static Expr* parse_unary_impl(Parser* p) {
  TokenKind kind = parser_current_kind(p);
  if (kind == TOKEN_AMP_AMP) {
    Token op = parser_split_amp(p);
    Expr* right = parse_unary(p);
    Expr* e = expr_new(p, EXPR_PREFIX);
    e->as.prefix.right_expr = right;
    e->as.prefix.oper = op;
    return e;
  }
  switch (kind) {
    case TOKEN_PLUS:
    case TOKEN_MINUS:
    case TOKEN_NOT:
    case TOKEN_TILDE:
    case TOKEN_STAR:
    case TOKEN_AMP:
    case TOKEN_PLUS_PLUS:
    case TOKEN_MINUS_MINUS: {
      Token op = parser_advance(p);
      Expr* right = parse_unary(p);
      Expr* e = expr_new(p, EXPR_PREFIX);
      e->as.prefix.right_expr = right;
      e->as.prefix.oper = op;
      return e;
    }
    default:
      return parse_postfix(p);
  }
}

static Expr* parse_unary(Parser* p) {
  parser_enter(p);
  Expr* expr = parse_unary_impl(p);
  parser_leave(p);
  return expr;
}

static Expr* parse_cast(Parser* p) {
  Expr* left = parse_unary(p);
  while (parser_match(p, TOKEN_AS)) {
    Type* type = parse_type(p);
    Expr* e = expr_new(p, EXPR_CAST);
    e->as.cast.expr = left;
    e->as.cast.type = type;
    left = e;
  }
  return left;
}

static Expr* parse_binary(Parser* p, int min_precedence) {
  Expr* left = parse_cast(p);
  while (TRUE) {
    TokenKind kind = parser_current_kind(p);
    int precedence = binop_precedence(kind);
    if (precedence == 0 || precedence < min_precedence) {
      break;
    }
    Token op = parser_advance(p);
    Expr* right = parse_binary(p, precedence + 1);
    Expr* e = expr_new(p, EXPR_BINARY);
    e->as.bin.lhs = left;
    e->as.bin.oper = op;
    e->as.bin.rhs = right;
    left = e;
    if (precedence == 6 && binop_precedence(parser_current_kind(p)) == 6) {
      Token t = parser_current(p);
      PARSER_FATAL(TAKA_MSG_P_COMPARISON_OPERATORS_ARE_NON_ASSOCIATIVE, t.line, t.column);
    }
  }
  return left;
}

static Expr* parse_ternary_impl(Parser* p) {
  Expr* cond = parse_binary(p, 1);
  if (parser_match(p, TOKEN_QUESTION)) {
    Expr* then_expr = parse_expr_impl(p);
    parser_expect(p, TOKEN_COLON);
    Expr* else_expr = parse_ternary(p);
    Expr* e = expr_new(p, EXPR_TERNARY);
    e->as.ternary.cond = cond;
    e->as.ternary.then_expr = then_expr;
    e->as.ternary.else_expr = else_expr;
    return e;
  }
  return cond;
}

static Expr* parse_ternary(Parser* p) {
  parser_enter(p);
  Expr* expr = parse_ternary_impl(p);
  parser_leave(p);
  return expr;
}

static Expr* parse_expr_impl(Parser* p) {
  parser_enter(p);
  Expr* expr = parse_ternary(p);
  parser_leave(p);
  return expr;
}

Expr* parse_expr(Parser* p, BindingPower bp) {
  (void)bp;
  return parse_expr_impl(p);
}

Expr* parse_primary_expr(Parser* p) {
  return parse_primary(p);
}

static Stmt* parse_block(Parser* p) {
  parser_enter(p);
  parser_expect(p, TOKEN_OPEN_CURLY);
  Stmt* block = stmt_new(p, STMT_BLOCK);
  block->as.block.body = ptr_vec(p->arena);
  while (!parser_check(p, TOKEN_CLOSE_CURLY)) {
    if (parser_check(p, TOKEN_EOF)) {
      Token t = parser_current(p);
      PARSER_FATAL(TAKA_MSG_P_UNTERMINATED_BLOCK, t.line, t.column);
    }
    if (parser_match(p, TOKEN_SEMICOLON)) {
      continue;
    }
    if (parser_check(p, TOKEN_MATCH) || parser_check(p, TOKEN_HANDLE) ||
        parser_check(p, TOKEN_TRANSACT)) {
      Expr* e = parse_expr_impl(p);
      bool had_semicolon = parser_match(p, TOKEN_SEMICOLON);
      if (!had_semicolon && parser_check(p, TOKEN_CLOSE_CURLY)) {
        block->as.block.tail = e;
        break;
      }
      Stmt* stmt = expression_stmt(p, e);
      vec_push_ptr(block->as.block.body, stmt);
      continue;
    }
    if (parser_check(p, TOKEN_SPAWN)) {
      Expr* e = parse_expr_impl(p);
      bool had_semicolon = parser_match(p, TOKEN_SEMICOLON);
      if (!had_semicolon && parser_check(p, TOKEN_CLOSE_CURLY)) {
        block->as.block.tail = e;
        break;
      }
      Stmt* stmt = expression_stmt(p, e);
      vec_push_ptr(block->as.block.body, stmt);
      continue;
    }
    Expr* tail = NULL;
    Stmt* stmt = parse_statement(p, TRUE, &tail);
    if (tail) {
      block->as.block.tail = tail;
      break;
    }
    if (stmt) {
      vec_push_ptr(block->as.block.body, stmt);
    }
  }
  parser_expect(p, TOKEN_CLOSE_CURLY);
  parser_leave(p);
  return block;
}

static Pattern* pattern_new(Parser* p, PatternKind kind) {
  Pattern* pat = PUSH_STRUCT_ZERO(p->arena, Pattern);
  pat->kind = kind;
  return pat;
}

static Vector* parse_field_pats(Parser* p) {
  Vector* fields = ptr_vec(p->arena);
  while (!parser_check(p, TOKEN_CLOSE_CURLY)) {
    if (parser_match(p, TOKEN_DOT_DOT)) {
      FieldPat* ellipsis = PUSH_STRUCT_ZERO(p->arena, FieldPat);
      ellipsis->name = str8_lit("..");
      vec_push_ptr(fields, ellipsis);
      parser_match(p, TOKEN_COMMA);
      break;
    }
    FieldPat* field = PUSH_STRUCT_ZERO(p->arena, FieldPat);
    field->name = parser_ident(p);
    if (parser_match(p, TOKEN_COLON)) {
      field->has_pattern = TRUE;
      field->pattern = parse_pattern(p);
    }
    vec_push_ptr(fields, field);
    if (!parser_match(p, TOKEN_COMMA)) {
      break;
    }
  }
  return fields;
}

static Pattern* parse_pattern_primary(Parser* p) {
  TokenKind kind = parser_current_kind(p);
  if (kind == TOKEN_WILDCARD) {
    parser_advance(p);
    return pattern_new(p, PAT_WILDCARD);
  }
  if (kind == TOKEN_AMP) {
    parser_advance(p);
    Pattern* pat = pattern_new(p, PAT_REF_BIND);
    pat->as.bind.name = parser_ident(p);
    pat->as.bind.is_mut = FALSE;
    return pat;
  }
  if (kind == TOKEN_NUMBER || kind == TOKEN_STRING || kind == TOKEN_CHAR ||
      kind == TOKEN_TRUE || kind == TOKEN_FALSE || kind == TOKEN_NULL ||
      kind == TOKEN_UNDEF) {
    Pattern* pat = pattern_new(p, PAT_LITERAL);
    pat->as.literal.literal = parse_literal(p);
    return pat;
  }
  if (kind == TOKEN_MINUS && parser_peek_kind(p, 1) == TOKEN_NUMBER) {
    Token op = parser_advance(p);
    Expr* number = parse_literal(p);
    Expr* prefix = expr_new(p, EXPR_PREFIX);
    prefix->as.prefix.oper = op;
    prefix->as.prefix.right_expr = number;
    Pattern* pat = pattern_new(p, PAT_LITERAL);
    pat->as.literal.literal = prefix;
    return pat;
  }
  if (kind == TOKEN_MUT) {
    parser_advance(p);
    Pattern* pat = pattern_new(p, PAT_BIND);
    pat->as.bind.is_mut = TRUE;
    pat->as.bind.name = parser_ident(p);
    return pat;
  }
  if (kind == TOKEN_IDENTIFIER || kind == TOKEN_SELF) {
    Path path = parse_path(p);
    Pattern* pat = pattern_new(p, PAT_PATH);
    pat->as.path.path = path;
    if (parser_match(p, TOKEN_OPEN_PAREN)) {
      pat->as.path.has_tuple = TRUE;
      pat->as.path.tuple = ptr_vec(p->arena);
      if (!parser_check(p, TOKEN_CLOSE_PAREN)) {
        while (TRUE) {
          Pattern* item = parse_pattern(p);
          vec_push_ptr(pat->as.path.tuple, item);
          if (!parser_match(p, TOKEN_COMMA)) {
            break;
          }
          if (parser_check(p, TOKEN_CLOSE_PAREN)) {
            break;
          }
        }
      }
      parser_expect(p, TOKEN_CLOSE_PAREN);
      return pat;
    }
    if (parser_match(p, TOKEN_OPEN_CURLY)) {
      pat->as.path.has_struct = TRUE;
      pat->as.path.fields = parse_field_pats(p);
      parser_expect(p, TOKEN_CLOSE_CURLY);
      return pat;
    }
    if (vec_len(path.segments) == 1) {
      pat->kind = PAT_BIND;
      pat->as.bind.is_mut = FALSE;
      pat->as.bind.name = *(String8*)vec_at(path.segments, 0);
    }
    return pat;
  }
  Token t = parser_current(p);
  PARSER_FATAL(TAKA_MSG_P_EXPECTED_PATTERN, t.line, t.column);
}

static Pattern* parse_pattern_impl(Parser* p) {
  Pattern* first = parse_pattern_primary(p);
  if (!parser_check(p, TOKEN_PIPE)) {
    return first;
  }
  Pattern* alt = pattern_new(p, PAT_ALT);
  alt->as.alt.alternatives = ptr_vec(p->arena);
  vec_push_ptr(alt->as.alt.alternatives, first);
  while (parser_match(p, TOKEN_PIPE)) {
    Pattern* next = parse_pattern_primary(p);
    vec_push_ptr(alt->as.alt.alternatives, next);
  }
  return alt;
}

static Pattern* parse_pattern(Parser* p) {
  parser_enter(p);
  Pattern* pattern = parse_pattern_impl(p);
  parser_leave(p);
  return pattern;
}

static Vector* parse_handler_ops(Parser* p) {
  parser_expect(p, TOKEN_OPEN_CURLY);
  Vector* ops = ptr_vec(p->arena);
  while (!parser_check(p, TOKEN_CLOSE_CURLY)) {
    HandlerOp* op = PUSH_STRUCT_ZERO(p->arena, HandlerOp);
    if (parser_match(p, TOKEN_RETURN)) {
      op->is_return = TRUE;
      op->name = str8_lit("return");
      parser_expect(p, TOKEN_OPEN_PAREN);
      op->params = ptr_vec(p->arena);
      Param* param = PUSH_STRUCT_ZERO(p->arena, Param);
      param->is_mut = parser_match(p, TOKEN_MUT);
      param->type = parse_type(p);
      param->name = parser_ident(p);
      vec_push_ptr(op->params, param);
      parser_expect(p, TOKEN_CLOSE_PAREN);
    } else {
      op->name = parser_ident(p);
      op->params = parse_params(p);
    }
    parser_expect(p, TOKEN_FAT_ARROW);
    bool saved = p->in_handler_op;
    p->in_handler_op = TRUE;
    if (parser_check(p, TOKEN_OPEN_CURLY)) {
      op->body_is_block = TRUE;
      op->block = parse_block(p);
    } else {
      op->expr = parse_expr_impl(p);
    }
    p->in_handler_op = saved;
    vec_push_ptr(ops, op);
    if (!parser_match(p, TOKEN_COMMA)) {
      break;
    }
  }
  parser_expect(p, TOKEN_CLOSE_CURLY);
  return ops;
}

static FnSig parse_fn_sig(Parser* p) {
  parser_expect(p, TOKEN_FN);
  FnSig sig = {0};
  sig.name = parser_ident(p);
  sig.generics = parse_generics(p);
  sig.params = parse_params(p);
  sig.effects = parse_effect_row(p);
  if (parser_match(p, TOKEN_ARROW)) {
    sig.return_type = parse_type(p);
  }
  sig.where = parse_where(p);
  return sig;
}

static Item* parse_fn_item(Parser* p, Visibility vis) {
  Item* item = item_new(p, ITEM_FN);
  item->visibility = vis;
  item->as.fn.sig = parse_fn_sig(p);
  if (parser_check(p, TOKEN_OPEN_CURLY)) {
    item->as.fn.has_body = TRUE;
    item->as.fn.body = parse_block(p);
  } else {
    parser_expect(p, TOKEN_SEMICOLON);
  }
  return item;
}

static Visibility parse_visibility(Parser* p) {
  if (parser_match(p, TOKEN_PUB)) {
    return VIS_PUB;
  }
  if (parser_match(p, TOKEN_PRIVATE)) {
    return VIS_PRIVATE;
  }
  return VIS_DEFAULT;
}

static Item* parse_import_item(Parser* p, Visibility vis) {
  parser_expect(p, TOKEN_IMPORT);
  Item* item = item_new(p, ITEM_IMPORT);
  item->visibility = vis;
  item->as.import.path = parse_path(p);
  parser_expect(p, TOKEN_SEMICOLON);
  return item;
}

static Item* parse_extern_item(Parser* p, Visibility vis) {
  parser_expect(p, TOKEN_EXTERN);
  Item* item = item_new(p, ITEM_EXTERN);
  item->visibility = vis;
  if (parser_check(p, TOKEN_STRING)) {
    item->as.extern_decl.has_abi = TRUE;
    item->as.extern_decl.abi = parser_advance(p).value;
  }
  item->as.extern_decl.sig = parse_fn_sig(p);
  parser_expect(p, TOKEN_SEMICOLON);
  return item;
}

static Item* parse_struct_item(Parser* p, Visibility vis) {
  parser_expect(p, TOKEN_STRUCT);
  Item* item = item_new(p, ITEM_STRUCT);
  item->visibility = vis;
  item->as.struct_decl.name = parser_ident(p);
  item->as.struct_decl.generics = parse_generics(p);
  item->as.struct_decl.where = parse_where(p);
  item->as.struct_decl.fields = ptr_vec(p->arena);
  parser_expect(p, TOKEN_OPEN_CURLY);
  while (!parser_check(p, TOKEN_CLOSE_CURLY)) {
    StructField* field = PUSH_STRUCT_ZERO(p->arena, StructField);
    field->visibility = parse_visibility(p);
    field->is_mut = parser_match(p, TOKEN_MUT);
    field->type = parse_type(p);
    field->name = parser_ident(p);
    vec_push_ptr(item->as.struct_decl.fields, field);
    if (!parser_match(p, TOKEN_COMMA)) {
      break;
    }
  }
  parser_expect(p, TOKEN_CLOSE_CURLY);
  return item;
}

static Item* parse_enum_item(Parser* p, Visibility vis) {
  parser_expect(p, TOKEN_ENUM);
  Item* item = item_new(p, ITEM_ENUM);
  item->visibility = vis;
  item->as.enum_decl.name = parser_ident(p);
  item->as.enum_decl.generics = parse_generics(p);
  item->as.enum_decl.where = parse_where(p);
  item->as.enum_decl.variants = ptr_vec(p->arena);
  parser_expect(p, TOKEN_OPEN_CURLY);
  while (!parser_check(p, TOKEN_CLOSE_CURLY)) {
    EnumVariant* variant = PUSH_STRUCT_ZERO(p->arena, EnumVariant);
    variant->visibility = parse_visibility(p);
    variant->name = parser_ident(p);
    variant->kind = VARIANT_UNIT;
    if (parser_match(p, TOKEN_OPEN_PAREN)) {
      variant->kind = VARIANT_TUPLE;
      variant->types = ptr_vec(p->arena);
      if (!parser_check(p, TOKEN_CLOSE_PAREN)) {
        while (TRUE) {
          Type* type = parse_type(p);
          vec_push_ptr(variant->types, type);
          if (!parser_match(p, TOKEN_COMMA)) {
            break;
          }
          if (parser_check(p, TOKEN_CLOSE_PAREN)) {
            break;
          }
        }
      }
      parser_expect(p, TOKEN_CLOSE_PAREN);
    } else if (parser_match(p, TOKEN_OPEN_CURLY)) {
      variant->kind = VARIANT_STRUCT;
      variant->fields = ptr_vec(p->arena);
      while (!parser_check(p, TOKEN_CLOSE_CURLY)) {
        VariantField* field = PUSH_STRUCT_ZERO(p->arena, VariantField);
        field->visibility = parse_visibility(p);
        field->type = parse_type(p);
        field->name = parser_ident(p);
        vec_push_ptr(variant->fields, field);
        if (!parser_match(p, TOKEN_COMMA)) {
          break;
        }
      }
      parser_expect(p, TOKEN_CLOSE_CURLY);
    } else if (parser_match(p, TOKEN_ASSIGNMENT)) {
      variant->kind = VARIANT_VALUE;
      variant->value = parse_expr_impl(p);
    }
    vec_push_ptr(item->as.enum_decl.variants, variant);
    if (!parser_match(p, TOKEN_COMMA)) {
      break;
    }
  }
  parser_expect(p, TOKEN_CLOSE_CURLY);
  return item;
}

static Item* parse_effect_item(Parser* p, Visibility vis) {
  parser_expect(p, TOKEN_EFFECT);
  Item* item = item_new(p, ITEM_EFFECT);
  item->visibility = vis;
  item->as.effect_decl.name = parser_ident(p);
  item->as.effect_decl.generics = parse_generics(p);
  item->as.effect_decl.ops = ptr_vec(p->arena);
  parser_expect(p, TOKEN_OPEN_CURLY);
  while (!parser_check(p, TOKEN_CLOSE_CURLY)) {
    EffectOp* op = PUSH_STRUCT_ZERO(p->arena, EffectOp);
    op->visibility = parse_visibility(p);
    parser_expect(p, TOKEN_FN);
    op->name = parser_ident(p);
    op->params = parse_params(p);
    if (parser_match(p, TOKEN_ARROW)) {
      op->return_type = parse_type(p);
    }
    vec_push_ptr(item->as.effect_decl.ops, op);
    if (!parser_match(p, TOKEN_COMMA)) {
      parser_match(p, TOKEN_SEMICOLON);
      if (!parser_check(p, TOKEN_CLOSE_CURLY)) {
        break;
      }
    }
  }
  parser_expect(p, TOKEN_CLOSE_CURLY);
  return item;
}

static Item* parse_handler_item(Parser* p, Visibility vis) {
  parser_expect(p, TOKEN_HANDLER);
  Item* item = item_new(p, ITEM_HANDLER);
  item->visibility = vis;
  item->as.handler_decl.name = parser_ident(p);
  item->as.handler_decl.generics = parse_generics(p);
  parser_expect(p, TOKEN_FOR);
  item->as.handler_decl.effect_path = parse_path(p);
  item->as.handler_decl.effects = parse_effect_row(p);
  item->as.handler_decl.ops = parse_handler_ops(p);
  return item;
}

static Item* parse_trait_item(Parser* p, Visibility vis) {
  parser_expect(p, TOKEN_TRAIT);
  Item* item = item_new(p, ITEM_TRAIT);
  item->visibility = vis;
  item->as.trait_decl.name = parser_ident(p);
  item->as.trait_decl.generics = parse_generics(p);
  if (parser_match(p, TOKEN_COLON)) {
    item->as.trait_decl.super = parse_bound(p);
  }
  item->as.trait_decl.methods = ptr_vec(p->arena);
  parser_expect(p, TOKEN_OPEN_CURLY);
  while (!parser_check(p, TOKEN_CLOSE_CURLY)) {
    FnDecl* method = PUSH_STRUCT_ZERO(p->arena, FnDecl);
    method->sig = parse_fn_sig(p);
    parser_expect(p, TOKEN_SEMICOLON);
    vec_push_ptr(item->as.trait_decl.methods, method);
  }
  parser_expect(p, TOKEN_CLOSE_CURLY);
  return item;
}

static Item* parse_impl_item(Parser* p, Visibility vis) {
  parser_expect(p, TOKEN_IMPL);
  Item* item = item_new(p, ITEM_IMPL);
  item->visibility = vis;
  item->as.impl_decl.generics = parse_generics(p);
  item->as.impl_decl.trait_path = parse_path(p);
  if (parser_check(p, TOKEN_LESS)) {
    item->as.impl_decl.trait_args = parse_generic_args(p);
  }
  parser_expect(p, TOKEN_FOR);
  item->as.impl_decl.for_type = parse_type(p);
  item->as.impl_decl.where = parse_where(p);
  item->as.impl_decl.methods = ptr_vec(p->arena);
  parser_expect(p, TOKEN_OPEN_CURLY);
  while (!parser_check(p, TOKEN_CLOSE_CURLY)) {
    Visibility method_vis = parse_visibility(p);
    (void)method_vis;
    FnDecl* method = PUSH_STRUCT_ZERO(p->arena, FnDecl);
    method->sig = parse_fn_sig(p);
    method->has_body = TRUE;
    method->body = parse_block(p);
    vec_push_ptr(item->as.impl_decl.methods, method);
  }
  parser_expect(p, TOKEN_CLOSE_CURLY);
  return item;
}

static Item* parse_type_alias_item(Parser* p, Visibility vis) {
  parser_expect(p, TOKEN_TYPE);
  Item* item = item_new(p, ITEM_TYPE_ALIAS);
  item->visibility = vis;
  item->as.type_alias.name = parser_ident(p);
  item->as.type_alias.generics = parse_generics(p);
  parser_expect(p, TOKEN_ASSIGNMENT);
  item->as.type_alias.type = parse_type(p);
  parser_expect(p, TOKEN_SEMICOLON);
  return item;
}

static Item* parse_global_item(Parser* p, Visibility vis) {
  Item* item = item_new(p, ITEM_GLOBAL);
  item->visibility = vis;
  if ((parser_check(p, TOKEN_CONST) || parser_check(p, TOKEN_MUT)) &&
      parser_peek_kind(p, 1) == TOKEN_IDENTIFIER &&
      parser_peek_kind(p, 2) == TOKEN_ASSIGNMENT) {
    item->as.global.declaration = parse_legacy_var(p);
    return item;
  }
  if (parser_check(p, TOKEN_IDENTIFIER) && parser_peek_kind(p, 1) == TOKEN_ASSIGNMENT) {
    item->as.global.declaration = parse_legacy_var(p);
    return item;
  }
  if (looks_like_declaration(p)) {
    item->as.global.declaration = parse_variable_declaration(p, TRUE);
    return item;
  }
  item->as.global.declaration = parse_statement(p, FALSE, NULL);
  return item;
}

static Item* parse_item(Parser* p) {
  Visibility vis = parse_visibility(p);
  switch (parser_current_kind(p)) {
    case TOKEN_IMPORT:
      return parse_import_item(p, vis);
    case TOKEN_FN:
      return parse_fn_item(p, vis);
    case TOKEN_EXTERN:
      return parse_extern_item(p, vis);
    case TOKEN_STRUCT:
      return parse_struct_item(p, vis);
    case TOKEN_ENUM:
      return parse_enum_item(p, vis);
    case TOKEN_EFFECT:
      return parse_effect_item(p, vis);
    case TOKEN_HANDLER:
      return parse_handler_item(p, vis);
    case TOKEN_TRAIT:
      return parse_trait_item(p, vis);
    case TOKEN_IMPL:
      return parse_impl_item(p, vis);
    case TOKEN_TYPE:
      return parse_type_alias_item(p, vis);
    case TOKEN_MOD: {
      Token t = parser_current(p);
      PARSER_FATAL(TAKA_MSG_P_MOD_BODIES_ARE_NOT_DEFINED, t.line, t.column);
    }
    default:
      return parse_global_item(p, vis);
  }
}

Stmt* parse_stmt(Parser* p) {
  return parse_statement(p, FALSE, NULL);
}

Stmt* parse_var_stmt(Parser* p) {
  if (looks_like_declaration(p)) {
    return parse_variable_declaration(p, TRUE);
  }
  return parse_legacy_var(p);
}

Parser create_parser(MemArena* arena, Token* tokens, u64 count) {
  Lookups* lookups = PUSH_STRUCT(arena, Lookups);
  lookup_init(lookups, arena);
  return (Parser){
      .arena = arena,
      .tokens = tokens,
      .count = count,
      .pos = 0,
      .lookups = lookups,
      .symbols = vec_new(arena, sizeof(ParserSymbol), 0),
      .pending_token = {.value = {.data = NULL, .size = 0},
                        .kind = TOKEN_EOF,
                        .line = 0,
                        .column = 0},
      .has_pending = FALSE,
      .suppress_struct_lit = FALSE,
      .in_handler_op = FALSE,
      .depth = 0,
  };
}

void parser_free(Parser* p) {
  if (p) {
    lookup_free(p->lookups);
  }
}

static jmp_buf* parser_error_jmp = NULL;

void parser_set_error_jmp(jmp_buf* target) {
  parser_error_jmp = target;
}

void parser_die(void) {
  if (parser_error_jmp) {
    longjmp(*parser_error_jmp, 1);
  }
  exit(1);
}

static void parser_diag_fatal(DiagDomain domain, const char* fmt, u64 line, u64 column, ...) {
  va_list args;
  va_start(args, column);
  diag_vemit_format(domain, fmt, line, column, args);
  va_end(args);
  parser_die();
}

static void parser_enter(Parser* p) {
  if (++p->depth > 512) {
    PARSER_FATAL(TAKA_MSG_P_NESTING_DEPTH_LIMIT_EXCEEDED, 0, 0);
  }
}

static void parser_leave(Parser* p) {
  if (p->depth > 0) {
    p->depth--;
  }
}

Stmt* parse_program(MemArena* arena, Token* tokens, u64 count) {
  Parser parser = create_parser(arena, tokens, count);
  Vector* body = vec_new(arena, sizeof(Stmt*), 0);

  while (parser_has_tokens(&parser)) {
    Item* item = parse_item(&parser);
    if (item->kind == ITEM_GLOBAL) {
      vec_push_ptr(body, item->as.global.declaration);
    } else {
      Stmt* stmt = stmt_new(&parser, STMT_ITEM);
      stmt->as.item.item = item;
      vec_push_ptr(body, stmt);
    }
  }

  parser_free(&parser);

  Stmt* block = PUSH_STRUCT_ZERO(arena, Stmt);
  block->kind = STMT_BLOCK;
  block->as.block.body = body;
  block->as.block.tail = NULL;
  return block;
}
