// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// The expression checker's front door: the dispatch over node kinds, the
// literals, and the composite literals whose element types are inferred from
// their contents. Each larger expression form has its own file beside this one.

#include "semantic/analyzer.hpp"
#include <format>

namespace saga {

TypePtr Analyzer::check_expr(const Node &node) {
  auto type = std::visit(
      overloaded{
          [&](const IdentifierNode &n) -> TypePtr {
            return check_identifier(n, node);
          },
          [&](const BoolLiteralNode &n) -> TypePtr {
            return check_bool_literal(n);
          },
          [&](const EnumShorthandNode &n) -> TypePtr {
            error(n.span,
                  std::format("cannot infer enum type for '.{}' here; name the "
                              "enum (Type.{}) or supply a target type",
                              n.variant.name, n.variant.name));
            return builtins.invalid_type;
          },
          [&](const IntegerLiteralNode &n) -> TypePtr {
            return check_int_literal(n);
          },
          [&](const FloatLiteralNode &n) -> TypePtr {
            return check_float_literal(n);
          },
          [&](const StringLiteralNode &n) -> TypePtr {
            return check_string_literal(n);
          },
          [&](const StringFragmentNode &) -> TypePtr {
            return builtins.string_type;
          },
          [&](const ArrayLiteralNode &n) -> TypePtr {
            return check_array_literal(n);
          },
          [&](const MapLiteralNode &n) -> TypePtr {
            return check_map_literal(n);
          },
          [&](const StructLiteralNode &n) -> TypePtr {
            return check_struct_literal(n);
          },
          [&](const BinaryExprNode &n) -> TypePtr {
            return check_binary_expr(n, node);
          },
          [&](const UnaryExprNode &n) -> TypePtr {
            return check_unary_expr(n);
          },
          [&](const IsExpr &n) -> TypePtr { return check_is_expr(n); },
          [&](const GroupExprNode &n) -> TypePtr {
            return check_group_expr(n);
          },
          [&](const CallExprNode &n) -> TypePtr {
            return check_call_expr(n, node);
          },
          [&](const IndexExprNode &n) -> TypePtr {
            return check_index_expr(n);
          },
          [&](const SelectorNode &n) -> TypePtr {
            return check_selector(n, node);
          },
          [&](const IfExprNode &n) -> TypePtr { return check_if_expr(n); },
          [&](const SwitchExprNode &n) -> TypePtr {
            return check_switch_expr(n);
          },
          [&](const ForExprNode &n) -> TypePtr { return check_for_expr(n); },
          [&](const SpawnExprNode &n) -> TypePtr {
            return check_spawn_expr(n, node);
          },
          [&](const OrExprNode &n) -> TypePtr { return check_or_expr(n); },
          [&](const FuncExprNode &n) -> TypePtr {
            return check_func_expr(n, node);
          },
          [&](const ImportExprNode &n) -> TypePtr {
            return check_import_expr(n);
          },
          [&](const BlockNode &n) -> TypePtr {
            push_scope(ScopeKind::Block);
            auto t = check_block(n);
            pop_scope();
            return t;
          },
          // Statements appearing in expression position return Void.
          [&](const VarDeclNode &n) -> TypePtr {
            check_var_decl(n, node);
            return builtins.void_type;
          },
          [&](const DeclAssignNode &n) -> TypePtr {
            check_decl_assign(n);
            return builtins.void_type;
          },
          [&](const AssignNode &n) -> TypePtr {
            check_assign(n);
            return builtins.void_type;
          },
          [&](const IncrementNode &n) -> TypePtr {
            check_increment(n);
            return builtins.void_type;
          },
          [&](const DecrementNode &n) -> TypePtr {
            check_decrement(n);
            return builtins.void_type;
          },
          [&](const ReturnNode &n) -> TypePtr {
            check_return(n);
            return builtins.void_type;
          },
          [&](const BreakNode &n) -> TypePtr {
            check_break(n);
            return builtins.void_type;
          },
          [&](const NextNode &) -> TypePtr { return builtins.void_type; },
          [&](const ArrayTypeNode &) -> TypePtr {
            return reject_type_as_value(node);
          },
          [&](const MapTypeNode &) -> TypePtr {
            return reject_type_as_value(node);
          },
          [&](const UnionTypeNode &) -> TypePtr {
            return reject_type_as_value(node);
          },
          [&](const FuncTypeNode &) -> TypePtr {
            return reject_type_as_value(node);
          },
          [&](const GenericTypeAppNode &) -> TypePtr {
            return reject_type_as_value(node);
          },
          [&](const auto &) -> TypePtr {
            return poison(node.span, "expression kind has no type rule");
          },
      },
      node.data);

  record_type(node, type);
  return type;
}

static bool is_type_expr_node(const Node &node) {
  return std::holds_alternative<ArrayTypeNode>(node.data) ||
         std::holds_alternative<MapTypeNode>(node.data) ||
         std::holds_alternative<UnionTypeNode>(node.data) ||
         std::holds_alternative<FuncTypeNode>(node.data) ||
         std::holds_alternative<GenericTypeAppNode>(node.data);
}

TypePtr Analyzer::check_type_or_value_expr(const Node &node) {
  if (auto *id = std::get_if<IdentifierNode>(&node.data)) {
    auto sym = lookup(std::string(id->name));
    // A type name and a package name are both legal here and nowhere else a
    // value is expected, which is why this path exists separately from
    // check_expr.
    if (sym && (sym->kind == SymbolKind::Type ||
                sym->kind == SymbolKind::Module)) {
      record_symbol(node, *sym);
      auto type = sym->type ? sym->type : builtins.invalid_type;
      record_type(node, type);
      return type;
    }
    return check_expr(node);
  }
  if (is_type_expr_node(node)) {
    auto type = resolve_type(node);
    record_type(node, type);
    return type;
  }
  return check_expr(node);
}

TypePtr Analyzer::reject_type_as_value(const Node &node) {
  error(node.span, std::format("cannot use type '{}' as a value",
                               type_to_string(resolve_type(node))));
  return builtins.invalid_type;
}

TypePtr Analyzer::check_identifier(const IdentifierNode &ident,
                                   const Node &parent) {
  std::string name(ident.name);
  // Spec docs/language.md:25-27 — underscore-prefixed names are "ignored"
  // variables: legal to declare, illegal to read back.  Declaration sites
  // (`_x := 1`) bypass this path entirely; a use here is therefore a
  // read attempt or a re-assignment, both of which the spec forbids.
  if (!name.empty() && name[0] == '_') {
    error(ident.span,
          std::format("cannot access ignored variable '{}'", name));
    return builtins.invalid_type;
  }

  auto sym = lookup(name);
  if (!sym) {
    // Already reported during name resolution.
    return builtins.invalid_type;
  }
  record_symbol(parent, *sym);

  if (sym->kind == SymbolKind::Type) {
    if (is_empty_shape(sym->type))
      return sym->type;
    error(ident.span,
          std::format("cannot use type '{}' as a value", name));
    return builtins.invalid_type;
  }

  // A package is introduced by an import binding, never copied by assignment;
  // reaching here means it was used where a value belongs. Selector objects go
  // through check_type_or_value_expr instead.
  if (sym->kind == SymbolKind::Module) {
    error(ident.span,
          std::format("cannot use package '{}' as a value; to bind it to "
                      "another name use `const Name = import \"...\"`",
                      name));
    return builtins.invalid_type;
  }

  // Forward reference inside a constant initialiser.  check_const_decl
  // runs in textual order and assigns each Constant's type as it goes,
  // so a Constant with no type means it was declared but not yet
  // checked — i.e. a sibling declared later.  Spec says this is a
  // compile-time error (docs/language.md:120-122).
  if (sym->kind == SymbolKind::Constant && !sym->type) {
    error(ident.span,
          std::format("constant '{}' read before its own declaration", name));
    return builtins.invalid_type;
  }

  return sym->type ? sym->type : builtins.invalid_type;
}

TypePtr Analyzer::check_bool_literal(const BoolLiteralNode &) {
  return builtins.bool_type;
}

TypePtr Analyzer::check_int_literal(const IntegerLiteralNode &) {
  return make_untyped_int_type();
}

TypePtr Analyzer::check_float_literal(const FloatLiteralNode &) {
  return make_untyped_float_type();
}

TypePtr Analyzer::check_string_literal(const StringLiteralNode &node) {
  // Each interpolated expression must satisfy Stringable so the runtime
  // can invoke `.String()` on it.
  for (auto &frag : node.fragments) {
    if (std::holds_alternative<StringFragmentNode>(frag->data))
      continue;
    auto t = check_expr(*frag);
    check_stringable_recursive(t, frag->span, "interpolated expression");
  }
  return builtins.string_type;
}

TypePtr Analyzer::check_array_literal(const ArrayLiteralNode &node) {
  if (node.elements.empty()) {
    // The element type comes from the context. A hole that never meets one is
    // caught where the binding is made.
    return make_array_type(builtins.unknown_type);
  }
  TypePtr elem_type = nullptr;
  for (auto &elem : node.elements) {
    auto t = check_expr(*elem);
    reject_void_value(elem->span, t, "an array element");
    if (!elem_type) {
      elem_type = t;
      continue;
    }
    if (!is_invalid_type(t) && !is_invalid_type(elem_type))
      expect_assignable(elem->span, elem_type, t, "array element");
  }
  return make_array_type(elem_type);
}

TypePtr Analyzer::check_map_literal(const MapLiteralNode &node) {
  if (node.entries.empty()) {
    return make_map_type(builtins.unknown_type, builtins.unknown_type);
  }
  TypePtr key_type = nullptr;
  TypePtr val_type = nullptr;
  for (auto &entry : node.entries) {
    auto kt = check_expr(*entry.key);
    auto vt = check_expr(*entry.value);
    reject_void_value(entry.key->span, kt, "a map key");
    reject_void_value(entry.value->span, vt, "a map value");
    if (!key_type) {
      key_type = kt;
      val_type = vt;
      continue;
    }
    if (!is_invalid_type(kt))
      expect_assignable(entry.key->span, key_type, kt, "map key");
    if (!is_invalid_type(vt))
      expect_assignable(entry.value->span, val_type, vt, "map value");
  }
  check_satisfies_protocol(key_type, ProtocolKind::Hashable,
                           node.entries[0].key->span, "map key");
  return make_map_type(key_type, val_type);
}

TypePtr Analyzer::check_struct_literal(const StructLiteralNode &node) {
  auto type_expr_type = check_type_or_value_expr(*node.type_expr);
  if (is_invalid_type(type_expr_type))
    return builtins.invalid_type;

  // For alias types, unwrap to get the underlying struct type for validation,
  // but return the alias type so the variable retains its alias identity.
  auto struct_type = type_expr_type;
  if (struct_type->kind == TypeKind::Alias) {
    struct_type = unwrap_alias(struct_type);
  }

  if (!struct_type || struct_type->kind != TypeKind::Struct) {
    error(node.type_expr->span, std::format("'{}' is not a struct type",
                                            type_to_string(type_expr_type)));
    return builtins.invalid_type;
  }

  auto &raw_info = std::get<StructTypeInfo>(struct_type->detail);

  // Pre-map field name → declared type for non-generic structs so a `.Variant`
  // field value can resolve against it. Generic field types are TypeParams,
  // which a shorthand can't resolve against, so it stays a plain check there.
  std::unordered_map<std::string, TypePtr> field_type_by_name;
  if (raw_info.type_params.empty()) {
    std::vector<FieldInfo> decl_fields;
    collect_promoted_fields(raw_info, decl_fields);
    for (auto &fi : decl_fields)
      if (fi.type)
        field_type_by_name.emplace(fi.name, fi.type);
  }

  // Check each field value first and collect types for generic inference.
  std::vector<std::pair<std::string, TypePtr>> field_vals;
  for (auto &fa : node.fields) {
    auto it = field_type_by_name.find(std::string(fa.name.name));
    TypePtr expected = it != field_type_by_name.end() ? it->second : nullptr;
    auto val_type = check_expr_expecting(*fa.value, expected);
    field_vals.push_back({std::string(fa.name.name), val_type});
  }

  // If the struct is generic, instantiate it by inferring type params.
  auto effective_type = struct_type;
  if (!raw_info.type_params.empty()) {
    effective_type =
        instantiate_generic_struct(struct_type, field_vals, node.span);
    if (is_invalid_type(effective_type))
      return builtins.invalid_type;
  }

  auto &info = std::get<StructTypeInfo>(effective_type->detail);

  // Collect all fields including those promoted from embedded types
  // (transitively). Own fields come first, so a child field shadows an
  // embedded one of the same name.
  std::vector<FieldInfo> all_fields;
  collect_promoted_fields(info, all_fields);

  // Validate each field assignment against the (possibly instantiated) type.
  for (size_t i = 0; i < field_vals.size(); ++i) {
    auto &[fname, val_type] = field_vals[i];
    bool found = false;
    for (auto &fi : all_fields) {
      if (fi.name == fname) {
        found = true;
        if (fi.type && !is_invalid_type(val_type)) {
          expect_assignable(node.span, fi.type, val_type,
                            std::format("field '{}'", fname));
        }
        // Record the field's declared type on the field-name span so
        // that LSP hover can display it (the IdentifierNode is not a
        // Node*, so node_types can't be used).
        if (fi.type) {
          auto &st = current_instantiation_ ? current_instantiation_->span_types
                                            : span_types;
          st.push_back({node.fields[i].name.span, fi.type});
        }
        break;
      }
    }
    if (found)
      continue;

    // A key may also name an embed, which initialises the embedded value as a
    // whole — the only way to set storage a child field shadows.
    if (auto embed = embed_by_name(info, fname)) {
      if (!is_invalid_type(val_type))
        expect_assignable(node.span, embed, val_type,
                          std::format("embedded '{}'", fname));
      auto &st = current_instantiation_ ? current_instantiation_->span_types
                                        : span_types;
      st.push_back({node.fields[i].name.span, embed});
      continue;
    }

    error(node.span,
          std::format("struct '{}' has no field '{}'", info.name, fname));
  }

  // If the original type was an alias, return the alias type so the
  // variable retains its alias identity.
  if (type_expr_type->kind == TypeKind::Alias)
    return type_expr_type;

  return effective_type;
}

} // namespace saga
