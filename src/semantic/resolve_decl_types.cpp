// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Giving each top-level declaration its type once every name is known: struct
// fields, method signatures, enum backing, alias targets. Runs after collection
// so a declaration may refer to one that appears later in the file.

#include "semantic/analyzer.hpp"
#include "semantic/analyzer_detail.hpp"
#include <algorithm>
#include <format>

namespace saga {

// `void` is the absence of a value. It can be returned and it can be a union
// alternative — that is what `T | void` means — but a value of it does not
// exist, so nothing can hold one.
static bool holds_void(const TypePtr &t) {
  auto u = unwrap_alias(t);
  if (!u)
    return false;
  switch (u->kind) {
  case TypeKind::Void:
    return true;
  case TypeKind::Array:
    return holds_void(std::get<ArrayTypeInfo>(u->detail).element);
  case TypeKind::Map: {
    auto &info = std::get<MapTypeInfo>(u->detail);
    return holds_void(info.key) || holds_void(info.value);
  }
  default:
    return false;
  }
}

bool Analyzer::reject_void_value(Span span, const TypePtr &type,
                                 std::string_view what) {
  if (!holds_void(type))
    return false;
  error(span, std::format("{} cannot be typed '{}': void is the absence of a "
                          "value, so there is nothing to hold",
                          what, type_to_string(type)));
  return true;
}

bool Analyzer::reject_void_bindings(
    const std::unordered_map<uint32_t, TypePtr> &bindings, Span span) {
  // `bindings` is unordered, so walk the ids in order or the diagnostics come
  // out differently from one run to the next.
  std::vector<uint32_t> ids;
  for (auto &[id, concrete] : bindings)
    ids.push_back(id);
  std::sort(ids.begin(), ids.end());

  bool rejected = false;
  for (uint32_t id : ids)
    rejected |=
        reject_void_value(span, bindings.at(id), "a type argument");
  return rejected;
}

TypePtr Analyzer::resolve_signature(const SignatureNode &sig) {
  std::vector<TypePtr> params;
  for (auto &p : sig.params) {
    auto pt = resolve_type(*p.type);
    reject_void_value(p.type->span, pt, "a parameter");
    // Each name in the identifier list maps to one parameter of that type.
    // An empty identifier list means an unnamed parameter (Go-style interface
    // method) — push the type once.
    if (p.names.identifiers.empty()) {
      params.push_back(p.is_variadic ? make_array_type(pt) : pt);
      continue;
    }
    for (size_t i = 0; i < p.names.identifiers.size(); ++i) {
      if (p.is_variadic && i == p.names.identifiers.size() - 1) {
        // Variadic wraps the type in an array.
        params.push_back(make_array_type(pt));
      } else {
        params.push_back(pt);
      }
    }
  }
  TypePtr ret = sig.return_type ? resolve_type(*sig.return_type) : nullptr;
  // Record the resolved return type so codegen can recover it without
  // re-resolving the AST against a scope where package-local names (a user
  // error `E` in `int | E`) aren't reachable.
  if (sig.return_type && ret)
    record_type(*sig.return_type, ret);
  return make_func_type(std::move(params), std::move(ret));
}

void Analyzer::declare_parameters(const SignatureNode &sig) {
  for (auto &p : sig.params) {
    auto pt = resolve_type(*p.type);
    for (auto &ident : p.names.identifiers) {
      auto param_type = p.is_variadic ? make_array_type(pt) : pt;
      declare_local(Symbol::parameter(std::string(ident.name),
                                      std::move(param_type), ident.span));
    }
  }
}

std::optional<std::string> type_decl_name(const Node &node) {
  using R = std::optional<std::string>;
  return std::visit(
      overloaded{
          [](const StructDeclNode &s) -> R { return std::string(s.name.name); },
          [](const EnumDeclNode &e) -> R { return std::string(e.name.name); },
          [](const ErrorDeclNode &e) -> R { return std::string(e.name.name); },
          [](const InterfaceDeclNode &i) -> R {
            return std::string(i.name.name);
          },
          [](const TypeDeclNode &t) -> R { return std::string(t.name.name); },
          [](const auto &) -> R { return std::nullopt; },
      },
      node.data);
}

void Analyzer::ensure_type_resolved(const std::string &name) {
  auto it = pending_type_decls_.find(name);
  if (it == pending_type_decls_.end())
    return;
  const Node *decl = it->second;
  // Drop the entry before resolving: a type that names itself, or a peer that
  // names it back, re-enters here. Each resolver publishes its type into the
  // symbol before descending into fields, so the cycle sees a real type.
  pending_type_decls_.erase(it);
  AtPackageScope at_package(*this);
  std::visit(overloaded{
                 [&](const StructDeclNode &s) { resolve_struct_decl(s); },
                 [&](const EnumDeclNode &e) { resolve_enum_decl(e); },
                 [&](const ErrorDeclNode &e) { resolve_error_decl(e); },
                 [&](const InterfaceDeclNode &i) { resolve_interface_decl(i); },
                 [&](const TypeDeclNode &t) { resolve_type_decl(t); },
                 [&](const auto &) {},
             },
             decl->data);
}

void Analyzer::resolve_declaration(const Node &node) {
  if (auto name = type_decl_name(node))
    return ensure_type_resolved(*name);
  std::visit(overloaded{
                 [&](const FuncDeclNode &fn) { resolve_func_decl(fn); },
                 [&](const ConstDeclNode &c) { resolve_const_decl(c); },
                 [&](const ImportDeclNode &) { /* processed in phase 1.5 */ },
                 [&](const auto &) { /* already reported in collect */ },
             },
             node.data);
}

void Analyzer::resolve_func_decl(const FuncDeclNode &fn) {
  // If the function is generic, push a temporary scope to hold type params
  // so the signature can reference them.  The scope must also be active
  // while resolving the receiver type, so we defer pop_scope() until after
  // the receiver binding below.
  bool has_generics = fn.generic.has_value();
  std::vector<TypeParam> generic_params;
  if (has_generics) {
    push_scope(ScopeKind::Block);
    generic_params = enter_generics(*fn.generic);
  }

  auto fn_type = resolve_signature(fn.signature);

  // Mark the function type as variadic if the last param is.
  if (!fn.signature.params.empty() && fn.signature.params.back().is_variadic) {
    auto &fi = std::get<FuncTypeInfo>(fn_type->detail);
    fi.is_variadic = true;
  }

  // If this is a receiver method, attach it to the receiver type.
  // NOTE: generics scope (if any) is still active here so that non-identifier
  // receiver types like [T] can resolve T as the function's type parameter.
  if (fn.receiver) {
    auto &recv_type_node = fn.receiver->type;
    // Helper: check D4 — cannot bind a receiver to a type from another package.
    auto check_recv_origin = [&](const std::string &origin,
                                 const std::string &type_name) -> bool {
      if (!origin.empty() && origin != current_package_name()) {
        error(fn.receiver->name.span,
              std::format("cannot bind receiver method to type '{}' from "
                          "another package",
                          type_name));
        return false;
      }
      return true;
    };

    if (auto *gapp =
            std::get_if<GenericTypeAppNode>(&recv_type_node->data)) {
      // Generic type application receiver: pub fn |T| (b |T| Box) Method(...)
      // Attach the method to the base struct type (ignoring the type args).
      if (auto *base_ident =
              std::get_if<IdentifierNode>(&gapp->base_type->data)) {
        auto recv_sym = lookup(std::string(base_ident->name));
        if (recv_sym && recv_sym->type &&
            recv_sym->type->kind == TypeKind::Struct) {
          auto &struct_info =
              std::get<StructTypeInfo>(recv_sym->type->detail);
          if (check_recv_origin(struct_info.origin_package,
                                struct_info.name)) {
            // Remap the function's TypeParam IDs to the struct's TypeParam IDs
            // so that instantiate_generic_struct can substitute them correctly.
            // e.g. fn |T#5| (b |T#5| Box) Get() T#5  →  Get() T#0 (struct's T)
            TypePtr stored_sig = fn_type;
            std::unordered_map<uint32_t, uint32_t> id_remap;
            if (!struct_info.type_params.empty() &&
                gapp->type_args.size() == struct_info.type_params.size()) {
              std::unordered_map<uint32_t, TypePtr> remap;
              for (size_t i = 0; i < gapp->type_args.size(); ++i) {
                auto arg_t = resolve_type(*gapp->type_args[i]);
                if (arg_t && arg_t->kind == TypeKind::TypeParam) {
                  auto &tp = std::get<TypeParamInfo>(arg_t->detail);
                  remap[tp.param.id] =
                      make_type_param(struct_info.type_params[i].id,
                                      struct_info.type_params[i].name);
                  id_remap[tp.param.id] = struct_info.type_params[i].id;
                }
              }
              if (!remap.empty())
                stored_sig = substitute(fn_type, remap);
            }
            // Apply the same remap to the function's generic_params so that
            // generic_templates_[&fn] uses struct-aligned IDs. This keeps
            // bindings derived from the struct (instantiate_generic_struct,
            // cross-package method specialisation) keyed compatibly with the
            // GenericTemplate, both for ordered-args lookup in
            // emit_specialisation and for substitute() over the stored sig.
            for (auto &gp : generic_params) {
              auto it = id_remap.find(gp.id);
              if (it != id_remap.end())
                gp.id = it->second;
            }
            // Also register the struct-aligned signature so that
            // load_imported_method_decl (which iterates the struct's methods)
            // can resolve back to this FuncDeclNode.
            if (stored_sig && stored_sig.get() != fn_type.get())
              func_decl_by_type_[stored_sig.get()] = &fn;
            struct_info.methods.push_back(
                {std::string(fn.name.name), stored_sig, fn.is_public,
                 current_package_name()});
          }
        }
      }
    } else if (auto *ident =
                   std::get_if<IdentifierNode>(&recv_type_node->data)) {
      auto recv_sym = lookup(std::string(ident->name));
      if (recv_sym && recv_sym->type) {
        if (recv_sym->type->kind == TypeKind::Struct) {
          auto &struct_info = std::get<StructTypeInfo>(recv_sym->type->detail);
          if (struct_info.is_error) {
            error(recv_type_node->span,
                  std::format("methods cannot be attached to error type '{}'",
                              struct_info.name));
          } else if (check_recv_origin(struct_info.origin_package,
                                       struct_info.name)) {
            struct_info.methods.push_back(
                {std::string(fn.name.name), fn_type, fn.is_public,
                 current_package_name()});
          }
        } else if (recv_sym->type->kind == TypeKind::Alias) {
          auto &alias_info = std::get<AliasTypeInfo>(recv_sym->type->detail);
          if (alias_info.structural) {
            error(recv_type_node->span,
                  std::format("cannot attach methods to structural alias '{}'; "
                              "declare it nominal (`type {} {}`) to add methods",
                              alias_info.name, alias_info.name,
                              type_to_string(alias_info.underlying)));
          } else {
            alias_info.methods.push_back(
                {std::string(fn.name.name), fn_type, fn.is_public,
                 current_package_name()});
          }
        } else if (recv_sym->type->kind == TypeKind::Enum) {
          // Enums can also have methods bound to them.
          // Store in the type_methods side table.
          type_methods_[recv_sym->type.get()].push_back(
              {std::string(fn.name.name), fn_type, fn.is_public});
        } else if (recv_sym->type->kind == TypeKind::Int ||
                   recv_sym->type->kind == TypeKind::Float ||
                   recv_sym->type->kind == TypeKind::Bool ||
                   recv_sym->type->kind == TypeKind::String) {
          // Receiver methods on intrinsic types — stdlib only.
          if (!is_stdlib) {
            error(fn.receiver->name.span,
                  "receiver methods on intrinsic types can only be "
                  "defined in stdlib packages");
          } else {
            type_methods_[recv_sym->type.get()].push_back(
                {std::string(fn.name.name), fn_type, fn.is_public});
          }
        }
      }
    } else if (std::get_if<SelectorNode>(&recv_type_node->data)) {
      // Qualified receiver: pub fn (self pkg.Type) Method(...)
      // D4: types from other packages always have a different origin — reject.
      error(fn.receiver->name.span,
            "cannot bind receiver method to type from another package");
    } else if (auto *arr_tn =
                   std::get_if<ArrayTypeNode>(&recv_type_node->data)) {
      // Generic array receiver: pub fn (self [T]) Method(...) ...
      // Stdlib only. Normalize the T type-param ID to the sentinel 9990 so
      // that check_selector's existing substitution logic handles it.
      if (!is_stdlib) {
        error(fn.receiver->name.span,
              "receiver methods on generic types can only be "
              "defined in stdlib packages");
      } else {
        auto elem_type = resolve_type(*arr_tn->element_type);
        TypePtr normalized = fn_type;
        if (elem_type && elem_type->kind == TypeKind::TypeParam) {
          auto &tp = std::get<TypeParamInfo>(elem_type->detail);
          std::unordered_map<uint32_t, TypePtr> subst;
          subst[tp.param.id] = make_type_param(9990, "T");
          normalized = substitute(fn_type, subst);
        }
        auto &vec = kind_methods_[TypeKind::Array];
        bool dup = false;
        for (auto &e : vec)
          if (e.name == std::string(fn.name.name)) { dup = true; break; }
        if (!dup) {
          vec.push_back({std::string(fn.name.name), normalized, fn.is_public});
          kind_method_decls_[TypeKind::Array][std::string(fn.name.name)] =
              KindMethodDecl{&fn, fn_type, generic_params};
        }
      }
    } else if (auto *map_tn =
                   std::get_if<MapTypeNode>(&recv_type_node->data)) {
      // Generic map receiver: pub fn (self {K:V}) Method(...) ...
      // Stdlib only. Normalize K→9991, V→9992.
      if (!is_stdlib) {
        error(fn.receiver->name.span,
              "receiver methods on generic types can only be "
              "defined in stdlib packages");
      } else {
        auto key_type = resolve_type(*map_tn->key_type);
        auto val_type = resolve_type(*map_tn->value_type);
        TypePtr normalized = fn_type;
        std::unordered_map<uint32_t, TypePtr> subst;
        if (key_type && key_type->kind == TypeKind::TypeParam)
          subst[std::get<TypeParamInfo>(key_type->detail).param.id] =
              make_type_param(9991, "K");
        if (val_type && val_type->kind == TypeKind::TypeParam)
          subst[std::get<TypeParamInfo>(val_type->detail).param.id] =
              make_type_param(9992, "V");
        if (!subst.empty())
          normalized = substitute(fn_type, subst);
        auto &vec = kind_methods_[TypeKind::Map];
        bool dup = false;
        for (auto &e : vec)
          if (e.name == std::string(fn.name.name)) { dup = true; break; }
        if (!dup) {
          vec.push_back({std::string(fn.name.name), normalized, fn.is_public});
          kind_method_decls_[TypeKind::Map][std::string(fn.name.name)] =
              KindMethodDecl{&fn, fn_type, generic_params};
        }
      }
    }
  }

  if (fn.type_name && !fn.receiver)
    attach_type_method(fn, fn_type);

  if (has_generics)
    pop_scope();

  // Update the function symbol.
  auto sym_it = current_scope->symbols.find(std::string(fn.name.name));
  if (sym_it != current_scope->symbols.end()) {
    sym_it->second.type = fn_type;
  }

  // Record the reverse FuncDecl lookup so that check_call_expr can get
  // from a callee's type back to its AST for body instantiation.
  // For receiver methods on concrete types (e.g. generic methods on a
  // user struct), we also register here so the monomorphiser can find them.
  bool is_generic_on_concrete_recv =
      has_generics && fn.receiver &&
      fn_type && fn_type->kind == TypeKind::Func;
  // Generic receiver on Array/Map (kind_methods_) is also registered so
  // codegen can drive emit_specialisation per concrete K when the body
  // dispatches through a named protocol on a TypeParam value.
  bool is_generic_on_kind_recv = false;
  if (has_generics && fn.receiver) {
    auto &rt = fn.receiver->type->data;
    is_generic_on_kind_recv = std::get_if<ArrayTypeNode>(&rt) ||
                              std::get_if<MapTypeNode>(&rt);
  }
  if (fn_type && (!fn.receiver || is_generic_on_concrete_recv ||
                   is_generic_on_kind_recv)) {
    func_decl_by_type_[fn_type.get()] = &fn;
  }

  // Stash the generic template for lazy body-analysis at instantiation
  // time.  Generic receiver methods on concrete types are included so
  // their bodies are checked per-instantiation (same as free generics).
  if (has_generics && (!fn.receiver || is_generic_on_concrete_recv ||
                        is_generic_on_kind_recv)) {
    generic_templates_[&fn] =
        GenericTemplate{&fn, current_scope, std::move(generic_params),
                        is_stdlib};
  }
}

void Analyzer::attach_type_method(const FuncDeclNode &fn,
                                  const TypePtr &fn_type) {
  auto type_sym = lookup(std::string(fn.type_name->name));
  if (!type_sym || !type_sym->type || type_sym->kind != SymbolKind::Type) {
    error(fn.type_name->span,
          std::format("unknown type '{}' for type method", fn.type_name->name));
    return;
  }
  if (type_sym->type->kind != TypeKind::Struct) {
    error(fn.type_name->span,
          std::format("type methods are only supported on structs; '{}' is not "
                      "a struct",
                      fn.type_name->name));
    return;
  }
  auto &sinfo = std::get<StructTypeInfo>(type_sym->type->detail);
  if (sinfo.is_error) {
    error(fn.type_name->span,
          std::format("methods cannot be attached to error type '{}'",
                      fn.type_name->name));
    return;
  }
  if (!sinfo.origin_package.empty() &&
      sinfo.origin_package != current_package_name()) {
    error(fn.type_name->span,
          std::format("cannot bind a type method to type '{}' from another "
                      "package",
                      fn.type_name->name));
    return;
  }
  auto &vec = struct_type_methods_[type_sym->type.get()];
  for (auto &m : vec)
    if (m.name == std::string(fn.name.name)) {
      error(fn.name.span,
            std::format("type method '{}.{}' is already declared",
                        fn.type_name->name, fn.name.name));
      return;
    }
  vec.push_back({std::string(fn.name.name), fn_type, fn.is_public,
                 current_package_name()});
}

TypePtr Analyzer::lookup_struct_type_method(const TypePtr &struct_type,
                                            const std::string &name) const {
  if (!struct_type)
    return nullptr;
  auto it = struct_type_methods_.find(struct_type.get());
  if (it == struct_type_methods_.end())
    return nullptr;
  for (auto &m : it->second)
    if (m.name == name)
      return m.signature;
  return nullptr;
}

void Analyzer::resolve_struct_decl(const StructDeclNode &s) {
  std::vector<FieldInfo> fields;
  std::vector<MethodInfo> methods;
  std::vector<TypeParam> type_params;

  // If the struct is generic, push a temporary scope for type params.
  bool has_generics = s.generic.has_value();
  if (has_generics) {
    push_scope(ScopeKind::Block);
    type_params = enter_generics(*s.generic);
  }

  std::string struct_name(s.name.name);
  auto &target_scope = has_generics ? current_scope->parent : current_scope;

  // Publish the type before resolving fields, so a mention of the struct
  // inside its own body resolves to this very type rather than the Invalid
  // sentinel. The remaining detail is filled in below; every holder shares
  // the one TypePtr and sees it complete.
  auto struct_type = make_struct_type(struct_name, {}, std::move(methods),
                                      type_params, current_package_name());
  auto sym_it = target_scope->symbols.find(struct_name);
  if (sym_it != target_scope->symbols.end())
    sym_it->second.type = struct_type;

  resolving_structs_.insert(struct_name);

  // Resolve fields.  Methods are bound externally (`fn (x T) M()`) and
  // registered onto the struct type by resolve_func_decl.
  for (auto &member : s.members) {
    auto *fs = std::get_if<FieldSpecNode>(&member.member->data);
    if (!fs)
      continue;
    auto ft = resolve_type(*fs->type);
    for (auto &ident : fs->names.identifiers) {
      fields.push_back({std::string(ident.name), ft, member.is_public,
                        fs->default_value.get()});
    }
  }

  // Resolve embeds. Each entry is an IdentifierNode (local) or a
  // SelectorNode (`pkg.Name`); resolve_type handles both, so we get
  // qualified-name support for free.
  std::vector<TypePtr> embeds;
  for (auto &embed_node : s.embeds) {
    auto et = resolve_type(*embed_node);
    if (!et || et->kind == TypeKind::Invalid) continue;
    if (et->kind != TypeKind::Struct) {
      error(embed_node->span, "embedded type must be a struct");
      continue;
    }
    if (embed_name_taken(*embed_node, et, fields, embeds)) continue;
    embeds.push_back(et);
  }

  resolving_structs_.erase(struct_name);

  auto &info = std::get<StructTypeInfo>(struct_type->detail);
  info.fields = std::move(fields);
  info.embeds = std::move(embeds);

  if (has_generics) {
    pop_scope();
  }
}

// resolve_error_decl — a nominal error is struct-backed with the `is_error`
// marker, boxed at runtime as a pointer to { type_id, message, ...fields }.
// Field 0 is the hidden `type_id`; field 1 the mandatory `message` string
// (auto-injected; the body supplies only its optional `message = Expr`
// default); extra fields follow.
void Analyzer::resolve_error_decl(const ErrorDeclNode &e) {
  std::string error_name(e.name.name);
  auto err_type = make_struct_type(error_name, {}, /*methods=*/{},
                                   /*type_params=*/{}, current_package_name());
  std::get<StructTypeInfo>(err_type->detail).is_error = true;

  auto sym_it = current_scope->symbols.find(error_name);
  if (sym_it != current_scope->symbols.end())
    sym_it->second.type = err_type;

  std::vector<FieldInfo> fields;
  fields.push_back({"type_id", builtins.int64_type, /*is_public=*/false,
                    nullptr});
  fields.push_back({"message", builtins.string_type, /*is_public=*/true,
                    e.message_default.get()});

  for (auto &member : e.members) {
    auto *fs = std::get_if<FieldSpecNode>(&member.member->data);
    if (!fs)
      continue;
    auto ft = resolve_type(*fs->type);
    for (auto &ident : fs->names.identifiers)
      fields.push_back({std::string(ident.name), ft, member.is_public,
                        fs->default_value.get()});
  }

  std::get<StructTypeInfo>(err_type->detail).fields = std::move(fields);
}

std::optional<std::string> plain_string_literal(const NodePtr &n) {
  if (!n)
    return std::nullopt;
  auto *sl = std::get_if<StringLiteralNode>(&n->data);
  if (!sl || sl->fragments.size() != 1)
    return std::nullopt;
  auto *frag = std::get_if<StringFragmentNode>(&sl->fragments[0]->data);
  if (!frag)
    return std::nullopt;
  return unescape_string_fragment(*frag);
}

void Analyzer::resolve_enum_decl(const EnumDeclNode &e) {
  std::vector<EnumVariant> variants;
  int64_t next_index = 0;
  for (auto &field : e.fields) {
    int64_t index = next_index;
    std::string string_value;
    if (e.string_backed) {
      string_value = std::string(field.name.name);
      if (auto s = plain_string_literal(field.value))
        string_value = *s;
    } else if (field.value) {
      if (auto *lit = std::get_if<IntegerLiteralNode>(&field.value->data)) {
        std::string clean;
        for (char c : lit->literal)
          if (c != '_') clean += c;
        index = std::stoll(clean);
      }
    }
    variants.push_back(
        {std::string(field.name.name), index, std::move(string_value)});
    next_index = index + 1;
  }

  auto enum_type =
      make_enum_type(std::string(e.name.name), std::move(variants),
                     current_package_name(), e.string_backed);

  auto sym_it = current_scope->symbols.find(std::string(e.name.name));
  if (sym_it != current_scope->symbols.end()) {
    sym_it->second.type = enum_type;
  }
}

void Analyzer::resolve_interface_decl(const InterfaceDeclNode &i) {
  std::vector<TypeParam> type_params;

  bool has_generics = i.generic.has_value();
  if (has_generics) {
    push_scope(ScopeKind::Block);
    type_params = enter_generics(*i.generic);
  }

  // Forward-declare the interface type so its methods can reference the
  // interface itself (Go-style self-referential signatures, e.g.
  // Equals(Hashable) Bool inside `interface Hashable`).  The method list is
  // filled in after signature resolution.
  auto iface_type = make_interface_type(
      std::string(i.name.name), {}, type_params, current_package_name());

  auto &target_scope = has_generics ? current_scope->parent : current_scope;
  auto sym_it = target_scope->symbols.find(std::string(i.name.name));
  if (sym_it != target_scope->symbols.end()) {
    sym_it->second.type = iface_type;
  }

  std::vector<MethodInfo> methods;
  for (auto &field : i.methods) {
    auto fn_type = resolve_signature(field.signature);
    methods.push_back({std::string(field.name.name), fn_type, field.is_public,
                       current_package_name()});
  }

  // Patch the interface type with its resolved methods.  Embedded interfaces
  // are merged later in flatten_all_interfaces, once every interface's own
  // method set is resolved.
  std::get<InterfaceTypeInfo>(iface_type->detail).methods = std::move(methods);

  if (has_generics) {
    pop_scope();
  }
}

void Analyzer::flatten_all_interfaces(
    const std::vector<const InterfaceDeclNode *> &ifaces) {
  std::unordered_map<std::string, const InterfaceDeclNode *> by_name;
  for (auto *i : ifaces)
    by_name[std::string(i->name.name)] = i;

  std::unordered_map<std::string, int> state; // 0 unvisited, 1 active, 2 done
  for (auto *i : ifaces)
    flatten_interface(*i, by_name, state);
}

// Merge each embedded interface's method set into `decl`'s own.  Local embeds
// are flattened first (depth-first) so transitive methods are carried through;
// imported embeds already arrive flat from their `.sgi`.
void Analyzer::flatten_interface(
    const InterfaceDeclNode &decl,
    const std::unordered_map<std::string, const InterfaceDeclNode *> &by_name,
    std::unordered_map<std::string, int> &state) {
  std::string key(decl.name.name);
  if (state[key] == 2)
    return;
  if (state[key] == 1) {
    error(decl.span,
          std::format("interface '{}' is part of an embedding cycle", key));
    state[key] = 2;
    return;
  }
  state[key] = 1;

  auto sym = lookup(key);
  if (sym && sym->type && sym->type->kind == TypeKind::Interface) {
    auto &info = std::get<InterfaceTypeInfo>(sym->type->detail);
    for (auto &embed_node : decl.embeds)
      merge_embed(info, *embed_node, by_name, state);
  }

  state[key] = 2;
}

// Resolve a single embedded name to an interface and fold its methods in,
// recursing first when the embed names another local interface.
void Analyzer::merge_embed(
    InterfaceTypeInfo &info, const Node &embed_node,
    const std::unordered_map<std::string, const InterfaceDeclNode *> &by_name,
    std::unordered_map<std::string, int> &state) {
  if (auto *id = std::get_if<IdentifierNode>(&embed_node.data)) {
    auto it = by_name.find(std::string(id->name));
    if (it != by_name.end())
      flatten_interface(*it->second, by_name, state);
  }

  TypePtr embedded = resolve_type(embed_node);
  if (!embedded || is_invalid_type(embedded))
    return;
  if (embedded->kind != TypeKind::Interface) {
    error(embed_node.span,
          std::format("embedded type '{}' is not an interface",
                      type_to_string(embedded)));
    return;
  }
  merge_embedded_methods(info, embedded, embed_node);
}

void Analyzer::merge_embedded_methods(InterfaceTypeInfo &target,
                                      const TypePtr &embedded,
                                      const Node &embed_node) {
  auto &source = std::get<InterfaceTypeInfo>(embedded->detail);
  for (auto &method : source.methods) {
    auto existing =
        std::find_if(target.methods.begin(), target.methods.end(),
                     [&](const MethodInfo &m) { return m.name == method.name; });
    if (existing == target.methods.end()) {
      target.methods.push_back(method);
      continue;
    }
    if (!types_equal(existing->signature, method.signature))
      error(embed_node.span,
            std::format("embedded method '{}' conflicts with an existing "
                        "method of the same name",
                        method.name));
  }
}

void Analyzer::resolve_const_decl(const ConstDeclNode &c) {
  TypePtr const_type = nullptr;
  if (c.type) {
    const_type = resolve_type(**c.type);
  }

  // The initializer expression will be type-checked later; for now we just
  // record the declared type if present.
  auto sym_it = current_scope->symbols.find(std::string(c.name.name));
  if (sym_it != current_scope->symbols.end() && const_type) {
    sym_it->second.type = const_type;
  }
}

// type ID T      — nominal: distinct identity, inherits + shadows methods.
// type ID = T    — structural: transparent to the underlying, no own methods.
void Analyzer::resolve_type_decl(const TypeDeclNode &t) {
  auto underlying = resolve_type(*t.underlying);
  auto sym_it = current_scope->symbols.find(std::string(t.name.name));
  if (sym_it == current_scope->symbols.end())
    return;
  sym_it->second.kind = SymbolKind::Type;
  sym_it->second.type =
      make_alias_type(std::string(t.name.name), underlying, {},
                      current_package_name(), t.is_structural);
}

} // namespace saga
