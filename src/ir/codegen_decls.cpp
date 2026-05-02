// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/raw_ostream.h>

#include <charconv>

namespace mc {

/// Return true if the name refers to a scalar intrinsic type.
static bool is_intrinsic_type_name(std::string_view name) {
  return name == "Int" || name == "Int8" || name == "Int16" ||
         name == "Int32" || name == "Int64" ||
         name == "Uint8" || name == "Uint16" ||
         name == "Uint32" || name == "Uint64" ||
         name == "Float" || name == "Float32" || name == "Float64" ||
         name == "Bool" || name == "String";
}

void CodeGen::declare_functions(const SourceNode &src) {
  for (auto &decl : src.declarations) {
    if (auto *fn = std::get_if<FuncDeclNode>(&decl->data)) {
      // Generic functions are emitted lazily as monomorphised
      // specialisations at each call site (see monomorphism_plan.md,
      // Step 5).  The template itself has no concrete LLVM signature.
      // Receiver methods on generic receiver types (Array/Map) are
      // handled through their own codegen path.
      if (fn->generic) {
        if (!fn->receiver)
          continue;
        auto &rt = fn->receiver->type->data;
        bool is_generic_recv = std::get_if<ArrayTypeNode>(&rt) ||
                               std::get_if<MapTypeNode>(&rt);
        if (!is_generic_recv)
          continue;
      }

      // Skip receiver methods on intrinsic types — handled by
      // declare_intrinsic_methods or hardcoded codegen.
      if (fn->receiver) {
        auto &rt = fn->receiver->type->data;
        if (auto *ri = std::get_if<IdentifierNode>(&rt)) {
          if (is_intrinsic_type_name(ri->name))
            continue;
        }
        // Also skip generic receivers ([T], {K:V}) — these are dispatched
        // through the hardcoded array/map codegen path, not as direct calls.
        if (std::get_if<ArrayTypeNode>(&rt) || std::get_if<MapTypeNode>(&rt))
          continue;
      }
      std::string name(fn->name.name);
      bool is_main = (name == "Main");
      std::string link_name = is_main ? "main" : mangle(name);

      // Skip if already declared (e.g. by a previous source file).
      if (module->getFunction(link_name))
        continue;

      auto *fn_type = build_func_type(*fn);
      auto *func = llvm::Function::Create(
          fn_type, llvm::Function::ExternalLinkage, link_name, module.get());

      apply_func_abi_attrs(func, *fn);

      // Name the arguments for readability.
      size_t arg_idx = 0;
      // Skip the hidden sret arg if present.
      if (fn->signature.returns.size() == 1) {
        auto *r_ll = resolve_type_node(*fn->signature.returns[0]);
        if (r_ll && r_ll->isStructTy()) {
          if (arg_idx < func->arg_size())
            func->getArg(arg_idx++)->setName("sret.out");
        }
      }
      for (auto &param : fn->signature.params) {
        for (auto &ident : param.names.identifiers) {
          if (arg_idx < func->arg_size())
            func->getArg(arg_idx++)->setName(std::string(ident.name));
        }
      }
    }
  }
}

// ===========================================================================
// Struct declarations
// ===========================================================================

void CodeGen::declare_structs(const SourceNode &src) {
  for (auto &decl : src.declarations) {
    if (auto *s = std::get_if<StructDeclNode>(&decl->data))
      emit_struct_decl(*s);
  }
}

void CodeGen::emit_struct_decl(const StructDeclNode &node) {
  std::string name(node.name.name);
  std::string key = mangle(package_name, name);

  // If a body has already been set (re-declaration / re-emit), skip.
  auto existing = struct_types.find(key);
  if (existing != struct_types.end() && !existing->second->isOpaque())
    return;

  // Use the analyzer's already-resolved StructTypeInfo when available — the
  // analyzer's current_scope may not match this package by the time codegen
  // runs, so calling analyzer.resolve_type() on bare identifiers can fail.
  TypePtr struct_sem_type;
  if (analyzer.package_scope_) {
    auto sym_it = analyzer.package_scope_->symbols.find(name);
    if (sym_it != analyzer.package_scope_->symbols.end())
      struct_sem_type = sym_it->second.type;
  }

  std::vector<llvm::Type *> field_types;
  std::vector<std::string> field_names;

  if (struct_sem_type && struct_sem_type->kind == TypeKind::Struct) {
    auto &sinfo = std::get<StructTypeInfo>(struct_sem_type->detail);
    for (auto &fi : sinfo.fields) {
      field_types.push_back(llvm_type(fi.type));
      field_names.push_back(fi.name);
    }
  } else {
    for (auto &member : node.members) {
      if (auto *fs = std::get_if<FieldSpecNode>(&member.member->data)) {
        auto sem_type = analyzer.resolve_type(*fs->type);
        auto *ll = llvm_type(sem_type);
        for (auto &ident : fs->names.identifiers) {
          field_types.push_back(ll);
          field_names.push_back(std::string(ident.name));
        }
      }
    }
  }

  // Use a pre-existing opaque forward declaration if one was created by
  // llvm_type during recursive resolution above; otherwise create fresh.
  llvm::StructType *st = nullptr;
  auto it = struct_types.find(key);
  if (it != struct_types.end() && it->second->isOpaque()) {
    st = it->second;
    st->setBody(field_types);
  } else {
    st = llvm::StructType::create(context, field_types, "mc." + key);
    struct_types[key] = st;
  }
  struct_fields[key] = std::move(field_names);
}

// ===========================================================================
// Enum declarations
// ===========================================================================

// ===========================================================================
// Package-level constant declarations
// ===========================================================================

void CodeGen::emit_const_decl(const ConstDeclNode &node) {
  std::string name(node.name.name);
  std::string link_name = mangle(name);

  // Skip if already emitted (e.g. by a previous source file).
  if (module->getGlobalVariable(link_name))
    return;

  // Determine the semantic type.
  auto sem_type = semantic_type(*node.value);
  if (!sem_type && node.type)
    sem_type = analyzer.resolve_type(**node.type);
  if (!sem_type)
    return;

  auto *ll_type = llvm_type(sem_type);

  // Collection constants (arrays, maps) cannot be built as compile-time
  // LLVM constants — their backing storage is heap-allocated.  Emit a
  // mutable null-valued ptr global and queue the value expression so it
  // runs from `<pkg>__init__` before user code observes the global.
  if (sem_type->kind == TypeKind::Array || sem_type->kind == TypeKind::Map) {
    auto *ptr_ty = llvm::PointerType::getUnqual(context);
    new llvm::GlobalVariable(
        *module, ptr_ty, /*isConstant=*/false,
        llvm::GlobalValue::ExternalLinkage,
        llvm::Constant::getNullValue(ptr_ty), link_name);
    deferred_const_inits_.push_back(&node);
    init_function_needed = true;
    return;
  }

  // Try to build a constant initializer.
  llvm::Constant *init = nullptr;

  // Integer literal.
  if (auto *int_lit = std::get_if<IntegerLiteralNode>(&node.value->data)) {
    int64_t val = 0;
    auto sv = int_lit->literal;
    std::from_chars(sv.data(), sv.data() + sv.size(), val);
    init = llvm::ConstantInt::get(i64_type, val);
  }
  // Float literal.
  else if (auto *flt_lit = std::get_if<FloatLiteralNode>(&node.value->data)) {
    double val = std::stod(std::string(flt_lit->literal));
    init = llvm::ConstantFP::get(f64_type, val);
  }
  // Bool literal.
  else if (auto *bool_lit = std::get_if<BoolLiteralNode>(&node.value->data)) {
    init = llvm::ConstantInt::get(i1_type, bool_lit->literal == "true" ? 1 : 0);
  }
  // String literal (plain, no interpolation in const context).
  else if (auto *str_lit = std::get_if<StringLiteralNode>(&node.value->data)) {
    std::string text;
    for (auto &frag_node : str_lit->fragments) {
      if (auto *frag = std::get_if<StringFragmentNode>(&frag_node->data))
        text += unescape_fragment(frag->text);
    }
    // make_string_constant returns GlobalVariable* (a Constant* subclass).
    init = llvm::cast<llvm::Constant>(make_string_constant(text));
    ll_type = llvm::PointerType::getUnqual(context);
  }
  // Struct literal.
  else if (std::get_if<StructLiteralNode>(&node.value->data)) {
    if (auto *c = build_const_value(*node.value, sem_type)) {
      init = c;
      ll_type = c->getType();
    }
  }

  if (!init)
    init = llvm::Constant::getNullValue(ll_type);

  auto *gv = new llvm::GlobalVariable(
      *module, ll_type, /*isConstant=*/true,
      llvm::GlobalValue::ExternalLinkage, init, link_name);
  (void)gv;
}

llvm::Constant *CodeGen::build_const_value(const Node &val_node,
                                            const TypePtr &expected) {
  if (auto *il = std::get_if<IntegerLiteralNode>(&val_node.data)) {
    int64_t v = 0;
    auto sv = il->literal;
    std::from_chars(sv.data(), sv.data() + sv.size(), v);
    auto *ty = expected ? llvm_type(expected) : (llvm::Type *)i64_type;
    if (!ty || !ty->isIntegerTy()) ty = i64_type;
    return llvm::ConstantInt::get(ty, v);
  }
  if (auto *fl = std::get_if<FloatLiteralNode>(&val_node.data)) {
    double v = std::stod(std::string(fl->literal));
    auto *ty = expected ? llvm_type(expected) : (llvm::Type *)f64_type;
    if (!ty || !ty->isFloatingPointTy()) ty = f64_type;
    return llvm::ConstantFP::get(ty, v);
  }
  if (auto *bl = std::get_if<BoolLiteralNode>(&val_node.data)) {
    return llvm::ConstantInt::get(i1_type, bl->literal == "true" ? 1 : 0);
  }
  if (auto *sl = std::get_if<StringLiteralNode>(&val_node.data)) {
    std::string text;
    for (auto &frag_node : sl->fragments) {
      if (auto *frag = std::get_if<StringFragmentNode>(&frag_node->data))
        text += unescape_fragment(frag->text);
    }
    return llvm::cast<llvm::Constant>(make_string_constant(text));
  }
  if (auto *slit = std::get_if<StructLiteralNode>(&val_node.data)) {
    if (!expected || expected->kind != TypeKind::Struct) return nullptr;
    if (!std::holds_alternative<StructTypeInfo>(expected->detail))
      return nullptr;
    auto &sinfo = std::get<StructTypeInfo>(expected->detail);
    std::string skey = key_for(sinfo.origin_package, sinfo.name);
    auto st_it = struct_types.find(skey);
    if (st_it == struct_types.end()) return nullptr;
    auto *st = st_it->second;
    if (st->isOpaque()) llvm_type(expected);  // force body fill
    size_t n = st->getNumElements();
    auto &fnames = struct_fields[skey];
    std::vector<llvm::Constant *> field_vals(n, nullptr);
    for (size_t i = 0; i < n; ++i)
      field_vals[i] = llvm::Constant::getNullValue(st->getElementType(i));
    for (auto &fa : slit->fields) {
      std::string fname(fa.name.name);
      for (size_t i = 0; i < n && i < fnames.size(); ++i) {
        if (fnames[i] == fname) {
          TypePtr field_ty;
          for (auto &fi : sinfo.fields)
            if (fi.name == fname) { field_ty = fi.type; break; }
          if (auto *c = build_const_value(*fa.value, field_ty))
            field_vals[i] = c;
          break;
        }
      }
    }
    return llvm::ConstantStruct::get(st, field_vals);
  }
  return nullptr;
}

void CodeGen::declare_enums(const SourceNode &src) {
  for (auto &decl : src.declarations) {
    if (auto *e = std::get_if<EnumDeclNode>(&decl->data))
      emit_enum_decl(*e);
  }
}

void CodeGen::emit_enum_decl(const EnumDeclNode &node) {
  std::string name(node.name.name);
  std::string key = mangle(package_name, name);
  if (enum_types.count(key))
    return;
  enum_types[key] = true;

  int64_t next_index = 0;
  for (auto &field : node.fields) {
    std::string variant_name(field.name.name);

    // Check for explicit {index: N} override.
    for (auto &init : field.initializer) {
      std::string init_key(init.name.name);
      if (init_key == "index") {
        if (auto *lit =
                std::get_if<IntegerLiteralNode>(&init.value->data)) {
          std::string clean;
          for (char c : lit->literal)
            if (c != '_') clean += c;
          next_index = std::stoll(clean);
        }
      }
    }

    enum_variants[key + "." + variant_name] = next_index;
    next_index++;
  }
}

// ===========================================================================
// Interface declarations
// ===========================================================================

void CodeGen::declare_interfaces(const SourceNode &src) {
  for (auto &decl : src.declarations) {
    if (auto *i = std::get_if<InterfaceDeclNode>(&decl->data))
      emit_interface_decl(*i);
  }
}

void CodeGen::emit_interface_decl(const InterfaceDeclNode &node) {
  std::string name(node.name.name);
  std::string key = mangle(package_name, name);
  if (iface_vtable_types.count(key))
    return;

  auto *ptr_type = llvm::PointerType::getUnqual(context);

  // Build a vtable struct: one function pointer per method.
  std::vector<llvm::Type *> vtable_fields;
  std::vector<std::string> method_names;

  for (auto &method : node.methods) {
    vtable_fields.push_back(ptr_type); // fn ptr (opaque)
    method_names.push_back(std::string(method.name.name));
  }

  auto *vtable_st = llvm::StructType::create(
      context, vtable_fields, "mc.vtable." + key);
  iface_vtable_types[key] = vtable_st;
  iface_method_names[key] = std::move(method_names);

  // Build a minimal InterfaceTypeInfo for codegen type lookup.
  // We just need the name and method names for vtable matching.
  std::vector<MethodInfo> sem_methods;
  for (auto &m : node.methods) {
    // Build a simple FuncTypeInfo from the signature.
    std::vector<TypePtr> params;
    for (auto &p : m.signature.params) {
      auto pt = analyzer.resolve_type(*p.type);
      for (size_t i = 0; i < p.names.identifiers.size(); ++i)
        params.push_back(pt);
    }
    std::vector<TypePtr> returns;
    for (auto &r : m.signature.returns)
      returns.push_back(analyzer.resolve_type(*r));
    auto fn_type = make_func_type(std::move(params), std::move(returns));
    sem_methods.push_back(
        {std::string(m.name.name), fn_type, m.is_public, package_name});
  }
  named_sem_types[key] =
      make_interface_type(name, std::move(sem_methods), {}, package_name);
}

// ===========================================================================
// Struct method declarations
// ===========================================================================

CodeGen::MethodSig
CodeGen::build_method_signature(const FuncDeclNode &fn) {
  MethodSig sig;
  auto *ptr_type = llvm::PointerType::getUnqual(context);

  // Sret lowering for struct returns.
  llvm::Type *ret_type = void_ll_type;
  if (!fn.signature.returns.empty()) {
    auto *r = resolve_type_node(*fn.signature.returns[0]);
    if (r && r->isStructTy()) {
      sig.sret_struct_ty = r;
      ret_type = void_ll_type;
    } else {
      ret_type = r;
    }
  }

  std::vector<llvm::Type *> param_types;
  if (sig.sret_struct_ty)
    param_types.push_back(ptr_type);
  param_types.push_back(ptr_type); // self pointer
  for (auto &param : fn.signature.params) {
    auto *ll = resolve_type_node(*param.type);
    bool byval = ll && ll->isStructTy() && !param.is_variadic;
    for (size_t i = 0; i < param.names.identifiers.size(); ++i) {
      sig.byval_struct_tys.push_back(byval ? ll : nullptr);
      param_types.push_back(byval ? ptr_type : ll);
    }
  }
  sig.fn_type = llvm::FunctionType::get(ret_type, param_types, false);
  return sig;
}

void CodeGen::apply_method_abi_attrs(llvm::Function *func,
                                       const MethodSig &sig) {
  unsigned idx = 0;
  if (sig.sret_struct_ty) {
    llvm::AttrBuilder ab(context);
    ab.addStructRetAttr(sig.sret_struct_ty);
    ab.addAlignmentAttr(
        module->getDataLayout().getABITypeAlign(sig.sret_struct_ty));
    func->addParamAttrs(idx++, ab);
  }
  ++idx; // self
  for (auto *bv : sig.byval_struct_tys) {
    if (bv) {
      llvm::AttrBuilder ab(context);
      ab.addByValAttr(bv);
      ab.addAlignmentAttr(module->getDataLayout().getABITypeAlign(bv));
      func->addParamAttrs(idx, ab);
    }
    ++idx;
  }
}

void CodeGen::name_method_args(llvm::Function *func, const MethodSig &sig,
                               const FuncDeclNode &fn,
                               std::string_view receiver_name) {
  unsigned aidx = 0;
  if (sig.sret_struct_ty)
    func->getArg(aidx++)->setName("sret.out");
  func->getArg(aidx++)->setName(std::string(receiver_name));
  for (auto &param : fn.signature.params)
    for (auto &ident : param.names.identifiers)
      if (aidx < func->arg_size())
        func->getArg(aidx++)->setName(std::string(ident.name));
}

void CodeGen::declare_struct_method_symbols(const SourceNode &src) {
  // In-bound methods (defined inside a struct body).
  for (auto &decl : src.declarations) {
    auto *s = std::get_if<StructDeclNode>(&decl->data);
    if (!s)
      continue;

    std::string struct_name(s->name.name);
    std::string struct_key = mangle(package_name, struct_name);
    if (!struct_types.count(struct_key))
      continue;

    for (auto &member : s->members) {
      auto *fn = std::get_if<FuncDeclNode>(&member.member->data);
      if (!fn || fn->generic)
        continue;

      std::string method_name(fn->name.name);
      std::string link_name = mangle(struct_name + "__" + method_name);
      if (module->getFunction(link_name))
        continue;

      auto sig = build_method_signature(*fn);
      auto *func = llvm::Function::Create(
          sig.fn_type, llvm::Function::ExternalLinkage, link_name,
          module.get());
      apply_method_abi_attrs(func, sig);
      name_method_args(func, sig, *fn, "self");
    }
  }

  // Out-bound methods (top-level functions with a receiver).
  for (auto &decl : src.declarations) {
    auto *fn = std::get_if<FuncDeclNode>(&decl->data);
    if (!fn || !fn->receiver || fn->generic)
      continue;

    auto *recv_ident = std::get_if<IdentifierNode>(&fn->receiver->type->data);
    if (!recv_ident)
      continue;

    std::string struct_name(recv_ident->name);
    std::string struct_key = mangle(package_name, struct_name);
    if (!struct_types.count(struct_key))
      continue;

    std::string method_name(fn->name.name);
    std::string link_name = mangle(struct_name + "__" + method_name);
    if (module->getFunction(link_name))
      continue;

    auto sig = build_method_signature(*fn);
    auto *func = llvm::Function::Create(
        sig.fn_type, llvm::Function::ExternalLinkage, link_name, module.get());
    apply_method_abi_attrs(func, sig);
    name_method_args(func, sig, *fn, fn->receiver->name.name);
  }
}

void CodeGen::register_struct_method_links(const SourceNode &src) {
  auto register_link = [&](const std::string &struct_key,
                           const std::string &struct_name,
                           const std::string &method_name) {
    std::string link_name = mangle(struct_name + "__" + method_name);
    auto &links = struct_method_links[struct_key];
    for (auto &existing : links)
      if (existing.first == link_name)
        return;
    links.push_back({link_name, method_name});
  };

  for (auto &decl : src.declarations) {
    auto *s = std::get_if<StructDeclNode>(&decl->data);
    if (!s)
      continue;

    std::string struct_name(s->name.name);
    std::string struct_key = mangle(package_name, struct_name);
    if (!struct_types.count(struct_key))
      continue;

    for (auto &member : s->members) {
      auto *fn = std::get_if<FuncDeclNode>(&member.member->data);
      if (!fn || fn->generic)
        continue;
      register_link(struct_key, struct_name, std::string(fn->name.name));
    }
  }

  for (auto &decl : src.declarations) {
    auto *fn = std::get_if<FuncDeclNode>(&decl->data);
    if (!fn || !fn->receiver || fn->generic)
      continue;

    auto *recv_ident = std::get_if<IdentifierNode>(&fn->receiver->type->data);
    if (!recv_ident)
      continue;

    std::string struct_name(recv_ident->name);
    std::string struct_key = mangle(package_name, struct_name);
    if (!struct_types.count(struct_key))
      continue;

    register_link(struct_key, struct_name, std::string(fn->name.name));
  }
}

void CodeGen::emit_struct_methods(const SourceNode &src) {
  // Emit in-bound method bodies.
  for (auto &decl : src.declarations) {
    auto *s = std::get_if<StructDeclNode>(&decl->data);
    if (!s)
      continue;

    std::string struct_name(s->name.name);
    std::string struct_key = mangle(package_name, struct_name);

    for (auto &member : s->members) {
      auto *fn = std::get_if<FuncDeclNode>(&member.member->data);
      if (!fn)
        continue;
      if (fn->generic)
        continue;

      std::string method_name(fn->name.name);
      std::string link_name = mangle(struct_name + "__" + method_name);

      auto *func = module->getFunction(link_name);
      if (!func || !func->empty())
        continue;

      auto *entry = llvm::BasicBlock::Create(context, "entry", func);
      builder.SetInsertPoint(entry);

      locals.clear();
      managed_locals.clear();
      current_func_is_main = false;

      // Self parameter — alloca for the struct pointer.
      auto *self_alloca = create_entry_alloca(
          func, "self", llvm::PointerType::getUnqual(context));
      builder.CreateStore(func->getArg(0), self_alloca);
      locals["self"] = self_alloca;

      // Inject struct fields as locals so bare names (e.g. `fd`) resolve.
      // Load each field from self via GEP and copy into a local alloca.
      {
        auto st_it = struct_types.find(struct_key);
        if (st_it != struct_types.end()) {
          auto *st = st_it->second;
          auto &fields = struct_fields[struct_key];
          auto *self_ptr = builder.CreateLoad(
              llvm::PointerType::getUnqual(context), self_alloca, "self.ptr");
          for (size_t fi = 0; fi < fields.size(); ++fi) {
            auto *ftype = st->getElementType(fi);
            auto *gep = builder.CreateStructGEP(st, self_ptr, fi, fields[fi]);
            auto *val = builder.CreateLoad(ftype, gep, fields[fi] + ".val");
            auto *field_alloca = create_entry_alloca(func, fields[fi], ftype);
            builder.CreateStore(val, field_alloca);
            locals[fields[fi]] = field_alloca;
          }
        }
      }

      // Regular parameters.
      size_t arg_idx = 1;
      for (auto &param : fn->signature.params) {
        auto *ll_type = resolve_type_node(*param.type);
        for (auto &ident : param.names.identifiers) {
          std::string pname(ident.name);
          auto *alloca = create_entry_alloca(func, pname, ll_type);
          builder.CreateStore(func->getArg(arg_idx++), alloca);
          locals[pname] = alloca;
        }
      }

      // Emit body.
      auto &block = std::get<BlockNode>(fn->body->data);
      auto *tail_val = emit_block(block);

      if (!builder.GetInsertBlock()->getTerminator()) {
        emit_release_locals();
        auto *ret_type = func->getReturnType();
        if (ret_type->isVoidTy()) {
          builder.CreateRetVoid();
        } else if (tail_val && tail_val->getType() == ret_type) {
          builder.CreateRet(tail_val);
        } else if (tail_val && ret_type->isStructTy() &&
                   tail_val->getType()->isPointerTy()) {
          if (auto *ai = llvm::dyn_cast<llvm::AllocaInst>(tail_val)) {
            if (ai->getAllocatedType() == ret_type) {
              auto *loaded = builder.CreateLoad(ret_type, tail_val, "ret.union");
              builder.CreateRet(loaded);
            } else {
              builder.CreateRet(llvm::Constant::getNullValue(ret_type));
            }
          } else {
            builder.CreateRet(llvm::Constant::getNullValue(ret_type));
          }
        } else {
          builder.CreateRet(llvm::Constant::getNullValue(ret_type));
        }
      }

      llvm::verifyFunction(*func);
    }
  }

  // Emit out-bound method bodies (top-level functions with receivers).
  for (auto &decl : src.declarations) {
    auto *fn = std::get_if<FuncDeclNode>(&decl->data);
    if (!fn || !fn->receiver)
      continue;
    if (fn->generic)
      continue;

    auto *recv_ident = std::get_if<IdentifierNode>(&fn->receiver->type->data);
    if (!recv_ident)
      continue;

    std::string struct_name(recv_ident->name);
    if (!struct_types.count(mangle(package_name, struct_name)))
      continue;

    std::string method_name(fn->name.name);
    std::string link_name = mangle(struct_name + "__" + method_name);

    auto *func = module->getFunction(link_name);
    if (!func || !func->empty())
      continue;

    auto *entry = llvm::BasicBlock::Create(context, "entry", func);
    builder.SetInsertPoint(entry);

    locals.clear();
    managed_locals.clear();
    current_func_is_main = false;

    // Receiver parameter.
    std::string recv_name(fn->receiver->name.name);
    auto *recv_alloca = create_entry_alloca(
        func, recv_name, llvm::PointerType::getUnqual(context));
    builder.CreateStore(func->getArg(0), recv_alloca);
    locals[recv_name] = recv_alloca;

    // Regular parameters.
    size_t arg_idx = 1;
    for (auto &param : fn->signature.params) {
      auto *ll_type = resolve_type_node(*param.type);
      for (auto &ident : param.names.identifiers) {
        std::string pname(ident.name);
        auto *alloca = create_entry_alloca(func, pname, ll_type);
        builder.CreateStore(func->getArg(arg_idx++), alloca);
        locals[pname] = alloca;
      }
    }

    // Emit body.
    auto &block = std::get<BlockNode>(fn->body->data);
    auto *tail_val = emit_block(block);

    if (!builder.GetInsertBlock()->getTerminator()) {
      emit_release_locals();
      auto *ret_type = func->getReturnType();
      if (ret_type->isVoidTy()) {
        builder.CreateRetVoid();
      } else if (tail_val && tail_val->getType() == ret_type) {
        builder.CreateRet(tail_val);
      } else if (tail_val && ret_type->isStructTy() &&
                 tail_val->getType()->isPointerTy()) {
        if (auto *ai = llvm::dyn_cast<llvm::AllocaInst>(tail_val)) {
          if (ai->getAllocatedType() == ret_type) {
            auto *loaded = builder.CreateLoad(ret_type, tail_val, "ret.union");
            builder.CreateRet(loaded);
          } else {
            builder.CreateRet(llvm::Constant::getNullValue(ret_type));
          }
        } else {
          builder.CreateRet(llvm::Constant::getNullValue(ret_type));
        }
      } else {
        builder.CreateRet(llvm::Constant::getNullValue(ret_type));
      }
    }

    llvm::verifyFunction(*func);
  }
}


// ===========================================================================
// Intrinsic type methods (receiver methods on Int, Float, Bool, String,
// Array, Map)
// ===========================================================================

/// Determine the intrinsic type name for a receiver type node.
/// Returns the type name ("Int", "Float", "Bool", "String", "Array", "Map")
/// or empty string if not an intrinsic type receiver.
static std::string intrinsic_receiver_type_name(const Node &type_node) {
  if (auto *ident = std::get_if<IdentifierNode>(&type_node.data)) {
    if (is_intrinsic_type_name(ident->name))
      return std::string(ident->name);
  }
  if (std::get_if<ArrayTypeNode>(&type_node.data))
    return "Array";
  if (std::get_if<MapTypeNode>(&type_node.data))
    return "Map";
  return {};
}

/// Collect generic type parameter names from a FuncDeclNode.
static std::unordered_set<std::string>
collect_generic_names(const FuncDeclNode &fn) {
  std::unordered_set<std::string> names;
  if (fn.generic) {
    for (auto &tp : fn.generic->type_params) {
      if (auto *id = std::get_if<IdentifierNode>(&tp->data))
        names.insert(std::string(id->name));
    }
  }
  return names;
}

void CodeGen::declare_intrinsic_methods(const SourceNode &src) {
  for (auto &decl : src.declarations) {
    auto *fn = std::get_if<FuncDeclNode>(&decl->data);
    if (!fn || !fn->receiver)
      continue;

    std::string type_name = intrinsic_receiver_type_name(*fn->receiver->type);
    if (type_name.empty())
      continue;

    std::string method_name(fn->name.name);
    std::string link_name = mangle(type_name + "__" + method_name);

    if (module->getFunction(link_name))
      continue;

    // Collect generic type param names so we can map them to opaque ptr.
    auto gnames = collect_generic_names(*fn);
    auto resolve_or_ptr = [&](const Node &type_node) -> llvm::Type * {
      if (auto *id = std::get_if<IdentifierNode>(&type_node.data)) {
        if (gnames.count(std::string(id->name)))
          return llvm::PointerType::getUnqual(context);
      }
      return resolve_type_node(type_node);
    };

    // Build function type: first param is the value type of self.
    auto *self_type = resolve_or_ptr(*fn->receiver->type);
    std::vector<llvm::Type *> param_types;
    param_types.push_back(self_type);

    for (auto &param : fn->signature.params) {
      auto *ll = resolve_or_ptr(*param.type);
      for (size_t i = 0; i < param.names.identifiers.size(); ++i)
        param_types.push_back(ll);
    }

    llvm::Type *ret_type = void_ll_type;
    if (!fn->signature.returns.empty())
      ret_type = resolve_or_ptr(*fn->signature.returns[0]);

    auto *fn_type = llvm::FunctionType::get(ret_type, param_types, false);
    auto *func = llvm::Function::Create(
        fn_type, llvm::Function::ExternalLinkage, link_name, module.get());

    func->getArg(0)->setName(std::string(fn->receiver->name.name));
    size_t arg_idx = 1;
    for (auto &param : fn->signature.params)
      for (auto &ident : param.names.identifiers)
        if (arg_idx < func->arg_size())
          func->getArg(arg_idx++)->setName(std::string(ident.name));

    intrinsic_method_links[type_name].push_back({link_name, method_name});
  }
}

void CodeGen::emit_intrinsic_methods(const SourceNode &src) {
  for (auto &decl : src.declarations) {
    auto *fn = std::get_if<FuncDeclNode>(&decl->data);
    if (!fn || !fn->receiver)
      continue;

    std::string type_name = intrinsic_receiver_type_name(*fn->receiver->type);
    if (type_name.empty())
      continue;

    std::string method_name(fn->name.name);
    std::string link_name = mangle(type_name + "__" + method_name);

    auto *func = module->getFunction(link_name);
    if (!func || !func->empty())
      continue;

    auto *entry = llvm::BasicBlock::Create(context, "entry", func);
    builder.SetInsertPoint(entry);

    locals.clear();
    managed_locals.clear();
    current_func_is_main = false;
    current_func_return_sem = fn->signature.returns.empty()
        ? nullptr
        : semantic_type(*fn->signature.returns[0]);

    // Collect generic param names for type resolution.
    auto gnames = collect_generic_names(*fn);
    auto resolve_or_ptr = [&](const Node &type_node) -> llvm::Type * {
      if (auto *id = std::get_if<IdentifierNode>(&type_node.data)) {
        if (gnames.count(std::string(id->name)))
          return llvm::PointerType::getUnqual(context);
      }
      return resolve_type_node(type_node);
    };

    // Receiver parameter — self is a value, not a pointer to a struct.
    std::string recv_name(fn->receiver->name.name);
    auto *self_type = func->getArg(0)->getType();
    auto *recv_alloca = create_entry_alloca(func, recv_name, self_type);
    builder.CreateStore(func->getArg(0), recv_alloca);
    locals[recv_name] = recv_alloca;

    // Regular parameters.
    size_t arg_idx = 1;
    for (auto &param : fn->signature.params) {
      auto *ll_type = resolve_or_ptr(*param.type);
      for (auto &ident : param.names.identifiers) {
        std::string pname(ident.name);
        auto *alloca = create_entry_alloca(func, pname, ll_type);
        builder.CreateStore(func->getArg(arg_idx++), alloca);
        locals[pname] = alloca;
      }
    }

    // Emit body.
    auto &block = std::get<BlockNode>(fn->body->data);
    auto *tail_val = emit_block(block);

    if (!builder.GetInsertBlock()->getTerminator()) {
      emit_release_locals();
      auto *ret_type = func->getReturnType();
      if (ret_type->isVoidTy()) {
        builder.CreateRetVoid();
      } else if (tail_val && tail_val->getType() == ret_type) {
        builder.CreateRet(tail_val);
      } else if (tail_val && ret_type->isIntegerTy() &&
                 tail_val->getType()->isIntegerTy() &&
                 tail_val->getType() != ret_type) {
        unsigned src = tail_val->getType()->getIntegerBitWidth();
        unsigned dst = ret_type->getIntegerBitWidth();
        auto *conv = (src > dst)
            ? builder.CreateTrunc(tail_val, ret_type, "ret.trunc")
            : builder.CreateZExt(tail_val, ret_type, "ret.zext");
        builder.CreateRet(conv);
      } else {
        builder.CreateRet(llvm::Constant::getNullValue(ret_type));
      }
    }

    llvm::verifyFunction(*func);
  }
}

} // namespace mc
