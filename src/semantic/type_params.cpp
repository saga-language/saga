// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Declaring a generic's type parameters and their bounds.

#include "semantic/analyzer.hpp"

#include <algorithm>
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

std::vector<TypeParam>
Analyzer::enter_generics(const GenericNode &generic,
                         const std::vector<TypeParam> &declared) {
  std::vector<TypeParam> params;
  for (size_t i = 0; i < generic.type_params.size(); ++i) {
    auto read = read_type_param(*generic.type_params[i]);
    if (!read)
      continue;
    auto &[tp, span] = *read;
    if (i < declared.size())
      add_bound(tp, declared[i], span);
    auto tp_type = make_type_param(tp);
    declare(Symbol::type_param(tp.name, tp_type, span));
    current_scope->type_bindings[tp.id] = tp_type;
    params.push_back(std::move(tp));
  }
  return params;
}

std::vector<TypeParam> Analyzer::enter_func_generics(const FuncDeclNode &fn) {
  if (!fn.generic)
    return {};
  if (!fn.receiver)
    return enter_generics(*fn.generic);
  return enter_generics(*fn.generic, declared_receiver_bounds(*fn.receiver));
}

std::optional<std::pair<TypeParam, Span>>
Analyzer::read_type_param(const Node &node) {
  TypeParam tp{fresh_type_param_id(), ""};
  if (auto *param = std::get_if<TypeParamNode>(&node.data)) {
    tp.name = std::string(param->name.name);
    if (param->constraint)
      bound_param(tp, *param->constraint);
    return std::pair{std::move(tp), param->name.span};
  }
  // Legacy / instantiation-position shape; no constraint slot.
  if (auto *ident = std::get_if<IdentifierNode>(&node.data)) {
    tp.name = std::string(ident->name);
    return std::pair{std::move(tp), ident->span};
  }
  return std::nullopt;
}

// A method is a method of every instance its type admits, so its receiver's
// parameters hold to the bounds the type declares for them. A map's key is
// Hashable: that is how a map finds it.
std::vector<TypeParam>
Analyzer::declared_receiver_bounds(const ReceiverNode &receiver) {
  if (std::holds_alternative<MapTypeNode>(receiver.type->data) &&
      builtins.hashable_iface)
    return {TypeParam{0, "K", TypeConstraint::None, {builtins.hashable_iface}},
            TypeParam{0, "V"}};
  auto *app = std::get_if<GenericTypeAppNode>(&receiver.type->data);
  if (!app)
    return {};
  auto *base = std::get_if<IdentifierNode>(&app->base_type->data);
  if (!base)
    return {};
  ensure_type_resolved(std::string(base->name));
  auto sym = lookup(std::string(base->name));
  if (!sym || !sym->type || sym->type->kind != TypeKind::Struct)
    return {};
  return std::get<StructTypeInfo>(sym->type->detail).type_params;
}

// The parameter must meet both bounds.
void Analyzer::add_bound(TypeParam &param, const TypeParam &declared,
                         Span span) {
  narrow_constraint(param, declared.constraint, span);
  std::vector<TypePtr> interfaces;
  for (auto &iface : declared.interfaces)
    if (std::ranges::none_of(param.interfaces, [&](const TypePtr &t) {
          return types_equal(t, iface);
        }))
      interfaces.push_back(iface);
  interfaces.insert(interfaces.end(), param.interfaces.begin(),
                    param.interfaces.end());
  param.interfaces = std::move(interfaces);
}

// To the smaller of the two sets; disjoint sets admit no type at all.
void Analyzer::narrow_constraint(TypeParam &param, TypeConstraint adds,
                                 Span span) {
  auto has = param.constraint;
  if (adds == TypeConstraint::None || constraint_within(has, adds))
    return;
  if (has != TypeConstraint::None && !constraint_within(adds, has))
    error(span, std::format("{} is declared {}, so it cannot also be {}",
                            param.name, constraint_name(adds),
                            constraint_name(has)));
  param.constraint = adds;
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
