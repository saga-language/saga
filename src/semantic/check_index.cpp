// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Indexing an array, map or string. A map lookup can miss, so the result type
// is the element widened with the error the miss produces — which is why this
// is longer than the syntax suggests.

#include "semantic/analyzer.hpp"
#include <format>

namespace saga {

TypePtr Analyzer::check_index_expr(const IndexExprNode &node) {
  auto obj_type = check_expr(*node.object);
  if (is_invalid_type(obj_type))
    return builtins.invalid_type;

  // Check for slice.
  if (std::holds_alternative<SliceNode>(node.index->data)) {
    auto &slice = std::get<SliceNode>(node.index->data);
    if (slice.low)
      check_expr(**slice.low);
    if (slice.high)
      check_expr(**slice.high);

    // Slicing a string or array returns the same type.
    if (obj_type->kind == TypeKind::String)
      return builtins.string_type;
    if (obj_type->kind == TypeKind::Array)
      return obj_type;

    error(node.span,
          std::format("cannot slice type {}", type_to_string(obj_type)));
    return builtins.invalid_type;
  }

  auto index_type = check_expr(*node.index);

  switch (obj_type->kind) {
  case TypeKind::Array: {
    auto &arr = std::get<ArrayTypeInfo>(obj_type->detail);
    if (!is_invalid_type(index_type) && index_type->kind != TypeKind::Int) {
      error(node.index->span, "array index must be an integer");
    }
    // Indexing returns T | Error (out of bounds).
    return make_union_type({arr.element, builtins.error_base});
  }
  case TypeKind::Map: {
    auto &map_info = std::get<MapTypeInfo>(obj_type->detail);
    if (!is_invalid_type(index_type)) {
      expect_assignable(node.index->span, map_info.key, index_type, "map key");
    }
    // Map access returns V | Error (missing key).
    return make_union_type({map_info.value, builtins.error_base});
  }
  case TypeKind::String: {
    if (!is_invalid_type(index_type) && index_type->kind != TypeKind::Int) {
      error(node.index->span, "string index must be an integer");
    }
    return builtins.string_type;
  }
  default:
    error(node.span, std::format("type {} does not support indexing",
                                 type_to_string(obj_type)));
    return builtins.invalid_type;
  }
}

TypePtr Analyzer::resolve_module_selector(const ModuleTypeInfo &mod,
                                          const std::string &field_name,
                                          Span field_span) {
  for (auto &exp : mod.exports)
    if (exp.name == field_name)
      return exp.type ? exp.type
                      : poison(field_span, "package export '" + field_name +
                                               "' has no type");
  error(field_span,
        std::format("package '{}' has no exported member '{}'", mod.name,
                    field_name));
  return builtins.invalid_type;
}

namespace {

// Returns the (substituted) member signature for an Array/Map kind method.
TypePtr substitute_kind_method(TypeKind kind, const TypePtr &effective_type,
                               const TypePtr &sig) {
  if (kind == TypeKind::Map) {
    auto &map_info = std::get<MapTypeInfo>(effective_type->detail);
    std::unordered_map<uint32_t, TypePtr> bindings;
    bindings[9991] = map_info.key;
    bindings[9992] = map_info.value;
    return substitute(sig, bindings);
  }
  if (kind == TypeKind::Array) {
    auto &arr_info = std::get<ArrayTypeInfo>(effective_type->detail);
    std::unordered_map<uint32_t, TypePtr> bindings;
    bindings[9990] = arr_info.element;
    return substitute(sig, bindings);
  }
  return sig;
}

} // namespace

TypePtr Analyzer::resolve_struct_member(const TypePtr &owner_type,
                                        const std::string &field_name,
                                        Span field_span) {
  auto &info = std::get<StructTypeInfo>(owner_type->detail);
  for (auto &f : info.fields)
    if (f.name == field_name)
      return f.type ? f.type
                    : poison(field_span,
                             "struct field '" + field_name + "' has no type");

  for (auto &m : info.methods) {
    if (m.name != field_name)
      continue;
    auto sig = m.signature ? m.signature : builtins.invalid_type;
    // A method is monomorphised from the receiver's type arguments alone, so
    // on a concrete instantiation nothing generic may be left in its
    // signature — neither a method-own type parameter nor a generic struct
    // the receiver's arguments do not reach.
    if (!info.type_args.empty() && has_type_params(sig)) {
      error(field_span,
            std::format("cannot call '{}' on '{}': its signature has type "
                        "parameters that the receiver does not bind",
                        field_name, type_to_string(owner_type)));
      return builtins.invalid_type;
    }
    return sig;
  }

  // An embed answers to its own type name, which reaches the embedded value
  // itself. This is the only way to read storage a child field shadows, so it
  // is checked before the promoted-member recursion.
  if (auto embed = embed_by_name(info, field_name))
    return embed;

  // Promoted members: recurse through embeds so a member declared several
  // levels deep is still found. Own members are checked above first, so a
  // child member shadows an embedded one of the same name.
  for (auto &embed : info.embeds) {
    if (!embed || embed->kind != TypeKind::Struct)
      continue;
    if (auto t = resolve_struct_member(embed, field_name, field_span))
      return t;
  }
  return nullptr;
}

// An embed answers to its bare type name, so anything else claiming that name
// would leave the embedded value unreachable. Two embeds of the same name from
// different packages collide for the same reason.
bool Analyzer::embed_name_taken(const Node &embed_node, const TypePtr &embed,
                                const std::vector<FieldInfo> &fields,
                                const std::vector<TypePtr> &seen) {
  auto &einfo = std::get<StructTypeInfo>(embed->detail);

  for (auto &f : fields) {
    if (f.name != einfo.name)
      continue;
    error(embed_node.span,
          std::format("embedded type '{}' collides with a field of the same "
                      "name",
                      einfo.name));
    return true;
  }

  for (auto &other : seen) {
    if (std::get<StructTypeInfo>(other->detail).name != einfo.name)
      continue;
    error(embed_node.span,
          std::format("embedded type '{}' collides with another embed of the "
                      "same name",
                      einfo.name));
    return true;
  }

  return false;
}

TypePtr Analyzer::embed_by_name(const StructTypeInfo &info,
                                const std::string &name) {
  for (auto &embed : info.embeds) {
    if (!embed || embed->kind != TypeKind::Struct)
      continue;
    if (std::get<StructTypeInfo>(embed->detail).name == name)
      return embed;
  }
  return nullptr;
}

void Analyzer::collect_promoted_fields(const StructTypeInfo &info,
                                       std::vector<FieldInfo> &out) {
  out.insert(out.end(), info.fields.begin(), info.fields.end());
  for (auto &embed : info.embeds) {
    if (embed && embed->kind == TypeKind::Struct)
      collect_promoted_fields(std::get<StructTypeInfo>(embed->detail), out);
  }
}

TypePtr Analyzer::resolve_method_signature(const TypePtr &obj_type,
                                           const std::string &field_name,
                                           Span span) {
  auto canonicalize_intrinsic = [this](const TypePtr &t) -> const Type * {
    switch (t->kind) {
    case TypeKind::Int: {
      auto &ii = std::get<IntType>(t->detail);
      if (ii.bits == 0) return builtins.int_type.get();
      if (ii.is_signed) {
        switch (ii.bits) {
        case 8:  return builtins.int8_type.get();
        case 16: return builtins.int16_type.get();
        case 32: return builtins.int32_type.get();
        case 64: return builtins.int64_type.get();
        }
      } else {
        switch (ii.bits) {
        case 8:  return builtins.uint8_type.get();
        case 16: return builtins.uint16_type.get();
        case 32: return builtins.uint32_type.get();
        case 64: return builtins.uint64_type.get();
        }
      }
      return nullptr;
    }
    case TypeKind::Float: {
      auto &fi = std::get<FloatType>(t->detail);
      if (fi.bits == 0) return builtins.float_type.get();
      if (fi.bits == 32) return builtins.float32_type.get();
      if (fi.bits == 64) return builtins.float64_type.get();
      return nullptr;
    }
    case TypeKind::Bool:   return builtins.bool_type.get();
    case TypeKind::String: return builtins.string_type.get();
    default: return nullptr;
    }
  };

  auto find_user_methods = [&]() -> const std::vector<MethodInfo> * {
    auto it = type_methods_.find(obj_type.get());
    if (it == type_methods_.end() && obj_type->kind == TypeKind::Alias) {
      auto underlying = unwrap_alias(obj_type);
      if (underlying)
        it = type_methods_.find(underlying.get());
    }
    if (it == type_methods_.end()) {
      const Type *canonical = canonicalize_intrinsic(obj_type);
      if (canonical && canonical != obj_type.get())
        it = type_methods_.find(canonical);
    }
    return it == type_methods_.end() ? nullptr : &it->second;
  };

  if (auto *vec = find_user_methods())
    for (auto &m : *vec)
      if (m.name == field_name)
        return m.signature ? m.signature
                           : poison(span, "method '" + field_name +
                                              "' has no signature");

  auto effective_kind = underlying_kind(obj_type);
  auto effective_type = unwrap_alias(obj_type);

  auto kind_it = kind_methods_.find(effective_kind);
  if (kind_it != kind_methods_.end()) {
    for (auto &m : kind_it->second) {
      if (m.name != field_name)
        continue;
      if (!m.signature)
        return poison(span,
                      "kind method '" + field_name + "' has no signature");
      if (has_type_params(m.signature))
        return substitute_kind_method(effective_kind, effective_type,
                                      m.signature);
      return m.signature;
    }
  }

  for (auto &m : builtin_methods(effective_kind, builtins)) {
    if (m.name != field_name)
      continue;
    if (!m.signature)
      return poison(span,
                    "builtin method '" + field_name + "' has no signature");
    if (has_type_params(m.signature))
      return substitute_kind_method(effective_kind, effective_type,
                                    m.signature);
    return m.signature;
  }

  // Named-protocol fallback for TypeParam values inside generic bodies.
  // A method call on a value of TypeParam type is accepted if the called
  // method belongs to a compiler-known protocol (Hashable, Stringable);
  // satisfaction by the concrete type is verified at the monomorphisation
  // site.  Self-typed positions in the protocol signature are rewritten to
  // refer to the TypeParam.
  if (obj_type->kind == TypeKind::TypeParam) {
    auto subst_self = [](const TypePtr &sig, const TypePtr &iface,
                          const TypePtr &concrete) -> TypePtr {
      if (!sig || sig->kind != TypeKind::Func) return sig;
      auto &fi = std::get<FuncTypeInfo>(sig->detail);
      auto &iinfo = std::get<InterfaceTypeInfo>(iface->detail);
      auto subst_one = [&](const TypePtr &t) -> TypePtr {
        if (t && t->kind == TypeKind::Interface) {
          auto &ti = std::get<InterfaceTypeInfo>(t->detail);
          if (ti.name == iinfo.name &&
              ti.origin_package == iinfo.origin_package)
            return concrete;
        }
        return t;
      };
      std::vector<TypePtr> nparams;
      for (auto &p : fi.params)  nparams.push_back(subst_one(p));
      TypePtr nret = fi.return_type ? subst_one(fi.return_type) : nullptr;
      auto out = make_func_type(std::move(nparams), std::move(nret));
      std::get<FuncTypeInfo>(out->detail).is_variadic = fi.is_variadic;
      return out;
    };
    auto try_protocol = [&](const TypePtr &iface) -> TypePtr {
      if (!iface || iface->kind != TypeKind::Interface) return nullptr;
      auto &ii = std::get<InterfaceTypeInfo>(iface->detail);
      for (auto &m : ii.methods)
        if (m.name == field_name && m.signature)
          return subst_self(m.signature, iface, obj_type);
      return nullptr;
    };
    auto record_dispatch = [&]() {
      if (current_eager_kind_method_decl_)
        kind_method_uses_typeparam_dispatch_.insert(
            current_eager_kind_method_decl_);
    };
    auto &tpinfo = std::get<TypeParamInfo>(obj_type->detail);
    if (tpinfo.bound) {
      if (auto t = try_protocol(*tpinfo.bound)) {
        record_dispatch();
        return t;
      }
      return nullptr;
    }
    if (auto t = try_protocol(builtins.stringable_iface)) {
      record_dispatch();
      return t;
    }
    if (auto t = try_protocol(builtins.hashable_iface)) {
      record_dispatch();
      return t;
    }
  }

  if (auto u = unwrap_structural_alias(obj_type);
      u && u->kind == TypeKind::Union)
    return resolve_union_method(u, field_name);

  return nullptr;
}

TypePtr Analyzer::resolve_union_method(const TypePtr &union_type,
                                       const std::string &field_name) {
  auto &alts = std::get<UnionTypeInfo>(union_type->detail).alternatives;
  if (alts.empty())
    return nullptr;

  // Codegen dispatches per member through a struct- or scalar-receiver call;
  // other member kinds (array/map/alias/enum) have no receiver-call lowering
  // yet, so restrict eligibility to avoid accepting a call codegen can't emit.
  auto dispatchable = [](const TypePtr &t) {
    switch (t->kind) {
    case TypeKind::Struct:
    case TypeKind::Int:
    case TypeKind::Float:
    case TypeKind::Bool:
    case TypeKind::String:
      return true;
    default:
      return false;
    }
  };
  for (auto &alt : alts)
    if (!dispatchable(alt))
      return nullptr;

  auto self_free = [](const TypePtr &sig, const TypePtr &iface) -> bool {
    if (!sig || sig->kind != TypeKind::Func)
      return false;
    auto &fi = std::get<FuncTypeInfo>(sig->detail);
    auto &ii = std::get<InterfaceTypeInfo>(iface->detail);
    auto refs_self = [&](const TypePtr &t) {
      if (!t || t->kind != TypeKind::Interface)
        return false;
      auto &ti = std::get<InterfaceTypeInfo>(t->detail);
      return ti.name == ii.name && ti.origin_package == ii.origin_package;
    };
    for (auto &p : fi.params)
      if (refs_self(p))
        return false;
    return !(fi.return_type && refs_self(fi.return_type));
  };

  auto try_iface = [&](const TypePtr &iface) -> TypePtr {
    if (!iface || iface->kind != TypeKind::Interface)
      return nullptr;
    auto &ii = std::get<InterfaceTypeInfo>(iface->detail);
    const MethodInfo *m = nullptr;
    for (auto &im : ii.methods)
      if (im.name == field_name) {
        m = &im;
        break;
      }
    if (!m || !self_free(m->signature, iface))
      return nullptr;
    for (auto &alt : alts)
      if (!satisfies_interface(alt, iface))
        return nullptr;
    return m->signature;
  };

  if (auto s = try_iface(builtins.stringable_iface))
    return s;
  if (auto s = try_iface(builtins.hashable_iface))
    return s;
  if (package_scope_)
    for (auto &[name, sym] : package_scope_->symbols)
      if (sym.kind == SymbolKind::Type && sym.type &&
          sym.type->kind == TypeKind::Interface)
        if (auto s = try_iface(sym.type))
          return s;
  return nullptr;
}

TypePtr Analyzer::check_selector(const SelectorNode &node,
                                 const Node & /*parent*/) {
  auto obj_type = check_type_or_value_expr(*node.object);
  if (is_invalid_type(obj_type))
    return builtins.invalid_type;

  std::string field_name(node.field.name);

  // A type-name object accesses the type's own members, not an instance's.
  // For a struct that means its `fn Type.Fn()` type methods; a struct has no
  // instance fields or receiver methods reachable through the bare type name.
  if (auto *id = std::get_if<IdentifierNode>(&node.object->data)) {
    auto sym = lookup(std::string(id->name));
    if (sym && sym->kind == SymbolKind::Type &&
        obj_type->kind == TypeKind::Struct) {
      if (auto sig = lookup_struct_type_method(obj_type, field_name))
        return sig;
      error(node.field.span,
            std::format("type {} has no type method '{}'",
                        type_to_string(obj_type), field_name));
      return builtins.invalid_type;
    }
  }

  if (obj_type->kind == TypeKind::Module) {
    auto &mod = std::get<ModuleTypeInfo>(obj_type->detail);
    return resolve_module_selector(mod, field_name, node.field.span);
  }

  if (obj_type->kind == TypeKind::Struct)
    if (auto t = resolve_struct_member(obj_type, field_name, node.field.span))
      return t;

  if (obj_type->kind == TypeKind::Interface) {
    auto &info = std::get<InterfaceTypeInfo>(obj_type->detail);
    for (auto &m : info.methods)
      if (m.name == field_name)
        return m.signature ? m.signature
                           : poison(node.field.span,
                                    "interface method '" + field_name +
                                        "' has no signature");
  }

  if (obj_type->kind == TypeKind::Enum) {
    auto &info = std::get<EnumTypeInfo>(obj_type->detail);
    for (auto &v : info.variants)
      if (v.name == field_name)
        return obj_type;
    // `Enum.From(value)` reverse-lookup constructor: takes the backing value
    // (string for a string-backed enum, int otherwise) and returns the enum
    // or `Missing` when no variant matches.
    if (field_name == "From") {
      auto backing =
          info.string_backed ? builtins.string_type : builtins.int_type;
      return make_func_type(
          {backing}, {make_union_type({obj_type, builtins.error_base})});
    }
  }

  if (obj_type->kind == TypeKind::Alias) {
    auto &alias_info = std::get<AliasTypeInfo>(obj_type->detail);
    for (auto &m : alias_info.methods)
      if (m.name == field_name)
        return m.signature ? m.signature
                           : poison(node.field.span,
                                    "alias method '" + field_name +
                                        "' has no signature");
    auto underlying = unwrap_alias(obj_type);
    if (underlying && underlying->kind == TypeKind::Struct)
      if (auto t =
              resolve_struct_member(underlying, field_name, node.field.span))
        return t;
  }

  if (auto sig = resolve_method_signature(obj_type, field_name,
                                          node.field.span))
    return sig;

  error(node.field.span, std::format("type {} has no member '{}'",
                                     type_to_string(obj_type), field_name));
  return builtins.invalid_type;
}

} // namespace saga
