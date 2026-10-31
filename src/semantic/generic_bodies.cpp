// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// A generic body is checked once at its declaration, each type parameter
// abstract and held to its bound; that is where its errors are reported.
// Every instantiation checks it again under concrete bindings, but only to
// compute the concrete types codegen reads.

#include "semantic/analyzer.hpp"

namespace saga {

namespace {

using Bindings = std::unordered_map<uint32_t, TypePtr>;

bool binds_abstract(const Bindings &bindings) {
  for (auto &[id, t] : bindings)
    if (has_type_params(t))
      return true;
  return false;
}

bool same_bindings(const Bindings &a, const Bindings &b) {
  if (a.size() != b.size())
    return false;
  for (auto &[id, t] : a) {
    auto it = b.find(id);
    if (it == b.end() || !types_equal(t, it->second))
      return false;
  }
  return true;
}

} // namespace

// Names in the body resolve against where the generic was declared, not where
// it is used, and its side-table writes go to `inst`. `instantiating` is set
// for every check but the declaration's.
class Analyzer::TemplateBodyScope {
public:
  TemplateBodyScope(Analyzer &a, const GenericTemplate &tpl,
                    BodyInstantiation &inst, bool instantiating)
      : a_(a), scope_(a.current_scope), inst_(a.current_instantiation_),
        is_stdlib_(a.is_stdlib),
        instantiating_(a.instantiating_checked_body_) {
    a.current_scope = tpl.decl_scope->child(ScopeKind::Block);
    a.current_instantiation_ = &inst;
    a.is_stdlib = a.is_stdlib || tpl.is_stdlib;
    a.instantiating_checked_body_ = instantiating;
  }
  ~TemplateBodyScope() {
    a_.current_scope = scope_;
    a_.current_instantiation_ = inst_;
    a_.is_stdlib = is_stdlib_;
    a_.instantiating_checked_body_ = instantiating_;
  }
  TemplateBodyScope(const TemplateBodyScope &) = delete;
  TemplateBodyScope &operator=(const TemplateBodyScope &) = delete;

private:
  Analyzer &a_;
  Scope::Ptr scope_;
  BodyInstantiation *inst_;
  bool is_stdlib_;
  bool instantiating_;
};

// A template that is still being checked answers yes, so a body that reaches
// itself through a concrete call does not wait on its own verdict.
bool Analyzer::check_generic_template(GenericTemplate &tpl) {
  if (tpl.body_check != GenericTemplate::BodyCheck::Unchecked)
    return tpl.body_check != GenericTemplate::BodyCheck::Failed;
  tpl.body_check = GenericTemplate::BodyCheck::Checking;
  BodyInstantiation scratch;
  scratch.decl = tpl.decl;
  {
    TemplateBodyScope guard(*this, tpl, scratch, false);
    enter_generics(*tpl.decl->generic);
    check_template_body(*tpl.decl);
  }
  tpl.body_check = scratch.failed ? GenericTemplate::BodyCheck::Failed
                                  : GenericTemplate::BodyCheck::Passed;
  return !scratch.failed;
}

// The body may be checked from inside a closure, whose captures it does not
// see, so it gets a writer frame of its own.
void Analyzer::check_template_body(const FuncDeclNode &fn) {
  push_scope(ScopeKind::Function);
  declare_signature(fn);
  auto &block = std::get<BlockNode>(fn.body->data);
  resolve_block(block);
  push_body_frame();
  check_function_block(fn, block);
  pop_writer_frame();
  report_unread_locals(*current_scope);
  pop_scope();
}

void Analyzer::bind_type_params(const std::vector<TypeParam> &params,
                                const Bindings &bindings) {
  for (auto &tp : params) {
    auto it = bindings.find(tp.id);
    if (it == bindings.end())
      continue;
    current_scope->symbols.emplace(
        tp.name, Symbol::type_param(tp.name, it->second, Span{}));
    current_scope->type_bindings[tp.id] = it->second;
  }
}

Analyzer::BodyInstantiation *
Analyzer::cached_instantiation(const FuncDeclNode &fn,
                               const Bindings &bindings) {
  for (auto &inst : instantiations_[&fn])
    if (same_bindings(inst.bindings, bindings))
      return &inst;
  return nullptr;
}

// Bindings that are themselves abstract come from a generic body checked at
// its declaration, which instantiates nothing: its own instantiations will.
// A cached entry still in progress is the recursion guard for mutually
// recursive generics.
Analyzer::BodyInstantiation *
Analyzer::instantiate_generic_body(const FuncDeclNode &fn,
                                   const Bindings &bindings,
                                   const Node &call_node) {
  auto tpl_it = generic_templates_.find(&fn);
  if (tpl_it == generic_templates_.end() || binds_abstract(bindings) ||
      !check_generic_template(tpl_it->second))
    return nullptr;
  if (auto *cached = cached_instantiation(fn, bindings))
    return cached;

  auto &inst = instantiations_[&fn].emplace_back();
  inst.decl = &fn;
  inst.bindings = bindings;
  inst.in_progress = true;
  instantiation_stack_.push_back(&call_node);
  {
    TemplateBodyScope guard(*this, tpl_it->second, inst, true);
    bind_type_params(tpl_it->second.type_params, bindings);
    check_template_body(fn);
  }
  instantiation_stack_.pop_back();
  inst.in_progress = false;
  return &inst;
}

} // namespace saga
