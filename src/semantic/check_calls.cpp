// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Checking a call: resolving which declaration it names, matching arguments
// against parameters, and reporting a mismatch against a callee the reader
// recognises rather than against the AST node that happened to hold it.

#include "semantic/analyzer.hpp"
#include <algorithm>
#include <format>
#include <unordered_set>

namespace saga {

namespace {

// Methods whose body passes the receiver as the first argument to one of
// these C runtime functions mutate the caller's collection in place, so they
// cannot be applied to an immutable constant.  Value-returning copy-on-write
// methods (Append/Insert/Set) are absent: they never write through the
// receiver, so they are valid on a const (the result is a fresh array).
const std::unordered_set<std::string> kMutatingIntrinsics{
    "saga_array_pop", "saga_map_set", "saga_map_remove"};

bool is_kind_method_mutating(const FuncDeclNode &fn) {
  if (!fn.body || !fn.receiver) return false;
  auto *blk = std::get_if<BlockNode>(&fn.body->data);
  if (!blk) return false;
  std::string_view recv = fn.receiver->name.name;

  for (auto &stmt : blk->stmts) {
    auto *call = std::get_if<CallExprNode>(&stmt->data);
    if (!call) continue;
    auto *id = std::get_if<IdentifierNode>(&call->callee->data);
    if (!id) continue;
    if (!kMutatingIntrinsics.count(std::string(id->name))) continue;
    if (call->args.empty()) continue;
    auto *recv_id = std::get_if<IdentifierNode>(&call->args[0]->data);
    if (recv_id && recv_id->name == recv) return true;
  }
  return false;
}

} // namespace

static std::string callee_display_name(const Node &callee) {
  if (auto *id = std::get_if<IdentifierNode>(&callee.data))
    return std::string(id->name);
  if (auto *sel = std::get_if<SelectorNode>(&callee.data))
    return std::string(sel->field.name);
  return "function";
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
    arg_types.push_back(check_expr_expecting(*node.args[i], expected));
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
    // kind_methods_ call (Array/Map receiver) where the substituted
    // signature is already concrete, but the body must be re-checked
    // with concrete K/V bindings because it dispatches through a named
    // protocol on a TypeParam value.  Drive instantiation per concrete
    // K so codegen can specialise.
    auto obj_sem = node_types.count(sel->object.get())
                       ? node_types[sel->object.get()]
                       : nullptr;
    if (obj_sem && (obj_sem->kind == TypeKind::Array ||
                    obj_sem->kind == TypeKind::Map)) {
      // Cross-package: the FuncDecl and dispatch flag are missing until
      // we lazily load std/array or std/map source.  Trigger that here
      // so kind_method_decls_ / kind_method_uses_typeparam_dispatch_
      // are populated before we look them up.
      if (!is_stdlib && kind_method_decls_.find(obj_sem->kind) ==
                            kind_method_decls_.end()) {
        const char *origin = obj_sem->kind == TypeKind::Array
                                  ? "array" : "map";
        ensure_source_loaded(origin);
      }
      auto km_it = kind_method_decls_.find(obj_sem->kind);
      if (km_it != kind_method_decls_.end()) {
        auto m_it = km_it->second.find(std::string(sel->field.name));
        if (m_it != km_it->second.end() &&
            is_kind_method_mutating(*m_it->second.decl)) {
          if (auto *recv_id =
                  std::get_if<IdentifierNode>(&sel->object->data)) {
            auto sym = lookup(std::string(recv_id->name));
            if (sym && sym->kind == SymbolKind::Constant) {
              error(node.span,
                    std::format("cannot call mutating method '{}' on "
                                "constant '{}'",
                                sel->field.name, recv_id->name));
            }
          }
        }
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
