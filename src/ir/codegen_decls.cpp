// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Verifier.h>

#include <charconv>

namespace mc {

/// Return true if the name refers to a scalar intrinsic type.
static bool is_intrinsic_type_name(std::string_view name) {
  return name == "Int" || name == "Float" || name == "Bool" ||
         name == "String";
}

void CodeGen::declare_functions(const SourceNode &src) {
  for (auto &decl : src.declarations) {
    if (auto *fn = std::get_if<FuncDeclNode>(&decl->data)) {
      // Generic free functions are emitted lazily as monomorphised
      // specialisations at each call site (see monomorphism_plan.md,
      // Step 5).  The template itself has no concrete LLVM signature.
      if (fn->generic && !fn->receiver)
        continue;

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

      // Name the arguments for readability.
      size_t arg_idx = 0;
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
  if (struct_types.count(name))
    return; // Already registered.

  // Resolve field types directly from the AST + analyzer.
  std::vector<llvm::Type *> field_types;
  std::vector<std::string> field_names;

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

  auto *st = llvm::StructType::create(context, field_types, "mc." + name);
  struct_types[name] = st;
  struct_fields[name] = std::move(field_names);
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
  // Struct literal.
  else if (auto *slit = std::get_if<StructLiteralNode>(&node.value->data)) {
    if (sem_type->kind == TypeKind::Struct) {
      auto &sinfo = std::get<StructTypeInfo>(sem_type->detail);
      auto st_it = struct_types.find(sinfo.name);
      if (st_it != struct_types.end()) {
        auto *st = st_it->second;
        auto &fnames = struct_fields[sinfo.name];
        // Build field constants (zero-init if not provided).
        std::vector<llvm::Constant *> field_vals(fnames.size(), nullptr);
        for (size_t i = 0; i < fnames.size(); ++i)
          field_vals[i] = llvm::Constant::getNullValue(st->getElementType(i));
        for (auto &fa : slit->fields) {
          std::string fname(fa.name.name);
          for (size_t i = 0; i < fnames.size(); ++i) {
            if (fnames[i] == fname) {
              // Try to evaluate field value as constant.
              if (auto *il = std::get_if<IntegerLiteralNode>(&fa.value->data)) {
                int64_t v = 0;
                auto sv = il->literal;
                std::from_chars(sv.data(), sv.data() + sv.size(), v);
                field_vals[i] = llvm::ConstantInt::get(
                    st->getElementType(i), v);
              } else if (auto *fl = std::get_if<FloatLiteralNode>(&fa.value->data)) {
                double v = std::stod(std::string(fl->literal));
                field_vals[i] = llvm::ConstantFP::get(
                    st->getElementType(i), v);
              } else if (auto *bl = std::get_if<BoolLiteralNode>(&fa.value->data)) {
                field_vals[i] = llvm::ConstantInt::get(
                    st->getElementType(i), bl->literal == "true" ? 1 : 0);
              }
              break;
            }
          }
        }
        init = llvm::ConstantStruct::get(st, field_vals);
        ll_type = st;
      }
    }
  }

  if (!init)
    init = llvm::Constant::getNullValue(ll_type);

  auto *gv = new llvm::GlobalVariable(
      *module, ll_type, /*isConstant=*/true,
      llvm::GlobalValue::ExternalLinkage, init, link_name);
  (void)gv;
}

void CodeGen::declare_enums(const SourceNode &src) {
  for (auto &decl : src.declarations) {
    if (auto *e = std::get_if<EnumDeclNode>(&decl->data))
      emit_enum_decl(*e);
  }
}

void CodeGen::emit_enum_decl(const EnumDeclNode &node) {
  std::string name(node.name.name);
  if (enum_types.count(name))
    return;
  enum_types[name] = true;

  int64_t next_index = 0;
  for (auto &field : node.fields) {
    std::string variant_name(field.name.name);

    // Check for explicit {index: N} override.
    for (auto &init : field.initializer) {
      std::string key(init.name.name);
      if (key == "index") {
        if (auto *lit =
                std::get_if<IntegerLiteralNode>(&init.value->data)) {
          std::string clean;
          for (char c : lit->literal)
            if (c != '_') clean += c;
          next_index = std::stoll(clean);
        }
      }
    }

    enum_variants[name + "." + variant_name] = next_index;
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
  if (iface_vtable_types.count(name))
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
      context, vtable_fields, "mc.vtable." + name);
  iface_vtable_types[name] = vtable_st;
  iface_method_names[name] = std::move(method_names);

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
    sem_methods.push_back({std::string(m.name.name), fn_type, m.is_public});
  }
  named_sem_types[name] = make_interface_type(name, std::move(sem_methods), {});
}

// ===========================================================================
// Struct method declarations
// ===========================================================================

void CodeGen::declare_struct_methods(const SourceNode &src) {
  for (auto &decl : src.declarations) {
    auto *s = std::get_if<StructDeclNode>(&decl->data);
    if (!s)
      continue;

    std::string struct_name(s->name.name);
    auto st_it = struct_types.find(struct_name);
    if (st_it == struct_types.end())
      continue;

    // In-bound methods (defined inside the struct body).
    for (auto &member : s->members) {
      auto *fn = std::get_if<FuncDeclNode>(&member.member->data);
      if (!fn)
        continue;

      std::string method_name(fn->name.name);
      std::string link_name = mangle(struct_name + "__" + method_name);

      if (module->getFunction(link_name))
        continue;

      // Build function type: first param is ptr to self struct.
      auto *ptr_type = llvm::PointerType::getUnqual(context);
      std::vector<llvm::Type *> param_types;
      param_types.push_back(ptr_type); // self pointer

      for (auto &param : fn->signature.params) {
        auto *ll = resolve_type_node(*param.type);
        for (size_t i = 0; i < param.names.identifiers.size(); ++i)
          param_types.push_back(ll);
      }

      llvm::Type *ret_type = void_ll_type;
      if (!fn->signature.returns.empty())
        ret_type = resolve_type_node(*fn->signature.returns[0]);

      auto *fn_type = llvm::FunctionType::get(ret_type, param_types, false);
      auto *func = llvm::Function::Create(
          fn_type, llvm::Function::ExternalLinkage, link_name, module.get());

      // Name arguments.
      func->getArg(0)->setName("self");
      size_t arg_idx = 1;
      for (auto &param : fn->signature.params)
        for (auto &ident : param.names.identifiers)
          if (arg_idx < func->arg_size())
            func->getArg(arg_idx++)->setName(std::string(ident.name));

      struct_method_links[struct_name].push_back({link_name, method_name});
    }
  }

  // Out-bound methods (top-level functions with a receiver).
  for (auto &decl : src.declarations) {
    auto *fn = std::get_if<FuncDeclNode>(&decl->data);
    if (!fn || !fn->receiver)
      continue;

    auto *recv_ident = std::get_if<IdentifierNode>(&fn->receiver->type->data);
    if (!recv_ident)
      continue;

    std::string struct_name(recv_ident->name);
    if (!struct_types.count(struct_name))
      continue;

    std::string method_name(fn->name.name);
    std::string link_name = mangle(struct_name + "__" + method_name);

    if (module->getFunction(link_name))
      continue;

    auto *ptr_type = llvm::PointerType::getUnqual(context);
    std::vector<llvm::Type *> param_types;
    param_types.push_back(ptr_type); // self pointer

    for (auto &param : fn->signature.params) {
      auto *ll = resolve_type_node(*param.type);
      for (size_t i = 0; i < param.names.identifiers.size(); ++i)
        param_types.push_back(ll);
    }

    llvm::Type *ret_type = void_ll_type;
    if (!fn->signature.returns.empty())
      ret_type = resolve_type_node(*fn->signature.returns[0]);

    auto *fn_type = llvm::FunctionType::get(ret_type, param_types, false);
    auto *func = llvm::Function::Create(
        fn_type, llvm::Function::ExternalLinkage, link_name, module.get());

    func->getArg(0)->setName(std::string(fn->receiver->name.name));
    size_t arg_idx = 1;
    for (auto &param : fn->signature.params)
      for (auto &ident : param.names.identifiers)
        if (arg_idx < func->arg_size())
          func->getArg(arg_idx++)->setName(std::string(ident.name));

    struct_method_links[struct_name].push_back({link_name, method_name});
  }
}

void CodeGen::emit_struct_methods(const SourceNode &src) {
  // Emit in-bound method bodies.
  for (auto &decl : src.declarations) {
    auto *s = std::get_if<StructDeclNode>(&decl->data);
    if (!s)
      continue;

    std::string struct_name(s->name.name);

    for (auto &member : s->members) {
      auto *fn = std::get_if<FuncDeclNode>(&member.member->data);
      if (!fn)
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
        auto st_it = struct_types.find(struct_name);
        if (st_it != struct_types.end()) {
          auto *st = st_it->second;
          auto &fields = struct_fields[struct_name];
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

    auto *recv_ident = std::get_if<IdentifierNode>(&fn->receiver->type->data);
    if (!recv_ident)
      continue;

    std::string struct_name(recv_ident->name);
    if (!struct_types.count(struct_name))
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
