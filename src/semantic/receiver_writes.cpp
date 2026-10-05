// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// What a body writes through the bindings it does not own: a method through
// its receiver, a closure through its captures. Assigning through one is read
// off the declaration (`writes_through_binding`); calling a method that writes
// its receiver on one is noted here while the body is checked, and settled
// once every body has been, since the method called may be checked later.

#include "semantic/analyzer.hpp"

#include <algorithm>
#include <format>

namespace saga {

namespace {
std::vector<MethodInfo> *declared_methods(const TypePtr &type) {
  if (!type) return nullptr;
  if (type->kind == TypeKind::Struct)
    return &std::get<StructTypeInfo>(type->detail).methods;
  if (type->kind == TypeKind::Alias)
    return &std::get<AliasTypeInfo>(type->detail).methods;
  return nullptr;
}
} // namespace

void Analyzer::push_method_frame(const FuncDeclNode &fn,
                                 const TypePtr &recv_type) {
  writer_frames_.push_back({recv_type,
                            std::string(fn.name.name),
                            nullptr,
                            {std::string(fn.receiver->name.name)}});
}

void Analyzer::push_closure_frame(const FuncExprNode &node,
                                  const Node &parent) {
  WriterFrame frame{nullptr, {}, &parent, {}};
  auto &caps = current_instantiation_
                   ? current_instantiation_->node_captures[&parent]
                   : node_captures[&parent];
  for (auto &cap : caps) {
    frame.bindings.push_back(cap.name);
    if (writes_through_binding(*node.body, cap.name))
      closures_writing_captures_.insert(&parent);
  }
  writer_frames_.push_back(std::move(frame));
}

void Analyzer::pop_writer_frame() { writer_frames_.pop_back(); }

// Only the innermost frame counts: a closure inside a method writes its own
// copy of the receiver, not the receiver.
void Analyzer::note_receiver_call(const SelectorNode &sel,
                                  const TypePtr &recv) {
  if (writer_frames_.empty())
    return;
  auto &frame = writer_frames_.back();
  auto root = binding_root(*sel.object);
  if (root.empty() || std::find(frame.bindings.begin(), frame.bindings.end(),
                                root) == frame.bindings.end())
    return;
  writer_calls_.push_back({frame.recv_type, frame.method, frame.closure, recv,
                           std::string(sel.field.name)});
}

bool Analyzer::mark_method_writes(const TypePtr &recv_type,
                                  const std::string &name) {
  auto *methods = declared_methods(recv_type);
  if (!methods)
    return false;
  for (auto &m : *methods)
    if (m.name == name && !m.mutates_receiver)
      return m.mutates_receiver = true;
  return false;
}

void Analyzer::settle_receiver_writes() {
  for (bool changed = true; changed;) {
    changed = false;
    for (auto &c : writer_calls_)
      if (c.caller_recv && method_mutates_receiver(c.recv, c.method))
        changed |= mark_method_writes(c.caller_recv, c.caller);
  }
  for (auto &c : writer_calls_)
    if (c.closure && method_mutates_receiver(c.recv, c.method))
      closures_writing_captures_.insert(c.closure);
  for (auto &d : deferred_const_calls_)
    if (method_mutates_receiver(d.recv, d.method))
      error(d.span, std::format("cannot call mutating method '{}' on "
                                "constant '{}'",
                                d.method, d.constant));
  writer_calls_.clear();
  deferred_const_calls_.clear();
}

} // namespace saga
