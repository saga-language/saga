// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Type-checking statements, blocks, and top-level declarations — everything
// whose value is either discarded or is the block's own result. Expression
// checking, which every case here calls into, lives in check_exprs.cpp.

#include "semantic/analyzer.hpp"
#include "semantic/analyzer_detail.hpp"
#include <format>

namespace saga {

void Analyzer::check_stmt(const Node &node) {
  std::visit(overloaded{
                 [&](const VarDeclNode &n) { check_var_decl(n, node); },
                 [&](const DeclAssignNode &n) { check_decl_assign(n); },
                 [&](const AssignNode &n) { check_assign(n); },
                 [&](const IncrementNode &n) { check_increment(n); },
                 [&](const DecrementNode &n) { check_decrement(n); },
                 [&](const ReturnNode &n) { check_return(n); },
                 [&](const BreakNode &n) { check_break(n); },
                 [&](const NextNode &) { check_next({}); },
                 [&](const auto &) { check_expr(node); },
             },
             node.data);
}

void Analyzer::check_var_decl(const VarDeclNode &var, const Node &parent) {
  TypePtr declared_type = nullptr;
  if (var.type) {
    declared_type = resolve_type(**var.type);
    // Record the resolved type for the type annotation node so codegen
    // can look it up after analysis (when scopes are no longer available).
    if (declared_type)
      record_type(**var.type, declared_type);
  }

  TypePtr final_type = declared_type;

  if (var.init) {
    TypePtr init_type;
    if (auto *for_node = std::get_if<ForExprNode>(&(*var.init)->data)) {
      init_type = check_for_expr(*for_node, declared_type);
      record_type(**var.init, init_type);
    } else {
      init_type = check_expr_expecting(**var.init, declared_type);
    }
    // An empty `[]` / `{}` adopts the declared type — its element type is a
    // hole and the declaration is the context that fills it. The hole only
    // yields to a declaration of the same shape, so `a int = []` still fails.
    if (declared_type && contains_unknown(init_type)) {
      TypePtr underlying = unwrap_alias(declared_type);
      if (underlying && init_type && underlying->kind == init_type->kind)
        init_type = declared_type;
    }
    if (declared_type && !is_invalid_type(init_type)) {
      expect_assignable((*var.init)->span, declared_type, init_type,
                        "variable initializer");
    }
    if (!final_type)
      final_type = resolve_binding_type(materialize_untyped(init_type),
                                        (*var.init)->span);
  }

  // Update or create the symbol in the current scope.
  std::string name(var.name.name);
  // Ahead of the ignored-name exit: an ignored name still declares storage,
  // so `_ := f()` on a void `f` is a void slot. Call it as a statement.
  reject_void_value(var.name.span, final_type, "a variable");
  if (is_ignored_name(name))
    return;
  auto sym_it = current_scope->symbols.find(name);
  if (sym_it != current_scope->symbols.end()) {
    sym_it->second.type = final_type;
  } else {
    current_scope->symbols.emplace(
        name, Symbol::variable(name, final_type, var.name.span));
  }
}

// A binding is an inference hole's last chance to be filled: nothing after it
// supplies a type. Spec: "arr1 := [] // invalid, no inferrable type"
// (docs/language.md:601).
TypePtr Analyzer::resolve_binding_type(TypePtr type, Span span) {
  if (!contains_unknown(type))
    return type;
  error(span, type->kind == TypeKind::Map
                  ? "empty map literal: cannot infer key/value type without a "
                    "declared type"
                  : "empty array literal: cannot infer element type without a "
                    "declared type");
  return builtins.invalid_type;
}

void Analyzer::check_decl_assign(const DeclAssignNode &decl) {
  auto rhs_type = resolve_binding_type(
      materialize_untyped(check_expr(*decl.value)), decl.value->span);

  for (auto &ident : decl.targets.identifiers) {
    std::string name(ident.name);
    reject_void_value(ident.span, rhs_type, "a variable");
    if (is_ignored_name(name))
      continue;
    auto sym_it = current_scope->symbols.find(name);
    if (sym_it != current_scope->symbols.end()) {
      sym_it->second.type = rhs_type;
    } else {
      // Symbol was declared during name resolution in a different scope
      // tree.  Re-declare it here so type information propagates.
      current_scope->symbols.emplace(
          name, Symbol::variable(name, rhs_type, ident.span));
    }
  }
}

const IdentifierNode *Analyzer::target_root(const Node &target) {
  if (auto *id = std::get_if<IdentifierNode>(&target.data))
    return id;
  if (auto *sel = std::get_if<SelectorNode>(&target.data))
    return target_root(*sel->object);
  if (auto *idx = std::get_if<IndexExprNode>(&target.data))
    return target_root(*idx->object);
  return nullptr;
}

// A write needs somewhere to land. Without this the store is emitted against a
// temporary and silently discarded, so the statement compiles and does nothing.
void Analyzer::reject_immutable_target(const Node &target,
                                       std::string_view verb) {
  auto *root = target_root(target);
  if (!root) {
    error(target.span,
          std::format("cannot {} a temporary value: the write would be "
                      "discarded",
                      verb));
    return;
  }

  auto sym = lookup(std::string(root->name));
  if (!sym)
    return;

  if (sym->kind == SymbolKind::Constant) {
    error(target.span,
          std::format("cannot {} constant '{}'", verb, root->name));
    return;
  }

  if (sym->kind != SymbolKind::Variable && sym->kind != SymbolKind::Parameter)
    error(target.span,
          std::format("cannot {} '{}': not a variable", verb, root->name));
}

// Errors are immutable pure data: reject writing a field via `=`, a compound
// assignment, or `++`/`--`.
void Analyzer::reject_error_field_mutation(const Node &target) {
  if (auto *sel = std::get_if<SelectorNode>(&target.data))
    if (is_error_valued(check_expr(*sel->object)))
      error(target.span,
            "cannot assign to a field of an error (errors are immutable)");
}

void Analyzer::check_assign(const AssignNode &node) {
  // Check each target and value.
  for (size_t i = 0; i < node.targets.size(); ++i) {
    reject_immutable_target(*node.targets[i], "assign to");
    reject_error_field_mutation(*node.targets[i]);

    auto target_type = check_expr(*node.targets[i]);
    if (i < node.values.size()) {
      auto val_type = check_expr(*node.values[i]);

      if (node.op == Token::Kind::Assignment) {
        expect_assignable(node.values[i]->span, target_type, val_type,
                          "assignment");
      } else if (node.op == Token::Kind::AddAssignment &&
                 target_type->kind == TypeKind::String) {
        // String concatenation assignment: s += "..."
        expect_assignable(node.values[i]->span, builtins.string_type, val_type,
                          "string concatenation assignment");
      } else if (node.op == Token::Kind::DivAssignment) {
        // Division assignment: x /= y — division can fail (div by zero),
        // so validate numeric but note the impure semantics.
        if (!is_numeric(target_type)) {
          error(node.targets[i]->span,
                std::format("/= requires numeric type, got {}",
                            type_to_string(target_type)));
        }
        if (!is_invalid_type(val_type)) {
          expect_assignable(node.values[i]->span, target_type, val_type,
                            "division assignment");
        }
      } else {
        // Compound assignment: +=, -=, *=
        // Target must be numeric.
        if (!is_numeric(target_type)) {
          error(node.targets[i]->span,
                std::format("compound assignment requires numeric type, "
                            "got {}",
                            type_to_string(target_type)));
        }
        if (!is_invalid_type(val_type)) {
          expect_assignable(node.values[i]->span, target_type, val_type,
                            "compound assignment");
        }
      }
    }
  }
}

void Analyzer::check_increment(const IncrementNode &node) {
  reject_immutable_target(*node.operand, "increment");
  reject_error_field_mutation(*node.operand);
  auto t = check_expr(*node.operand);
  if (!is_invalid_type(t) && t->kind != TypeKind::Int) {
    error(node.span, std::format("increment requires integer type, got {}",
                                 type_to_string(t)));
  }
}

void Analyzer::check_decrement(const DecrementNode &node) {
  reject_immutable_target(*node.operand, "decrement");
  reject_error_field_mutation(*node.operand);
  auto t = check_expr(*node.operand);
  if (!is_invalid_type(t) && t->kind != TypeKind::Int) {
    error(node.span, std::format("decrement requires integer type, got {}",
                                 type_to_string(t)));
  }
}

void Analyzer::check_return(const ReturnNode &node) {
  auto func_scope = current_scope->nearest(ScopeKind::Function);
  if (!func_scope) {
    error(node.span, "'return' outside of function");
    return;
  }

  auto &expected = func_scope->return_types;
  if (!node.value) {
    // Bare return — function must be Void or have no return type.
    if (!expected.empty() && !types_equal(expected[0], builtins.void_type)) {
      error(node.span, "missing return value");
    }
    return;
  }

  if (expected.empty()) {
    error(node.span, "return has a value, expected none");
    return;
  }

  auto val_type = check_expr_expecting(*node.value, expected[0]);
  expect_assignable(node.value->span, expected[0], val_type, "return value");
}

void Analyzer::check_break(const BreakNode &node) {
  if (!current_scope->is_inside(ScopeKind::Loop)) {
    error(node.span, "'break' outside of loop");
    return;
  }
  for (auto &val : node.values) {
    auto t = check_expr(*val);
    if (!break_value_types_.empty())
      break_value_types_.back().push_back(t);
  }
}

void Analyzer::check_next(const NextNode &) {
  // Nothing to type-check for next.
}

// ===========================================================================
// Block type-checking
// ===========================================================================

TypePtr Analyzer::check_block(const BlockNode &block) {
  TypePtr last_type = builtins.void_type;
  for (auto &stmt : block.stmts) {
    last_type = check_expr(*stmt);
  }
  return last_type;
}

// ---------------------------------------------------------------------------
// always_returns — true if every control-flow path through the node ends
// with a `return` statement.
// ---------------------------------------------------------------------------

bool Analyzer::always_returns(const Node &node) const {
  return std::visit(overloaded{
                        [](const ReturnNode &) -> bool { return true; },

                        [&](const BlockNode &b) -> bool {
                          // A block always returns if its last statement always
                          // returns.
                          if (b.stmts.empty())
                            return false;
                          return always_returns(*b.stmts.back());
                        },

                        [&](const IfExprNode &n) -> bool {
                          // Both then and else must exist and both must always
                          // return.
                          if (!n.else_block)
                            return false;
                          bool then_ret = always_returns(*n.then_block);
                          bool else_ret = always_returns(**n.else_block);
                          return then_ret && else_ret;
                        },

                        [&](const SwitchExprNode &n) -> bool {
                          // Every arm must always return, and there must be an
                          // else.
                          if (!n.else_body)
                            return false;
                          for (auto &arm : n.arms) {
                            if (!always_returns(*arm.body))
                              return false;
                          }
                          return always_returns(**n.else_body);
                        },

                        [](const auto &) -> bool { return false; },
                    },
                    node.data);
}

// ===========================================================================
// Top-level declaration type-checking
// ===========================================================================

void Analyzer::check_const_decl(const ConstDeclNode &c) {
  auto sym_it = current_scope->symbols.find(std::string(c.name.name));

  // `const Name = import "..."` is a named-import binding (bound in
  // process_imports), not a value constant.
  if (std::get_if<ImportExprNode>(&c.value->data))
    return;

  TypePtr declared_type = nullptr;
  if (c.type)
    declared_type = resolve_type(**c.type);

  auto init_type = check_expr(*c.value);

  if (!require_const_expr(*c.value)) {
    if (sym_it != current_scope->symbols.end())
      sym_it->second.type = builtins.invalid_type;
    return;
  }

  if (declared_type && !is_invalid_type(init_type))
    expect_assignable(c.value->span, declared_type, init_type,
                      "constant initializer");

  // Forward-ref initializers (e.g. `const A = B * 2` with B declared
  // later) leave init_type as Error.  Spec: zero value with no
  // diagnostic.  Fall back to Int so downstream method dispatch
  // (`A.String()`) and codegen find the right ABI.
  TypePtr const_type = declared_type             ? declared_type
                       : is_invalid_type(init_type) ? builtins.int_type
                                                  : init_type;

  reject_void_value(c.name.span, const_type, "a constant");

  if (sym_it != current_scope->symbols.end())
    sym_it->second.type = const_type;

  if (auto cv = evaluate_constant(*c.value))
    const_decl_values_[std::string(c.name.name)] = *cv;
}

bool Analyzer::reject_const(Span span, const std::string &message) {
  error(span, message);
  return false;
}

static bool string_has_interpolation(const StringLiteralNode &s) {
  for (auto &frag : s.fragments)
    if (!std::get_if<StringFragmentNode>(&frag->data))
      return true;
  return false;
}

bool Analyzer::const_ident_is_value(const IdentifierNode &id, Span span) {
  auto sym = lookup(std::string(id.name));
  if (!sym)
    return reject_const(span, std::format("undefined name '{}'", id.name));
  switch (sym->kind) {
  case SymbolKind::Constant:
  case SymbolKind::EnumVariant:
    return true;
  case SymbolKind::Type:
  case SymbolKind::TypeParam:
    return reject_const(
        span, "a constant must be a value, not a type; use `type` to "
              "declare an alias");
  default:
    return reject_const(
        span, std::format("'{}' is not a compile-time constant", id.name));
  }
}

bool Analyzer::const_selector_is_enum_variant(const SelectorNode &sel,
                                              Span span) {
  if (auto *base = std::get_if<IdentifierNode>(&sel.object->data)) {
    if (auto sym = lookup(std::string(base->name)); sym && sym->type) {
      auto t = unwrap_alias(sym->type);
      if (t->kind == TypeKind::Enum) {
        auto &info = std::get<EnumTypeInfo>(t->detail);
        for (auto &v : info.variants)
          if (v.name == sel.field.name)
            return true;
      }
    }
  }
  return reject_const(
      span, "a selector is not a compile-time constant unless it names an "
            "enum variant");
}

bool Analyzer::require_const_expr(const Node &expr) {
  return std::visit(
      overloaded{
          [](const BoolLiteralNode &) { return true; },
          [](const IntegerLiteralNode &) { return true; },
          [](const FloatLiteralNode &) { return true; },
          [&](const StringLiteralNode &s) {
            return string_has_interpolation(s)
                       ? reject_const(expr.span,
                                      "an interpolated string is not a "
                                      "compile-time constant")
                       : true;
          },
          [&](const GroupExprNode &g) { return require_const_expr(*g.inner); },
          [&](const UnaryExprNode &u) { return require_const_expr(*u.operand); },
          [&](const BinaryExprNode &b) {
            return require_const_expr(*b.lhs) && require_const_expr(*b.rhs);
          },
          [&](const ArrayLiteralNode &a) {
            for (auto &el : a.elements)
              if (!require_const_expr(*el))
                return false;
            return true;
          },
          [&](const StructLiteralNode &s) {
            for (auto &f : s.fields)
              if (!require_const_expr(*f.value))
                return false;
            return true;
          },
          [&](const IdentifierNode &id) {
            return const_ident_is_value(id, expr.span);
          },
          [&](const SelectorNode &sel) {
            return const_selector_is_enum_variant(sel, expr.span);
          },
          [&](const MapLiteralNode &) {
            return reject_const(expr.span,
                                "maps cannot be constants — they need runtime "
                                "construction; use a package-level variable");
          },
          [&](const CallExprNode &) {
            return reject_const(
                expr.span, "a function call is not a compile-time constant");
          },
          [&](const FuncExprNode &) {
            return reject_const(
                expr.span, "a function value is not a compile-time constant");
          },
          [&](const IndexExprNode &) {
            return reject_const(
                expr.span,
                "an indexing expression is not a compile-time constant");
          },
          [&](const SliceNode &) {
            return reject_const(expr.span,
                                "a slice is not a compile-time constant");
          },
          [&](const IfExprNode &) {
            return reject_const(
                expr.span, "an `if` expression is not a compile-time constant");
          },
          [&](const SwitchExprNode &) {
            return reject_const(
                expr.span,
                "a `switch` expression is not a compile-time constant");
          },
          [&](const ForExprNode &) {
            return reject_const(
                expr.span,
                "a `for` expression is not a compile-time constant");
          },
          [&](const SpawnExprNode &) {
            return reject_const(
                expr.span,
                "a `spawn` expression is not a compile-time constant");
          },
          [&](const OrExprNode &) {
            return reject_const(
                expr.span, "an `or` expression is not a compile-time constant");
          },
          [&](const IsExpr &) {
            return reject_const(
                expr.span, "an `is` expression is not a compile-time constant");
          },
          [&](const auto &) {
            return reject_const(expr.span,
                                "this expression is not a compile-time "
                                "constant");
          },
      },
      expr.data);
}

void Analyzer::check_enum_decl(const EnumDeclNode &e) {
  auto sym = lookup(std::string(e.name.name));
  if (!sym || !sym->type || sym->type->kind != TypeKind::Enum)
    return;
  auto &info = std::get<EnumTypeInfo>(sym->type->detail);

  std::unordered_set<std::string> seen_names;
  std::unordered_map<int64_t, std::string> seen_index;
  std::unordered_map<std::string, std::string> seen_string;
  for (size_t i = 0; i < e.fields.size(); ++i) {
    auto &field = e.fields[i];
    std::string vname(field.name.name);
    if (!seen_names.insert(vname).second)
      error(field.name.span, std::format("duplicate enum variant '{}'", vname));

    if (e.string_backed) {
      if (field.value && !plain_string_literal(field.value))
        error(field.value->span,
              std::format("string-backed enum variant '{}' backing value must "
                          "be a string literal",
                          vname));
      if (i < info.variants.size()) {
        auto &sv = info.variants[i].string_value;
        auto [it, inserted] = seen_string.try_emplace(sv, vname);
        if (!inserted)
          error(field.name.span,
                std::format("enum variant '{}' backing string \"{}\" already "
                            "used by '{}'",
                            vname, sv, it->second));
      }
      continue;
    }

    if (field.value && !std::get_if<IntegerLiteralNode>(&field.value->data))
      error(field.value->span,
            std::format("enum variant '{}' backing value must be an integer "
                        "literal",
                        vname));

    if (i < info.variants.size()) {
      int64_t idx = info.variants[i].index;
      auto [it, inserted] = seen_index.try_emplace(idx, vname);
      if (!inserted)
        error(field.name.span,
              std::format("enum variant '{}' has index {} already used by '{}'",
                          vname, idx, it->second));
    }
  }

  auto methods = type_methods_.find(sym->type.get());
  if (methods != type_methods_.end())
    check_method_uniqueness(methods->second, "enum", info.name, e.name.span,
                            {"Int", "String", "From"});
}

TypePtr Analyzer::check_enum_shorthand(const EnumShorthandNode &sh,
                                       const Node &node,
                                       const TypePtr &expected) {
  TypePtr enum_type = expected ? unwrap_alias(expected) : nullptr;
  if (!enum_type || enum_type->kind != TypeKind::Enum) {
    error(sh.span,
          std::format("'.{}' shorthand needs a known enum type here",
                      sh.variant.name));
    return builtins.invalid_type;
  }
  auto &info = std::get<EnumTypeInfo>(enum_type->detail);
  for (auto &v : info.variants)
    if (v.name == sh.variant.name) {
      record_type(node, enum_type);
      return enum_type;
    }
  error(sh.variant.span,
        std::format("enum '{}' has no variant '{}'", info.name,
                    sh.variant.name));
  return builtins.invalid_type;
}

TypePtr Analyzer::check_expr_expecting(const Node &expr,
                                       const TypePtr &expected) {
  if (auto *sh = std::get_if<EnumShorthandNode>(&expr.data))
    return check_enum_shorthand(*sh, expr, expected);
  // An empty collection literal carries no element type of its own, so it
  // takes the one being asked for.
  if (auto exp = unwrap_alias(expected)) {
    if (auto *arr = std::get_if<ArrayLiteralNode>(&expr.data);
        arr && arr->elements.empty() && exp->kind == TypeKind::Array) {
      record_type(expr, exp);
      return exp;
    }
    if (auto *map = std::get_if<MapLiteralNode>(&expr.data);
        map && map->entries.empty() && exp->kind == TypeKind::Map) {
      record_type(expr, exp);
      return exp;
    }
  }
  return check_expr(expr);
}

void Analyzer::check_type_decl(const TypeDeclNode &t) {
  auto sym = lookup(std::string(t.name.name));
  if (!sym || !sym->type || sym->type->kind != TypeKind::Alias)
    return;
  auto &info = std::get<AliasTypeInfo>(sym->type->detail);
  if (info.structural)
    return;
  check_method_uniqueness(info.methods, "type", info.name, t.name.span, {});
}

void Analyzer::check_func_decl(const FuncDeclNode &fn) {
  // The body is checked via check_func_decl_body.
}

void Analyzer::check_method_uniqueness(
    const std::vector<MethodInfo> &methods, std::string_view kind,
    std::string_view name, Span span,
    const std::unordered_set<std::string> &reserved) {
  std::unordered_set<std::string> seen;
  for (auto &m : methods) {
    if (reserved.count(m.name)) {
      error(span, std::format("cannot redefine built-in method '{}' on {} '{}'",
                              m.name, kind, name));
      continue;
    }
    if (!seen.insert(m.name).second)
      error(span, std::format("duplicate method '{}' in {} '{}'", m.name, kind,
                              name));
  }
}

void Analyzer::check_struct_decl(const StructDeclNode &s) {
  // Struct fields and methods are resolved in Phase 2b.  Here we check
  // for duplicate field names and duplicate method names.
  auto sym = lookup(std::string(s.name.name));
  if (!sym || !sym->type || sym->type->kind != TypeKind::Struct)
    return;

  auto &info = std::get<StructTypeInfo>(sym->type->detail);

  // Check duplicate fields.
  std::unordered_map<std::string, bool> seen_fields;
  for (auto &f : info.fields) {
    if (seen_fields.count(f.name)) {
      error(s.span, std::format("duplicate field '{}' in struct '{}'", f.name,
                                info.name));
    }
    seen_fields[f.name] = true;
    reject_void_value(s.span, f.type, std::format("field '{}'", f.name));
  }

  check_method_uniqueness(info.methods, "struct", info.name, s.span, {});

  check_no_infinite_size(sym->type, s.span);
  check_field_defaults(s);
}

// A struct holds its fields inline, so one that reaches itself that way has no
// finite size and no base case to stop at. An array, a map, or a union slot
// holding a boxed alternative puts the value on the heap, which is where a
// recursive shape gets its footing.
static bool reaches_by_value(const TypePtr &origin, const TypePtr &t,
                             std::unordered_set<const Type *> &visiting) {
  auto u = unwrap_alias(t);
  if (!u)
    return false;
  if (same_struct_decl(u, origin))
    return true;
  if (u->kind == TypeKind::Union) {
    for (auto &alt : std::get<UnionTypeInfo>(u->detail).alternatives)
      if (!union_alt_is_boxed(alt) && reaches_by_value(origin, alt, visiting))
        return true;
    return false;
  }
  if (u->kind != TypeKind::Struct)
    return false;
  if (!visiting.insert(u.get()).second)
    return false;
  auto &si = std::get<StructTypeInfo>(u->detail);
  for (auto &f : si.fields)
    if (reaches_by_value(origin, f.type, visiting))
      return true;
  for (auto &e : si.embeds)
    if (reaches_by_value(origin, e, visiting))
      return true;
  return false;
}

void Analyzer::check_no_infinite_size(const TypePtr &struct_type, Span span) {
  auto &info = std::get<StructTypeInfo>(struct_type->detail);
  auto blame = [&](const std::string &member) {
    error(span,
          std::format("'{}' contains itself through '{}', so it has no finite "
                      "size; give it a way to stop — '{} | Missing', an array, "
                      "or a map",
                      info.name, member, info.name));
  };
  for (auto &f : info.fields) {
    std::unordered_set<const Type *> visiting{struct_type.get()};
    if (reaches_by_value(struct_type, f.type, visiting))
      return blame(f.name);
  }
  for (auto &e : info.embeds) {
    std::unordered_set<const Type *> visiting{struct_type.get()};
    if (reaches_by_value(struct_type, e, visiting))
      return blame(type_to_string(e));
  }
}

void Analyzer::check_field_defaults(const StructDeclNode &s) {
  for (auto &member : s.members) {
    if (auto *fs = std::get_if<FieldSpecNode>(&member.member->data))
      check_field_default(*fs);
  }
}

// A field default must be a comptime expression assignable to the field type.
void Analyzer::check_field_default(const FieldSpecNode &fs) {
  if (!fs.default_value)
    return;

  auto field_type = resolve_type(*fs.type);
  auto def_type = check_expr(*fs.default_value);
  if (!require_const_expr(*fs.default_value))
    return;
  if (!field_type || is_invalid_type(def_type))
    return;

  std::string fname = fs.names.identifiers.empty()
                          ? "field"
                          : std::string(fs.names.identifiers.front().name);
  expect_assignable(fs.default_value->span, field_type, def_type,
                    std::format("default for field '{}'", fname));
}

void Analyzer::check_error_decl(const ErrorDeclNode &e) {
  auto sym = lookup(std::string(e.name.name));
  if (!sym || !sym->type || sym->type->kind != TypeKind::Struct)
    return;
  auto &info = std::get<StructTypeInfo>(sym->type->detail);

  // `Missing` and `Trapped` are built-in errors with reserved type_ids; a
  // user redeclaration would alias them (breaking `is`/`==` identity).
  if (info.name == "Missing" || info.name == "Trapped")
    error(e.name.span,
          std::format("'{}' is a reserved built-in error type", info.name));

  std::unordered_map<std::string, bool> seen;
  for (auto &f : info.fields) {
    if (seen.count(f.name))
      error(e.span, std::format("duplicate field '{}' in error '{}'", f.name,
                                info.name));
    seen[f.name] = true;
  }

  if (e.message_default)
    check_error_message_default(info, *e.message_default);

  for (auto &member : e.members) {
    if (auto *fs = std::get_if<FieldSpecNode>(&member.member->data))
      check_field_default(*fs);
  }
}

// The message default is checked with the error's own fields in scope, so a
// `message = "code {code}"` default can interpolate them. It must be a string
// literal (which may interpolate self-fields) or a comptime constant — not an
// arbitrary runtime expression.
void Analyzer::check_error_message_default(const StructTypeInfo &info,
                                           const Node &msg_default) {
  push_scope(ScopeKind::Block);
  for (auto &f : info.fields) {
    if (f.name == "type_id" || f.name == "message")
      continue;
    current_scope->symbols[f.name] =
        Symbol::variable(f.name, f.type, msg_default.span);
  }
  // The message default isn't reached by the normal resolve pass (its scope is
  // the error's fields, not a lexical scope), so resolve it here to report
  // undefined names before the interpolation silently drops them.
  resolve_expr(msg_default);
  auto def_type = check_expr(msg_default);
  pop_scope();

  // A self-interpolating string literal isn't a compile-time constant but is
  // allowed; any other non-constant expression (e.g. a call) is rejected.
  if (!std::holds_alternative<StringLiteralNode>(msg_default.data) &&
      !require_const_expr(msg_default))
    return;

  if (!is_invalid_type(def_type))
    expect_assignable(msg_default.span, builtins.string_type, def_type,
                      "error message default");
}

void Analyzer::check_interface_decl(const InterfaceDeclNode &i) {
  auto sym = lookup(std::string(i.name.name));
  if (!sym || !sym->type || sym->type->kind != TypeKind::Interface)
    return;

  auto &info = std::get<InterfaceTypeInfo>(sym->type->detail);

  if (info.methods.empty()) {
    error(i.span,
          std::format("interface '{}' must declare at least one method "
                      "(directly or through an embedded interface)",
                      info.name));
  }

  check_method_uniqueness(info.methods, "interface", info.name, i.span, {});
}

} // namespace saga
