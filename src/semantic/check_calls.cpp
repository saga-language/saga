// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Checking a call: resolving which declaration it names, matching arguments
// against parameters, and reporting a mismatch against a callee the reader
// recognises rather than against the AST node that happened to hold it.

#include "semantic/analyzer.hpp"
#include "semantic/analyzer_detail.hpp"
#include <algorithm>
#include <format>

namespace saga {

namespace {

// The receiver's own method set, which is where the declaration recorded
// whether the body writes through it.
const std::vector<MethodInfo> *receiver_methods(const TypePtr &type) {
  if (!type) return nullptr;
  if (type->kind == TypeKind::Struct)
    return &std::get<StructTypeInfo>(type->detail).methods;
  if (type->kind == TypeKind::Alias)
    return &std::get<AliasTypeInfo>(type->detail).methods;
  return nullptr;
}

} // namespace

// An alias-bound method wins over the underlying type's, the order method
// dispatch uses. Array and Map keep theirs in `kind_method_decls_` rather than
// on the type, because the receiver is a type kind and not a declaration — and
// an alias has to be unwrapped to reach it.
bool Analyzer::method_mutates_receiver(const TypePtr &type,
                                       std::string_view name) const {
  if (!type) return false;
  if (const auto *methods = receiver_methods(type))
    for (auto &m : *methods)
      if (m.name == name) return m.mutates_receiver;

  auto km = kind_method_decls_.find(unwrap_alias(type)->kind);
  if (km == kind_method_decls_.end()) return false;
  auto m = km->second.find(std::string(name));
  return m != km->second.end() && m->second.mutates_receiver;
}

static std::string callee_display_name(const Node &callee) {
  if (auto *id = std::get_if<IdentifierNode>(&callee.data))
    return std::string(id->name);
  if (auto *sel = std::get_if<SelectorNode>(&callee.data))
    return std::string(sel->field.name);
  return "function";
}

// A method that writes through its receiver needs the receiver to be somewhere
// the write can land. A constant is not, and the call would otherwise compile
// into a write to a temporary that is discarded.
void Analyzer::reject_mutating_call_on_constant(const CallExprNode &node,
                                                const SelectorNode &sel,
                                                const TypePtr &recv) {
  auto *recv_id = std::get_if<IdentifierNode>(&sel.object->data);
  if (!recv_id)
    return;
  auto sym = lookup(std::string(recv_id->name));
  if (!sym || sym->kind != SymbolKind::Constant)
    return;
  if (!method_mutates_receiver(recv, sel.field.name))
    return;

  error(node.span,
        std::format("cannot call mutating method '{}' on constant '{}'",
                    sel.field.name, recv_id->name));
}

TypePtr Analyzer::check_call_expr(const CallExprNode &node,
                                  const Node &parent) {
  // Gate all intrinsic_* calls to stdlib packages only.
  if (auto *ident = std::get_if<IdentifierNode>(&node.callee->data)) {
    if (ident->name.starts_with("intrinsic_") && !is_stdlib) {
      error(node.callee->span,
            std::format("'{}' can only be called from stdlib packages",
                        ident->name));
      return builtins.invalid_type;
    }
  }

  auto callee_type = check_expr(*node.callee);
  if (is_invalid_type(callee_type))
    return builtins.invalid_type;

  if (!is_callable(callee_type)) {
    error(node.callee->span,
          std::format("'{}' is not callable", type_to_string(callee_type)));
    return builtins.invalid_type;
  }

  // A function-typed alias (`type Op = fn(...) ...`) is callable through its
  // underlying signature.
  callee_type = unwrap_alias(callee_type);

  // Check arguments first to collect their types.  A `.Variant` shorthand arg
  // resolves against the matching parameter's (concrete enum) type.
  const std::vector<TypePtr> *params =
      callee_type->kind == TypeKind::Func
          ? &std::get<FuncTypeInfo>(callee_type->detail).params
          : nullptr;
  std::vector<TypePtr> arg_types;
  for (size_t i = 0; i < node.args.size(); ++i) {
    TypePtr expected = (params && i < params->size()) ? (*params)[i] : nullptr;
    arg_types.push_back(bubble_into(
        *node.args[i], check_expr_expecting(*node.args[i], expected),
        expected));
  }

  // If the callee contains type parameters, attempt generic instantiation.
  auto effective_type = callee_type;
  if (has_type_params(callee_type)) {
    std::unordered_map<uint32_t, TypePtr> bindings;
    auto instantiated =
        instantiate_generic_call(callee_type, arg_types, node.span, &bindings);
    if (!is_invalid_type(instantiated))
      effective_type = instantiated;

    // For generic free functions, analyse the body with these concrete
    // bindings so member-access, operator-overloading and capture
    // tracking see concrete types.  Methods on generic types (Array/Map)
    // and receiver-method calls go through their own paths and aren't
    // registered in generic_templates_.
    if (!bindings.empty() && callee_type) {
      auto fd_it = func_decl_by_type_.find(callee_type.get());
      if (fd_it != func_decl_by_type_.end() &&
          !fd_it->second->is_extern &&
          generic_templates_.find(fd_it->second) != generic_templates_.end()) {
        // Substitution stops at a struct boundary, so a generic struct named
        // in the signature keeps its own type parameters however the call
        // binds them. Lowering that reads the value through the wrong layout,
        // so refuse the call rather than answer wrongly. Only a specialisable
        // function is checked: elsewhere a leftover parameter means a template
        // body being checked generically, where nothing is concrete yet.
        if (!is_invalid_type(effective_type) &&
            has_type_params(effective_type)) {
          error(node.span,
                std::format("cannot call '{}': its signature names a generic "
                            "struct that inference does not substitute",
                            callee_display_name(*node.callee)));
          return builtins.invalid_type;
        }
        instantiate_generic_body(*fd_it->second, bindings, parent);
        if (current_instantiation_) {
          current_instantiation_->node_type_args[&parent] = bindings;
        } else {
          node_type_args[&parent] = bindings;
        }
      }
    }
  } else if (auto *sel = std::get_if<SelectorNode>(&node.callee->data)) {
    auto obj_sem = node_types.count(sel->object.get())
                       ? node_types[sel->object.get()]
                       : nullptr;
    auto recv_shape = unwrap_alias(obj_sem);
    bool kind_recv = recv_shape && (recv_shape->kind == TypeKind::Array ||
                                    recv_shape->kind == TypeKind::Map);
    // Cross-package: the FuncDecl and dispatch flag are missing until we
    // lazily load std/array or std/map source, and both the mutation check
    // and the instantiation below read those tables.
    if (kind_recv && !is_stdlib &&
        kind_method_decls_.find(recv_shape->kind) == kind_method_decls_.end())
      ensure_source_loaded(recv_shape->kind == TypeKind::Array ? "array"
                                                              : "map");

    reject_mutating_call_on_constant(node, *sel, obj_sem);

    // kind_methods_ call (Array/Map receiver) where the substituted
    // signature is already concrete, but the body must be re-checked
    // with concrete K/V bindings because it dispatches through a named
    // protocol on a TypeParam value.  Drive instantiation per concrete
    // K so codegen can specialise.
    if (kind_recv) {
      auto km_it = kind_method_decls_.find(recv_shape->kind);
      if (km_it != kind_method_decls_.end()) {
        auto m_it = km_it->second.find(std::string(sel->field.name));
        if (m_it != km_it->second.end() &&
            kind_method_uses_typeparam_dispatch_.count(m_it->second.decl)) {
          std::unordered_map<uint32_t, TypePtr> bindings;
          auto &tps = m_it->second.type_params;
          if (obj_sem->kind == TypeKind::Array && !tps.empty()) {
            auto &arr = std::get<ArrayTypeInfo>(obj_sem->detail);
            bindings[tps[0].id] = arr.element;
          } else if (obj_sem->kind == TypeKind::Map && tps.size() >= 2) {
            auto &mp = std::get<MapTypeInfo>(obj_sem->detail);
            bindings[tps[0].id] = mp.key;
            bindings[tps[1].id] = mp.value;
          }
          if (!bindings.empty()) {
            instantiate_generic_body(*m_it->second.decl, bindings, parent);
            if (current_instantiation_)
              current_instantiation_->node_type_args[&parent] = bindings;
            else
              node_type_args[&parent] = bindings;
          }
        }
      }

      // Array.String() / Map.String() require their element/key/value types
      // to satisfy Stringable (recursively for nested aggregates).  Phase 5
      // will migrate these out of builtins; the named-protocol diagnostic
      // here is the use-site enforcement promised by Phase 3.
      if (sel->field.name == "String" && node.args.empty()) {
        if (obj_sem->kind == TypeKind::Array) {
          auto &arr = std::get<ArrayTypeInfo>(obj_sem->detail);
          check_stringable_recursive(arr.element, sel->field.span,
                                     "array element of .String() receiver");
        } else if (obj_sem->kind == TypeKind::Map) {
          auto &mp = std::get<MapTypeInfo>(obj_sem->detail);
          check_stringable_recursive(mp.key, sel->field.span,
                                     "map key of .String() receiver");
          check_stringable_recursive(mp.value, sel->field.span,
                                     "map value of .String() receiver");
        }
      }
    }
  }

  auto &fn_info = std::get<FuncTypeInfo>(effective_type->detail);

  // Check argument count.
  if (!fn_info.is_variadic) {
    if (arg_types.size() != fn_info.params.size()) {
      error(node.span, std::format("expected {} argument(s), got {}",
                                   fn_info.params.size(), arg_types.size()));
      return builtins.invalid_type;
    }
  } else {
    if (fn_info.params.size() > 0 &&
        arg_types.size() < fn_info.params.size() - 1) {
      error(node.span,
            std::format("expected at least {} argument(s), got {}",
                        fn_info.params.size() - 1, arg_types.size()));
      return builtins.invalid_type;
    }
  }

  // Spec: arrays of the same element type may be passed directly into
  // a variadic without spreading.  (docs/language.md:276-285)
  // Detection: exactly one argument in the variadic position whose type
  // matches the variadic's array type.
  bool variadic_array_passthrough =
      fn_info.is_variadic && !fn_info.params.empty() &&
      arg_types.size() == fn_info.params.size() &&
      fn_info.params.back()->kind == TypeKind::Array &&
      arg_types.back() && arg_types.back()->kind == TypeKind::Array &&
      types_equal(arg_types.back(), fn_info.params.back());

  // Check argument types against the (possibly instantiated) signature.
  for (size_t i = 0; i < arg_types.size(); ++i) {
    bool is_variadic_param = fn_info.is_variadic && !fn_info.params.empty() &&
                             i >= fn_info.params.size() - 1;
    if (is_variadic_param && variadic_array_passthrough) {
      expect_assignable(node.args[i]->span, fn_info.params.back(),
                        arg_types[i],
                        std::format("variadic argument {}", i + 1));
    } else if (is_variadic_param) {
      // Variadic args are checked against the element type of the
      // array-wrapped last parameter.
      auto &last = fn_info.params.back();
      if (last->kind == TypeKind::Array) {
        auto &arr = std::get<ArrayTypeInfo>(last->detail);
        expect_assignable(node.args[i]->span, arr.element, arg_types[i],
                          std::format("variadic argument {}", i + 1));
      }
    } else if (i < fn_info.params.size()) {
      expect_assignable(node.args[i]->span, fn_info.params[i], arg_types[i],
                        std::format("argument {}", i + 1));
    }
  }

  // Return type.
  return fn_info.return_type ? fn_info.return_type : builtins.void_type;
}

} // namespace saga
