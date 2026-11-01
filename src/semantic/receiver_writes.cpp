// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// What a body writes through the bindings it does not own: a method through
// its receiver, a closure through its captures. Assigning through one is read
// off the declaration (`writes_through_binding`). So is a method's calling a
// method that writes on its receiver, typed by the fields the receiver
// declares, because a generic method's body is checked only per instantiation;
// a closure's calls are noted while its body is checked. Either is settled
// once every body has been, since the method called may be declared later.

#include "semantic/analyzer.hpp"

#include <algorithm>
#include <format>

namespace saga {

namespace {

// A struct's own method wins over a promoted one, and an alias's over its
// underlying type's: the order dispatch finds them in.
std::optional<bool> declared_writes(const TypePtr &type,
                                    std::string_view name) {
  if (auto *methods = declared_methods(type))
    for (auto &m : *methods)
      if (m.name == name) return m.mutates_receiver;
  if (type && type->kind == TypeKind::Struct)
    for (auto &embed : std::get<StructTypeInfo>(type->detail).embeds)
      if (auto writes = declared_writes(embed, name)) return writes;
  if (type && type->kind == TypeKind::Alias)
    return declared_writes(std::get<AliasTypeInfo>(type->detail).underlying,
                           name);
  return std::nullopt;
}

TypePtr element_of(const TypePtr &collection, const IndexExprNode &index) {
  if (!collection) return nullptr;
  if (std::holds_alternative<SliceNode>(index.index->data))
    return collection;
  if (collection->kind == TypeKind::Array)
    return std::get<ArrayTypeInfo>(collection->detail).element;
  if (collection->kind == TypeKind::Map)
    return std::get<MapTypeInfo>(collection->detail).value;
  return nullptr;
}

} // namespace

// Array and Map keep their methods in `kind_method_decls_` rather than on the
// type, because the receiver is a type kind and not a declaration.
bool Analyzer::method_mutates_receiver(const TypePtr &type,
                                       std::string_view name) const {
  if (!type) return false;
  if (auto writes = declared_writes(type, name)) return *writes;
  auto km = kind_method_decls_.find(unwrap_alias(type)->kind);
  if (km == kind_method_decls_.end()) return false;
  auto m = km->second.find(std::string(name));
  return m != km->second.end() && m->second.mutates_receiver;
}

void Analyzer::attach_receiver_method(const TypePtr &owner,
                                      const FuncDeclNode &fn,
                                      const TypePtr &signature,
                                      std::vector<TypePtr> receiver_params) {
  auto recv = fn.receiver->name.name;
  declared_methods(owner)->push_back(
      {std::string(fn.name.name), signature, fn.is_public,
       current_package_name(),
       fn.body && writes_through_binding(*fn.body, recv),
       std::move(receiver_params)});
  if (fn.body)
    receiver_methods_.push_back({owner, &fn});
}

void Analyzer::push_closure_frame(const FuncExprNode &node,
                                  const Node &parent) {
  WriterFrame frame{&parent, {}};
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

void Analyzer::push_body_frame() { writer_frames_.push_back({}); }

void Analyzer::pop_writer_frame() { writer_frames_.pop_back(); }

// Only the innermost frame counts: a closure writes its own copy of what it
// captures, not the binding outside.
void Analyzer::note_receiver_call(const SelectorNode &sel,
                                  const TypePtr &recv) {
  if (writer_frames_.empty())
    return;
  auto &frame = writer_frames_.back();
  auto root = binding_root(*sel.object);
  if (root.empty() || std::find(frame.bindings.begin(), frame.bindings.end(),
                                root) == frame.bindings.end())
    return;
  writer_calls_.push_back(
      {nullptr, {}, frame.closure, recv, std::string(sel.field.name)});
}

void Analyzer::note_method_calls() {
  for (auto &[owner, fn] : receiver_methods_)
    for (auto *sel : calls_through_binding(*fn->body, fn->receiver->name.name))
      writer_calls_.push_back({owner, std::string(fn->name.name), nullptr,
                               declared_path_type(*sel->object, owner),
                               std::string(sel->field.name)});
  receiver_methods_.clear();
}

// `path` is rooted at the receiver, so its root names `recv` itself.
TypePtr Analyzer::declared_path_type(const Node &path, const TypePtr &recv) {
  if (auto *sel = std::get_if<SelectorNode>(&path.data)) {
    auto owner = unwrap_alias(declared_path_type(*sel->object, recv));
    if (!owner || owner->kind != TypeKind::Struct) return nullptr;
    return resolve_struct_member(owner, std::string(sel->field.name),
                                 sel->field.span);
  }
  if (auto *idx = std::get_if<IndexExprNode>(&path.data))
    return element_of(unwrap_alias(declared_path_type(*idx->object, recv)),
                      *idx);
  return recv;
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
  note_method_calls();
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
