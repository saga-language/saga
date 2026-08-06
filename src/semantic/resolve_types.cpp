// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Turning a type annotation into a TypePtr: the named types, the containers,
// unions, function types and generic applications. The rules a union enforces
// on its alternatives live here because this is where they are composed.

#include "semantic/analyzer.hpp"
#include <format>

namespace saga {

TypePtr Analyzer::resolve_type(const Node &node) {
  return std::visit(
      overloaded{
          [&](const IdentifierNode &n) -> TypePtr {
            return resolve_identifier_type(n);
          },
          [&](const ArrayTypeNode &n) -> TypePtr {
            return resolve_array_type(n);
          },
          [&](const MapTypeNode &n) -> TypePtr { return resolve_map_type(n); },
          [&](const FuncTypeNode &n) -> TypePtr {
            return resolve_func_type(n);
          },
          [&](const UnionTypeNode &n) -> TypePtr {
            return resolve_union_type(n);
          },
          [&](const GenericTypeAppNode &n) -> TypePtr {
            return resolve_generic_type_app(n);
          },
          [&](const SelectorNode &n) -> TypePtr {
            return resolve_selector_type(n);
          },
          [&](const auto &) -> TypePtr {
            error(node.span, "expected type expression");
            return builtins.invalid_type;
          },
      },
      node.data);
}

TypePtr Analyzer::resolve_identifier_type(const IdentifierNode &node) {
  std::string name(node.name);
  auto sym = lookup(name);
  if (!sym) {
    undefined_error(node.span, name);
    return builtins.invalid_type;
  }
  if (sym->kind != SymbolKind::Type && sym->kind != SymbolKind::TypeParam) {
    error(node.span, std::format("'{}' is not a type", name));
    return builtins.invalid_type;
  }
  if (!sym->type) {
    ensure_type_resolved(name);
    sym = lookup(name);
  }
  return sym && sym->type ? sym->type : builtins.invalid_type;
}

TypePtr Analyzer::resolve_selector_type(const SelectorNode &node) {
  auto *obj_ident = std::get_if<IdentifierNode>(&node.object->data);
  if (!obj_ident) {
    error(node.span, "expected package name in qualified type");
    return builtins.invalid_type;
  }
  auto mod_sym = lookup(std::string(obj_ident->name));
  if (!mod_sym || !mod_sym->type ||
      mod_sym->type->kind != TypeKind::Module) {
    error(obj_ident->span,
          std::format("'{}' is not a package", obj_ident->name));
    return builtins.invalid_type;
  }
  record_symbol(*node.object, *mod_sym);
  auto &mod = std::get<ModuleTypeInfo>(mod_sym->type->detail);
  std::string type_name(node.field.name);
  for (auto &exp : mod.exports) {
    if (exp.name == type_name && exp.type)
      return exp.type;
  }
  error(node.field.span,
        std::format("package '{}' has no exported type '{}'",
                    mod.name, type_name));
  return builtins.invalid_type;
}

TypePtr Analyzer::resolve_array_type(const ArrayTypeNode &node) {
  auto elem = resolve_type(*node.element_type);
  return make_array_type(std::move(elem));
}

TypePtr Analyzer::resolve_map_type(const MapTypeNode &node) {
  auto key = resolve_type(*node.key_type);
  auto val = resolve_type(*node.value_type);
  check_satisfies_protocol(key, ProtocolKind::Hashable, node.key_type->span,
                           "map key");
  return make_map_type(std::move(key), std::move(val));
}

TypePtr Analyzer::resolve_func_type(const FuncTypeNode &node) {
  std::vector<TypePtr> params;
  for (auto &p : node.params)
    params.push_back(resolve_type(*p));
  TypePtr ret = node.return_type ? resolve_type(*node.return_type) : nullptr;
  return make_func_type(std::move(params), std::move(ret));
}

TypePtr Analyzer::resolve_union_type(const UnionTypeNode &node) {
  std::vector<TypePtr> alts;
  std::vector<TypePtr> seen; // flattened members, for duplicate detection
  for (auto &t : node.types) {
    auto rt = resolve_type(*t);
    alts.push_back(rt);

    // Rule 1 — concrete types only. A TypeParam is allowed (concrete after
    // instantiation); an interface (by any alias) is not.
    if (auto u = unwrap_alias(rt); u && u->kind == TypeKind::Interface) {
      error(t->span,
            std::format("a type union may only contain concrete types; "
                        "'{}' is an interface",
                        type_to_string(rt)));
      continue;
    }

    // Rule 2 — a union is a set of values, and `void` is the absence of one.
    // What reaches for this slot is a value carrying no information, which is
    // a different thing and has its own name.
    if (auto u = unwrap_alias(rt); u && u->kind == TypeKind::Void) {
      error(t->span,
            "a type union may only contain values; 'void' is the absence of a "
            "value — use 'Null' for a value that carries nothing");
      continue;
    }

    // Rule 3 — the composed set must be unique. A union alternative is spliced,
    // so `T | (T | U)` and `T | T` both report the repeat.
    for (auto &m : flatten_union_alternatives({rt})) {
      bool dup = false;
      for (auto &s : seen)
        if (types_equal(s, m)) { dup = true; break; }
      if (dup)
        error(t->span, std::format("duplicate type '{}' in union",
                                   type_to_string(m)));
      else
        seen.push_back(m);
    }
  }
  return make_union_type(std::move(alts));
}

// Whether the arguments are the struct's own parameters, in order.
static bool names_own_params(const StructTypeInfo &info,
                             const std::vector<TypePtr> &args) {
  if (args.size() != info.type_params.size())
    return false;
  for (size_t i = 0; i < args.size(); ++i) {
    if (!args[i] || args[i]->kind != TypeKind::TypeParam)
      return false;
    if (std::get<TypeParamInfo>(args[i]->detail).param.id !=
        info.type_params[i].id)
      return false;
  }
  return true;
}

TypePtr
Analyzer::resolve_generic_type_app(const GenericTypeAppNode &node) {
  auto base = resolve_type(*node.base_type);
  if (is_invalid_type(base))
    return builtins.invalid_type;

  // Resolve concrete type arguments.
  std::vector<TypePtr> args;
  for (auto &ta : node.type_args)
    args.push_back(resolve_type(*ta));

  // The base must be a generic struct (Task, etc.).
  if (base->kind != TypeKind::Struct) {
    error(node.span,
          std::format("type {} is not generic", type_to_string(base)));
    return builtins.invalid_type;
  }

  auto &info = std::get<StructTypeInfo>(base->detail);
  if (info.type_params.empty()) {
    error(node.span,
          std::format("type {} is not generic", type_to_string(base)));
    return builtins.invalid_type;
  }

  if (args.size() != info.type_params.size()) {
    error(node.span,
          std::format("expected {} type argument(s), got {}",
                      info.type_params.size(), args.size()));
    return builtins.invalid_type;
  }

  // Inside its own body a struct names itself: `Node<T>` there is this very
  // declaration, whose fields are still being filled. Instantiating now would
  // snapshot an empty field list.
  if (resolving_structs_.count(info.name) && names_own_params(info, args))
    return base;

  // Build bindings and instantiate.
  std::unordered_map<uint32_t, TypePtr> bindings;
  for (size_t i = 0; i < info.type_params.size(); ++i)
    bindings[info.type_params[i].id] = args[i];

  std::vector<FieldInfo> new_fields;
  for (auto &f : info.fields)
    new_fields.push_back({f.name, substitute(f.type, bindings), f.is_public});
  std::vector<MethodInfo> new_methods;
  for (auto &m : info.methods)
    new_methods.push_back(
        {m.name, substitute(m.signature, bindings), m.is_public,
         m.origin_package});

  auto result =
      make_struct_type(info.name, std::move(new_fields), std::move(new_methods),
                       {}, info.origin_package);
  auto &ri = std::get<StructTypeInfo>(result->detail);
  ri.type_params = info.type_params;
  ri.type_args = std::move(args);
  ri.embeds = info.embeds;
  return result;
}

} // namespace saga
