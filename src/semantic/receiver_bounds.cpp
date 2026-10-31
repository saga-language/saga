// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// A method on a generic struct may bound its receiver's parameters
// (`fn (b Box<T numeric>) Twice() T`). It is then a method of only the
// instances whose type arguments meet those bounds: elsewhere it is neither
// callable nor counted toward an interface.

#include "semantic/analyzer.hpp"

#include <format>

namespace saga {

namespace {

// `T numeric`, `T Named`: the parameter as its declaration bounds it.
std::string describe_param(const TypePtr &param) {
  auto &info = std::get<TypeParamInfo>(param->detail);
  if (info.param.constraint != TypeConstraint::None)
    return std::format("{} {}", info.param.name,
                       constraint_name(info.param.constraint));
  if (info.bound)
    return std::format("{} {}", info.param.name, type_to_string(*info.bound));
  return info.param.name;
}

} // namespace

bool Analyzer::meets_param(const TypePtr &arg, const TypePtr &param) {
  if (!param || param->kind != TypeKind::TypeParam)
    return true;
  auto &info = std::get<TypeParamInfo>(param->detail);
  auto c = info.param.constraint;
  if (c != TypeConstraint::None && !satisfies_constraint(arg, c))
    return false;
  return !info.bound || satisfies_interface(arg, *info.bound);
}

std::optional<std::string>
Analyzer::unmet_receiver_bound(const TypePtr &instance,
                               std::string_view name) {
  auto *methods = declared_methods(instance);
  if (!methods || instance->kind != TypeKind::Struct)
    return std::nullopt;
  auto &args = std::get<StructTypeInfo>(instance->detail).type_args;
  for (auto &m : *methods) {
    if (m.name != name)
      continue;
    for (size_t i = 0; i < m.receiver_params.size() && i < args.size(); ++i)
      if (!meets_param(args[i], m.receiver_params[i]))
        return std::format("it needs {}, and {} is {}",
                           describe_param(m.receiver_params[i]),
                           type_to_string(m.receiver_params[i]),
                           type_to_string(args[i]));
    return std::nullopt;
  }
  return std::nullopt;
}

} // namespace saga
