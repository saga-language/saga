// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Declaring a generic's type parameters and their bounds.

#include "semantic/analyzer.hpp"

#include <format>

namespace saga {

uint32_t Analyzer::fresh_type_param_id() { return next_type_param_id++; }

TypePtr Analyzer::resolve_constraint_bound(const IdentifierNode &constraint) {
  if (auto sym = lookup(std::string(constraint.name))) {
    if (sym->type && sym->type->kind == TypeKind::Interface)
      return sym->type;
    error(constraint.span,
          std::format("'{}' is not an interface — a constraint must be an "
                      "interface or one of integer, float, numeric",
                      constraint.name));
    return nullptr;
  }
  if (constraint.name == "Stringable" && builtins.stringable_iface)
    return builtins.stringable_iface;
  if (constraint.name == "Hashable" && builtins.hashable_iface)
    return builtins.hashable_iface;
  error(constraint.span,
        std::format("unknown type constraint '{}' — expected an interface or "
                    "one of integer, float, numeric",
                    constraint.name));
  return nullptr;
}

// A type parameter is a binding like any other, and shadowing one is an error;
// it would leave the type of that name unreachable inside the declaration.
void Analyzer::reject_shadowed_types(const GenericNode &generic) {
  for (auto &tp_node : generic.type_params) {
    auto name = type_param_name(*tp_node);
    if (!name)
      continue;
    auto sym = lookup(std::string(*name));
    if (sym && (sym->kind == SymbolKind::Type ||
                sym->kind == SymbolKind::TypeParam))
      error(tp_node->span,
            std::format("type parameter '{}' shadows a type of the same name",
                        *name));
  }
}

std::vector<TypeParam> Analyzer::enter_generics(const GenericNode &generic) {
  std::vector<TypeParam> params;
  for (auto &tp_node : generic.type_params) {
    std::string_view name;
    Span span = tp_node->span;
    TypeParam tp{fresh_type_param_id(), ""};

    if (auto *node = std::get_if<TypeParamNode>(&tp_node->data)) {
      name = node->name.name;
      span = node->name.span;
      if (node->constraint)
        bound_param(tp, *node->constraint);
    } else if (auto *ident = std::get_if<IdentifierNode>(&tp_node->data)) {
      // Legacy / instantiation-position shape; no constraint slot.
      name = ident->name;
      span = ident->span;
    } else {
      continue; // shape unrecognised; analyzer cannot bind it
    }

    tp.name = std::string(name);
    auto tp_type = make_type_param(tp);
    declare(Symbol::type_param(tp.name, tp_type, span));
    current_scope->type_bindings[tp.id] = tp_type;
    params.push_back(std::move(tp));
  }
  return params;
}

void Analyzer::bound_param(TypeParam &param, const IdentifierNode &bound) {
  param.constraint = constraint_from_name(bound.name);
  if (param.constraint != TypeConstraint::None)
    return;
  if (auto iface = resolve_constraint_bound(bound))
    param.interfaces.push_back(iface);
}

std::optional<std::string> Analyzer::unmet_bound(const TypePtr &arg,
                                                 const TypeParam &param) {
  if (param.constraint != TypeConstraint::None &&
      !satisfies_constraint(arg, param.constraint))
    return std::format("{} {}", param.name, constraint_name(param.constraint));
  for (auto &iface : param.interfaces)
    if (!satisfies_interface(arg, iface))
      return std::format("{} {}", param.name, type_to_string(iface));
  return std::nullopt;
}

bool Analyzer::report_unmet_bound(const TypePtr &arg, const TypeParam &param,
                                  Span span) {
  bool met = true;
  if (param.constraint != TypeConstraint::None &&
      !satisfies_constraint(arg, param.constraint)) {
    error(span, std::format("type {} does not satisfy constraint {}",
                            type_to_string(arg),
                            constraint_name(param.constraint)));
    met = false;
  }
  for (auto &iface : param.interfaces)
    met = report_interface_unsatisfied(
              arg, iface, std::get<InterfaceTypeInfo>(iface->detail).name,
              span, "") &&
          met;
  return met;
}

} // namespace saga
