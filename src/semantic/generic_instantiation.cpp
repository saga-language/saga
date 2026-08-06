// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Monomorphisation: binding a generic declaration's type parameters to
// concrete arguments and producing the specialised type. Substitution is
// memoised because a recursive generic would otherwise expand forever.

#include "semantic/analyzer.hpp"
#include <format>

namespace saga {

TypePtr
Analyzer::instantiate_generic_call(
    const TypePtr &callee_type, const std::vector<TypePtr> &arg_types,
    Span call_span, std::unordered_map<uint32_t, TypePtr> *out_bindings) {
  if (!callee_type || callee_type->kind != TypeKind::Func)
    return builtins.invalid_type;

  auto &fn_info = std::get<FuncTypeInfo>(callee_type->detail);
  std::unordered_map<uint32_t, TypePtr> bindings;

  // Attempt to unify each parameter type with the argument type.
  size_t count = std::min(fn_info.params.size(), arg_types.size());
  for (size_t i = 0; i < count; ++i) {
    if (!unify(fn_info.params[i], arg_types[i], bindings)) {
      error(call_span,
            std::format("cannot infer type parameter from argument {}", i + 1));
      return builtins.invalid_type;
    }
  }

  if (reject_void_bindings(bindings, call_span))
    return builtins.invalid_type;

  // Validate each binding against the type-parameter's constraint, if any.
  // Constraints are carried on the TypeParam nodes embedded in the function's
  // parameter/return types — walk the type tree to recover them.
  std::unordered_map<uint32_t, TypeConstraint> constraints;
  std::unordered_map<uint32_t, TypePtr> bounds;
  auto collect = [&](auto &self, const TypePtr &t) -> void {
    if (!t) return;
    if (t->kind == TypeKind::TypeParam) {
      auto &info = std::get<TypeParamInfo>(t->detail);
      if (info.param.constraint != TypeConstraint::None)
        constraints[info.param.id] = info.param.constraint;
      if (info.bound)
        bounds[info.param.id] = *info.bound;
      return;
    }
    switch (t->kind) {
    case TypeKind::Array:
      self(self, std::get<ArrayTypeInfo>(t->detail).element);
      break;
    case TypeKind::Map: {
      auto &m = std::get<MapTypeInfo>(t->detail);
      self(self, m.key);
      self(self, m.value);
      break;
    }
    case TypeKind::Func: {
      auto &f = std::get<FuncTypeInfo>(t->detail);
      for (auto &p : f.params) self(self, p);
      if (f.return_type) self(self, f.return_type);
      break;
    }
    case TypeKind::Union:
      for (auto &a : std::get<UnionTypeInfo>(t->detail).alternatives)
        self(self, a);
      break;
    default:
      break;
    }
  };
  for (auto &p : fn_info.params) collect(collect, p);
  if (fn_info.return_type) collect(collect, fn_info.return_type);

  bool constraint_violation = false;
  for (auto &[id, concrete] : bindings) {
    auto it = constraints.find(id);
    if (it == constraints.end()) continue;
    if (!satisfies_constraint(concrete, it->second)) {
      error(call_span,
            std::format("type {} does not satisfy constraint {}",
                        type_to_string(concrete),
                        constraint_name(it->second)));
      constraint_violation = true;
    }
  }
  for (auto &[id, concrete] : bindings) {
    auto it = bounds.find(id);
    if (it == bounds.end()) continue;
    if (!concrete || is_invalid_type(concrete) ||
        concrete->kind == TypeKind::TypeParam)
      continue;
    auto &ii = std::get<InterfaceTypeInfo>(it->second->detail);
    if (!report_interface_unsatisfied(concrete, it->second, ii.name, call_span,
                                      ""))
      constraint_violation = true;
  }
  if (constraint_violation)
    return poison(call_span, "constraint violation left unreported");

  if (out_bindings)
    *out_bindings = bindings;

  if (bindings.empty())
    return callee_type; // No type params to substitute.

  return substitute(callee_type, bindings);
}

Analyzer::BodyInstantiation *Analyzer::instantiate_generic_body(
    const FuncDeclNode &fn,
    const std::unordered_map<uint32_t, TypePtr> &bindings,
    const Node &call_node) {
  auto tpl_it = generic_templates_.find(&fn);
  if (tpl_it == generic_templates_.end())
    return nullptr; // not a generic free function we know about

  auto &tpl = tpl_it->second;

  // Reuse a cached instantiation with matching bindings.  A matching entry
  // that is still in_progress is the recursion guard for mutually recursive
  // generics: we hand back the partial entry without re-analysing.
  auto &list = instantiations_[&fn];
  auto same_bindings = [&](const std::unordered_map<uint32_t, TypePtr> &a,
                           const std::unordered_map<uint32_t, TypePtr> &b) {
    if (a.size() != b.size())
      return false;
    for (auto &[id, t] : a) {
      auto it = b.find(id);
      if (it == b.end())
        return false;
      if (!types_equal(t, it->second))
        return false;
    }
    return true;
  };
  for (auto &inst : list) {
    if (same_bindings(inst.bindings, bindings))
      return &inst;
  }

  // Fresh entry.  Insert with in_progress = true so a re-entrant call from
  // the body analysis (same decl, same bindings) finds the partial entry
  // and breaks the cycle.
  list.push_back(BodyInstantiation{});
  BodyInstantiation &inst = list.back();
  inst.decl = &fn;
  inst.bindings = bindings;
  inst.in_progress = true;

  // Swap to a fresh child of the declaration's lexical scope.  Names in
  // the body resolve against where the generic was declared, not the
  // caller's scope.
  auto saved_scope = current_scope;
  BodyInstantiation *saved_inst = current_instantiation_;
  bool saved_is_stdlib = is_stdlib;
  bool saved_suppress = suppress_unread_reports_;
  // Which locals a body reads is the same answer for every instantiation, so
  // only the first one draws it.
  suppress_unread_reports_ = list.size() > 1;
  instantiation_stack_.push_back(&call_node);

  current_scope = tpl.decl_scope->child(ScopeKind::Block);
  // Stdlib bodies (e.g. Map.String() in std/map) re-analysed at a user-
  // package call site keep their stdlib provenance so intrinsic_* calls
  // inside the body remain allowed.
  if (tpl.is_stdlib)
    is_stdlib = true;

  // Register each generic type parameter under its original name in the
  // new scope so a reference to `T` inside the body resolves to the
  // concrete binding.  Both the named symbol lookup and the type_bindings
  // table (used by substitute()) are populated.
  for (auto &tp : tpl.type_params) {
    auto bind_it = bindings.find(tp.id);
    if (bind_it == bindings.end())
      continue;
    auto &concrete = bind_it->second;
    current_scope->symbols.emplace(
        tp.name, Symbol::type_param(tp.name, concrete, Span{}));
    current_scope->type_bindings[tp.id] = concrete;
  }

  // Push the function scope that will hold parameters and return types.
  push_scope(ScopeKind::Function);

  // Inject receiver parameter for generic methods on concrete types.
  if (fn.receiver) {
    auto recv_type = resolve_type(*fn.receiver->type);
    declare_local(Symbol::parameter(std::string(fn.receiver->name.name),
                                    recv_type, fn.receiver->name.span));
  }

  // Substituted return type for return-stmt checking.
  if (fn.signature.return_type) {
    auto rt = resolve_type(*fn.signature.return_type);
    current_scope->return_types.push_back(substitute(rt, bindings));
  }

  // Inject parameters with substituted (concrete) types.  Without this
  // step, identifiers referring to parameters inside the body would
  // resolve to their original TypeParam-typed symbols and the body would
  // type-check against `T` rather than the concrete type.
  for (auto &p : fn.signature.params) {
    auto pt = resolve_type(*p.type);
    pt = substitute(pt, bindings);
    for (auto &ident : p.names.identifiers) {
      auto param_type = p.is_variadic ? make_array_type(pt) : pt;
      declare_local(Symbol::parameter(std::string(ident.name),
                                      std::move(param_type), ident.span));
    }
  }

  // Route side-table writes into this instantiation's view.
  current_instantiation_ = &inst;

  // Phase 3 — name resolution over the body.
  auto &block = std::get<BlockNode>(fn.body->data);
  resolve_block(block);

  // Phase 4 — type checking over the body.
  auto body_type = check_block(block);

  // Tail-expression return compatibility (mirrors check_func_decl_body).
  if (!current_scope->return_types.empty() && !block.stmts.empty()) {
    bool tail_is_return = always_returns(*block.stmts.back());
    if (!tail_is_return) {
      auto &expected = current_scope->return_types;
      if (expected.size() == 1 && !is_invalid_type(body_type)) {
        if (!types_equal(expected[0], builtins.void_type)) {
          expect_assignable(fn.body->span, expected[0], body_type,
                            "return type");
        }
      }
    }
  }

  // Restore everything.
  report_unread_locals(*current_scope);
  pop_scope();           // the Function scope
  current_scope = saved_scope;
  current_instantiation_ = saved_inst;
  is_stdlib = saved_is_stdlib;
  suppress_unread_reports_ = saved_suppress;
  instantiation_stack_.pop_back();

  inst.in_progress = false;
  return &inst;
}

TypePtr Analyzer::instantiate_generic_struct(
    const TypePtr &struct_type,
    const std::vector<std::pair<std::string, TypePtr>> &field_types,
    Span span) {
  if (!struct_type || struct_type->kind != TypeKind::Struct)
    return poison(span, "generic instantiation of a non-struct type");

  auto &info = std::get<StructTypeInfo>(struct_type->detail);
  if (info.type_params.empty())
    return struct_type; // Not generic.

  std::unordered_map<uint32_t, TypePtr> bindings;

  // Unify each provided field value type against the struct's field type. A
  // field may bind nothing — `tail Node<T> | Missing` given `Missing{}` says
  // nothing about T — which is silent here; an unbound parameter is caught
  // below by the field that was supposed to bind it.
  for (auto &[fname, ftype] : field_types) {
    for (auto &fi : info.fields) {
      if (fi.name == fname && fi.type) {
        unify(fi.type, ftype, bindings);
        break;
      }
    }
  }

  if (reject_void_bindings(bindings, span))
    return builtins.invalid_type;

  if (bindings.empty())
    return struct_type;

  // Publish the shell before substituting fields, and map the declaration onto
  // it, so a field naming the struct again lands on this instantiation.
  auto result = make_struct_type(info.name, {}, {}, info.type_params,
                                 info.origin_package);
  auto &result_info = std::get<StructTypeInfo>(result->detail);
  SubstMemo memo{{struct_type.get(), result}};

  for (auto &f : info.fields)
    result_info.fields.push_back(
        {f.name, substitute(f.type, bindings, memo), f.is_public,
         f.default_value});
  for (auto &m : info.methods)
    result_info.methods.push_back(
        {m.name, substitute(m.signature, bindings, memo), m.is_public,
         m.origin_package});
  // Record the concrete type arguments.
  for (auto &tp : info.type_params) {
    auto it = bindings.find(tp.id);
    if (it != bindings.end())
      result_info.type_args.push_back(it->second);
  }
  result_info.embeds = info.embeds;

  return result;
}

} // namespace saga
