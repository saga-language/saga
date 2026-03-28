// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>

#include <algorithm>
#include <charconv>
#include <format>

namespace mc {

// ===========================================================================
// Construction
// ===========================================================================

CodeGen::CodeGen(const std::string &module_name, Analyzer &analyzer)
    : module(std::make_unique<llvm::Module>(module_name, context)),
      builder(context),
      analyzer(analyzer) {
  // Set a default data layout so LLVM can compute type alignments.
  // This is overridden by write_object() with the actual target layout.
  module->setDataLayout("e-m:e-p270:32:32-p271:32:32-p272:64:64-"
                        "i64:64-i128:128-f80:128-n8:16:32:64-S128");
  init_types();
  declare_runtime();
}

// ===========================================================================
// Type helpers
// ===========================================================================

void CodeGen::init_types() {
  i64_type = llvm::Type::getInt64Ty(context);
  f64_type = llvm::Type::getDoubleTy(context);
  i1_type = llvm::Type::getInt1Ty(context);
  void_ll_type = llvm::Type::getVoidTy(context);

  // mc_string = { i8*, i64, i64 }  — data, len, refcount
  string_type = llvm::StructType::create(
      context,
      {llvm::PointerType::getUnqual(context), i64_type, i64_type},
      "mc_string");

  // Interface fat pointer: { ptr data, ptr vtable }
  auto *ptr_ty = llvm::PointerType::getUnqual(context);
  iface_fat_ptr_type = llvm::StructType::create(
      context, {ptr_ty, ptr_ty}, "mc_iface");

  // Register built-in enums.
  enum_types["Comparison"] = true;
  enum_variants["Comparison.Less"] = 0;
  enum_variants["Comparison.Equal"] = 1;
  enum_variants["Comparison.Greater"] = 2;
}

void CodeGen::declare_runtime() {
  auto *ptr_type = llvm::PointerType::getUnqual(context);

  // void mc_intrinsic_print(mc_string* s)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_intrinsic_print", module.get());

  // mc_string* mc_string_concat(mc_string* a, mc_string* b)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_string_concat", module.get());

  // int64_t mc_string_compare(mc_string* a, mc_string* b)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_string_compare", module.get());

  // mc_string* mc_int_to_string(int64_t val)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {i64_type}, false),
      llvm::Function::ExternalLinkage, "mc_int_to_string", module.get());

  // mc_string* mc_float_to_string(double val)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {f64_type}, false),
      llvm::Function::ExternalLinkage, "mc_float_to_string", module.get());

  // mc_string* mc_bool_to_string(int64_t val)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {i64_type}, false),
      llvm::Function::ExternalLinkage, "mc_bool_to_string", module.get());

  // mc_array* mc_array_new(i64 elem_size, i64 initial_cap)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {i64_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "mc_array_new", module.get());

  // void mc_array_push(mc_array* arr, void* elem)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_array_push", module.get());

  // void* mc_array_at(mc_array* arr, i64 index)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "mc_array_at", module.get());

  // i64 mc_array_size(mc_array* arr)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_array_size", module.get());

  // void mc_retain_string(mc_string* s)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_retain_string", module.get());

  // void mc_release_string(mc_string* s)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_release_string", module.get());

  // void mc_retain_array(mc_array* arr)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_retain_array", module.get());

  // void mc_release_array(mc_array* arr)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_release_array", module.get());
}

llvm::Type *CodeGen::llvm_type(const TypePtr &t) {
  if (!t)
    return void_ll_type;

  switch (t->kind) {
  case TypeKind::Int:
    return i64_type;
  case TypeKind::Float:
    return f64_type;
  case TypeKind::Bool:
    return i1_type;
  case TypeKind::String:
    return llvm::PointerType::getUnqual(context); // ptr to mc_string
  case TypeKind::Void:
    return void_ll_type;
  case TypeKind::Enum:
    return i64_type; // Enums are represented as i64 tags.
  case TypeKind::Struct: {
    auto &info = std::get<StructTypeInfo>(t->detail);
    auto it = struct_types.find(info.name);
    if (it != struct_types.end())
      return it->second;
    // Struct not yet registered — return opaque ptr as fallback.
    return llvm::PointerType::getUnqual(context);
  }
  case TypeKind::Interface:
    // Interfaces are represented as a fat pointer struct.
    return llvm::PointerType::getUnqual(context); // ptr to mc_iface
  default:
    return void_ll_type;
  }
}

llvm::AllocaInst *CodeGen::create_entry_alloca(llvm::Function *fn,
                                                const std::string &name,
                                                llvm::Type *type) {
  llvm::IRBuilder<> tmp_builder(&fn->getEntryBlock(),
                                fn->getEntryBlock().begin());
  return tmp_builder.CreateAlloca(type, nullptr, name);
}

// ===========================================================================
// Entry point
// ===========================================================================

void CodeGen::emit(const Node &root) {
  std::visit(
      overloaded{
          [&](const PackageNode &pkg) { emit_package(pkg); },
          [&](const SourceNode &src) { emit_source(src); },
          [&](const auto &) {},
      },
      root.data);
}

// ===========================================================================
// Top-level visitors
// ===========================================================================

void CodeGen::emit_package(const PackageNode &pkg) {
  for (auto &src : pkg.sources)
    emit_source(std::get<SourceNode>(src->data));
}

void CodeGen::emit_source(const SourceNode &src) {
  // Pass 1: create LLVM struct types, register enums and interfaces.
  declare_structs(src);
  declare_enums(src);
  declare_interfaces(src);

  // Pass 2: forward-declare all functions and struct methods.
  declare_functions(src);
  declare_struct_methods(src);

  // Pass 3: emit function bodies and struct method bodies.
  for (auto &decl : src.declarations) {
    std::visit(
        overloaded{
            [&](const FuncDeclNode &fn) { emit_func_decl(fn); },
            [&](const auto &) {},
        },
        decl->data);
  }
  emit_struct_methods(src);
}

// ===========================================================================
// Function forward-declarations
// ===========================================================================

void CodeGen::declare_functions(const SourceNode &src) {
  for (auto &decl : src.declarations) {
    if (auto *fn = std::get_if<FuncDeclNode>(&decl->data)) {
      std::string name(fn->name.name);
      bool is_main = (name == "Main");
      std::string link_name = is_main ? "main" : name;

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
      std::string link_name = struct_name + "." + method_name;

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
    std::string link_name = struct_name + "." + method_name;

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
      std::string link_name = struct_name + "." + method_name;

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

      // Struct fields are accessible directly by name through self.
      // We store self as a local so field accesses work via selectors.

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
    std::string link_name = struct_name + "." + method_name;

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
      } else {
        builder.CreateRet(llvm::Constant::getNullValue(ret_type));
      }
    }

    llvm::verifyFunction(*func);
  }
}

// ===========================================================================
// Vtable generation
// ===========================================================================

llvm::GlobalVariable *CodeGen::get_or_create_vtable(
    const std::string &struct_name, const std::string &iface_name) {
  std::string key = struct_name + "::" + iface_name;
  auto it = vtable_globals.find(key);
  if (it != vtable_globals.end())
    return it->second;

  auto vt_it = iface_vtable_types.find(iface_name);
  if (vt_it == iface_vtable_types.end())
    return nullptr;
  auto *vtable_st = vt_it->second;

  auto &method_names = iface_method_names[iface_name];
  auto &methods = struct_method_links[struct_name];

  // Build the vtable constant.
  std::vector<llvm::Constant *> entries;
  for (auto &iface_method : method_names) {
    // Find the corresponding struct method.
    std::string link_name = struct_name + "." + iface_method;
    auto *fn = module->getFunction(link_name);
    if (fn) {
      entries.push_back(fn);
    } else {
      // Method not found — null pointer (shouldn't happen if analyzer passed).
      entries.push_back(
          llvm::ConstantPointerNull::get(
              llvm::PointerType::getUnqual(context)));
    }
  }

  auto *vtable_const = llvm::ConstantStruct::get(vtable_st, entries);
  auto *vtable_global = new llvm::GlobalVariable(
      *module, vtable_st, true, llvm::GlobalValue::PrivateLinkage,
      vtable_const, "mc.vtable." + struct_name + "." + iface_name);

  vtable_globals[key] = vtable_global;
  return vtable_global;
}

// ===========================================================================
// Interface boxing
// ===========================================================================

llvm::Value *CodeGen::emit_interface_box(llvm::Value *concrete_val,
                                          const TypePtr &concrete_type,
                                          const TypePtr &iface_type) {
  if (!concrete_val || !concrete_type || !iface_type)
    return nullptr;
  if (iface_type->kind != TypeKind::Interface)
    return nullptr;


  auto &iface_info = std::get<InterfaceTypeInfo>(iface_type->detail);
  std::string iface_name = iface_info.name;

  // Determine the struct name.
  std::string struct_name;
  if (concrete_type->kind == TypeKind::Struct) {
    struct_name = std::get<StructTypeInfo>(concrete_type->detail).name;
  } else {
    return nullptr; // Only struct boxing supported for now.
  }

  // Get or create the vtable.
  auto *vtable = get_or_create_vtable(struct_name, iface_name);
  if (!vtable)
    return nullptr;

  // Allocate a fat pointer on the stack.
  auto *func = builder.GetInsertBlock()->getParent();
  auto *fat_alloca = create_entry_alloca(func, "iface.box", iface_fat_ptr_type);

  // Store the data pointer (the concrete struct pointer).
  auto *data_gep = builder.CreateStructGEP(iface_fat_ptr_type, fat_alloca, 0, "iface.data");
  builder.CreateStore(concrete_val, data_gep);

  // Store the vtable pointer.
  auto *vtable_gep = builder.CreateStructGEP(iface_fat_ptr_type, fat_alloca, 1, "iface.vtable");
  builder.CreateStore(vtable, vtable_gep);

  return fat_alloca;
}

// ===========================================================================
// Function type building
// ===========================================================================

llvm::Type *CodeGen::resolve_type_node(const Node &type_node) {
  if (auto *ident = std::get_if<IdentifierNode>(&type_node.data)) {
    std::string tname(ident->name);
    if (struct_types.count(tname))
      return llvm::PointerType::getUnqual(context); // struct as ptr
    if (enum_types.count(tname))
      return i64_type; // enum as i64 tag
    if (iface_vtable_types.count(tname))
      return llvm::PointerType::getUnqual(context); // interface as ptr to fat ptr
  }
  // Fall back to analyzer resolve (works for builtins).
  auto sem_type = analyzer.resolve_type(type_node);
  return llvm_type(sem_type);
}

llvm::FunctionType *CodeGen::build_func_type(const FuncDeclNode &fn) {
  bool is_main = (fn.name.name == "Main");
  std::string link_name = is_main ? "main" : std::string(fn.name.name);

  // Return type.
  llvm::Type *ret_type = void_ll_type;
  if (is_main) {
    ret_type = llvm::Type::getInt32Ty(context);
  } else if (fn.signature.returns.size() == 1) {
    ret_type = resolve_type_node(*fn.signature.returns[0]);
  } else if (fn.signature.returns.size() > 1) {
    // Multiple return values → pack into an LLVM struct.
    std::vector<llvm::Type *> ret_fields;
    for (auto &r : fn.signature.returns)
      ret_fields.push_back(resolve_type_node(*r));
    auto *st = llvm::StructType::create(context, ret_fields,
                                        "mc.ret." + link_name);
    multi_return_types[link_name] = st;
    multi_return_counts[link_name] = fn.signature.returns.size();
    ret_type = st;
  }

  // Parameter types.
  std::vector<llvm::Type *> param_types;
  if (!is_main) {
    for (auto &param : fn.signature.params) {
      auto *ll_type = resolve_type_node(*param.type);
      for (size_t i = 0; i < param.names.identifiers.size(); ++i)
        param_types.push_back(ll_type);
    }
  }

  return llvm::FunctionType::get(ret_type, param_types, /*isVarArg=*/false);
}

// ===========================================================================
// Function body emission
// ===========================================================================

void CodeGen::emit_func_decl(const FuncDeclNode &fn) {
  std::string name(fn.name.name);
  bool is_main = (name == "Main");
  std::string link_name = is_main ? "main" : name;

  auto *func = module->getFunction(link_name);
  if (!func)
    return; // Should have been forward-declared.

  auto *entry = llvm::BasicBlock::Create(context, "entry", func);
  builder.SetInsertPoint(entry);

  // Reset per-function state.
  locals.clear();
  managed_locals.clear();
  current_func_is_main = is_main;

  // Create allocas for parameters and store the incoming argument values.
  size_t arg_idx = 0;
  for (auto &param : fn.signature.params) {
    auto *ll_type = resolve_type_node(*param.type);
    for (auto &ident : param.names.identifiers) {
      std::string pname(ident.name);
      auto *alloca = create_entry_alloca(func, pname, ll_type);
      builder.CreateStore(func->getArg(arg_idx++), alloca);
      locals[pname] = alloca;
    }
  }

  // Emit body.
  auto &block = std::get<BlockNode>(fn.body->data);
  auto *tail_val = emit_block(block);

  // If the block didn't already terminate, release locals and return.
  if (!builder.GetInsertBlock()->getTerminator()) {
    emit_release_locals();
    auto *ret_type = func->getReturnType();
    if (is_main) {
      builder.CreateRet(
          llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0));
    } else if (ret_type->isVoidTy()) {
      builder.CreateRetVoid();
    } else if (tail_val && tail_val->getType() == ret_type) {
      // Tail expression: the last expression's value is the return value.
      // Retain the return value since the caller takes ownership, but
      // the release_locals above would have released it.
      builder.CreateRet(tail_val);
    } else {
      builder.CreateRet(llvm::Constant::getNullValue(ret_type));
    }
  }

  llvm::verifyFunction(*func);
}

// ===========================================================================
// Block / statement emission
// ===========================================================================

llvm::Value *CodeGen::emit_block(const BlockNode &block) {
  llvm::Value *last = nullptr;
  for (auto &stmt : block.stmts) {
    // If we already have a terminator (e.g. from a return), stop.
    if (builder.GetInsertBlock()->getTerminator())
      break;
    last = emit_expr(*stmt);
  }
  return last;
}

void CodeGen::emit_stmt(const Node &node) {
  std::visit(
      overloaded{
          [&](const VarDeclNode &n) { emit_var_decl(n); },
          [&](const DeclAssignNode &n) { emit_decl_assign(n); },
          [&](const AssignNode &n) { emit_assign(n); },
          [&](const ReturnNode &n) { emit_return(n); },
          [&](const IncrementNode &n) { emit_increment(n); },
          [&](const DecrementNode &n) { emit_decrement(n); },
          [&](const auto &) {
            // Everything else is an expression evaluated for side effects.
            emit_expr(node);
          },
      },
      node.data);
}

// ===========================================================================
// Statement emitters
// ===========================================================================

void CodeGen::emit_var_decl(const VarDeclNode &node) {
  std::string name(node.name.name);
  auto *func = builder.GetInsertBlock()->getParent();

  // Determine the LLVM type from the semantic type annotation or
  // the initializer's type.
  llvm::Type *var_type = i64_type; // default to Int
  if (node.type) {
    auto sem_type = analyzer.resolve_type(**node.type);
    var_type = llvm_type(sem_type);
  } else if (node.init) {
    // Infer from the init expression's semantic type.
    auto it = analyzer.node_types.find(&**node.init);
    if (it != analyzer.node_types.end())
      var_type = llvm_type(it->second);
  }

  // Determine semantic type for refcount tracking.
  TypePtr sem_type_ptr = nullptr;
  if (node.type) {
    // Try the node_types map first (set during analysis), falling back
    // to resolve_type for builtins.  For user-defined types like interfaces,
    // resolve_type may fail outside analyzer scope, so also check global
    // scope symbols.
    auto it = analyzer.node_types.find(&**node.type);
    if (it != analyzer.node_types.end()) {
      sem_type_ptr = it->second;
    } else {
      sem_type_ptr = analyzer.resolve_type(**node.type);
    }
    // If resolve returned an error/null, try our named_sem_types registry.
    if ((!sem_type_ptr || sem_type_ptr->kind == TypeKind::Error) && node.type) {
      if (auto *ident = std::get_if<IdentifierNode>(&(*node.type)->data)) {
        auto nst_it = named_sem_types.find(std::string(ident->name));
        if (nst_it != named_sem_types.end())
          sem_type_ptr = nst_it->second;
      }
    }
  } else if (node.init) {
    sem_type_ptr = semantic_type(**node.init);
  }

  if (node.init) {
    auto *val = emit_expr(**node.init);
    // Interface boxing: declared type is interface, init is a concrete struct.
    if (sem_type_ptr && sem_type_ptr->kind == TypeKind::Interface) {
      auto init_sem = semantic_type(**node.init);
      if (init_sem && init_sem->kind == TypeKind::Struct) {
        // We need the struct pointer, not the loaded value.
        // Check if the init expression is an identifier referencing
        // a local struct alloca.
        llvm::Value *struct_ptr = val;
        if (auto *id = std::get_if<IdentifierNode>(&(*node.init)->data)) {
          auto local_it = locals.find(std::string(id->name));
          if (local_it != locals.end())
            struct_ptr = local_it->second; // The alloca pointer.
        }
        if (struct_ptr) {
          auto *boxed = emit_interface_box(struct_ptr, init_sem, sem_type_ptr);
          if (boxed) {
            if (auto *ba = llvm::dyn_cast<llvm::AllocaInst>(boxed)) {
              ba->setName(name);
              locals[name] = ba;
            }
            return;
          }
        }
      }
    }

    // If the init is a struct alloca, alias it directly.
    if (val && llvm::isa<llvm::AllocaInst>(val)) {
      auto sem = semantic_type(**node.init);
      if (sem && sem->kind == TypeKind::Struct) {
        auto *alloca = llvm::cast<llvm::AllocaInst>(val);
        alloca->setName(name);
        locals[name] = alloca;
        return;
      }
    }
    auto *alloca = create_entry_alloca(func, name, var_type);
    locals[name] = alloca;
    if (val)
      builder.CreateStore(val, alloca);
  } else {
    auto *alloca = create_entry_alloca(func, name, var_type);
    locals[name] = alloca;
    // Zero-initialize.
    builder.CreateStore(llvm::Constant::getNullValue(var_type), alloca);
  }

  // Track for release at scope exit.
  track_managed(name, sem_type_ptr);
}

void CodeGen::emit_decl_assign(const DeclAssignNode &node) {
  auto *val = emit_expr(*node.value);
  auto *func = builder.GetInsertBlock()->getParent();
  auto val_sem = semantic_type(*node.value);

  // ── Multi-return unpacking ───────────────────────────────────────────
  // If there are multiple targets and the RHS is a multi-return struct,
  // extract each field into its own local variable.
  if (node.targets.identifiers.size() > 1 && val &&
      val->getType()->isStructTy()) {
    // Check if this is a known multi-return struct.
    std::string callee_name;
    if (auto *call = std::get_if<CallExprNode>(&node.value->data)) {
      if (auto *ident = std::get_if<IdentifierNode>(&call->callee->data))
        callee_name = std::string(ident->name);
    }
    std::string link_name = (callee_name == "Main") ? "main" : callee_name;
    auto mr_it = multi_return_types.find(link_name);

    if (!callee_name.empty() && mr_it != multi_return_types.end()) {
      auto *ret_st = mr_it->second;
      size_t count = std::min(node.targets.identifiers.size(),
                              (size_t)ret_st->getNumElements());

      // Resolve per-element semantic types from the callee's signature.
      // Look up the FuncDeclNode via the analyzer to get return types.
      std::vector<TypePtr> elem_sem_types;
      auto fn_sem = val_sem;
      if (fn_sem && fn_sem->kind == TypeKind::Func) {
        auto &fi = std::get<FuncTypeInfo>(fn_sem->detail);
        elem_sem_types = fi.returns;
      }

      for (size_t i = 0; i < count; ++i) {
        std::string name(node.targets.identifiers[i].name);
        auto *elem_val = builder.CreateExtractValue(val, i,
            name + ".unpack");
        auto *alloca = create_entry_alloca(func, name, elem_val->getType());
        locals[name] = alloca;
        builder.CreateStore(elem_val, alloca);

        // Track managed types.
        if (i < elem_sem_types.size())
          track_managed(name, elem_sem_types[i]);
      }
      return;
    }
  }

  // ── Single value assignment ──────────────────────────────────────────
  for (auto &ident : node.targets.identifiers) {
    std::string name(ident.name);

    // If the value is a struct alloca (from a struct literal), the variable
    // should just be an alias to that alloca — no extra indirection.
    if (val && llvm::isa<llvm::AllocaInst>(val)) {
      auto sem = semantic_type(*node.value);
      if (sem && sem->kind == TypeKind::Struct) {
        auto *alloca = llvm::cast<llvm::AllocaInst>(val);
        alloca->setName(name);
        locals[name] = alloca;
        continue;
      }
    }

    llvm::Type *var_type = val ? val->getType() : i64_type;
    auto *alloca = create_entry_alloca(func, name, var_type);
    locals[name] = alloca;
    if (val)
      builder.CreateStore(val, alloca);

    // Track managed types for release at scope exit.
    track_managed(name, val_sem);
  }
}

void CodeGen::emit_assign(const AssignNode &node) {
  for (size_t i = 0; i < node.targets.size() && i < node.values.size(); ++i) {
    auto *rhs = emit_expr(*node.values[i]);
    if (!rhs)
      continue;

    // Target can be an identifier or a selector (field assignment).
    if (auto *sel = std::get_if<SelectorNode>(&node.targets[i]->data)) {
      // Field assignment: obj.field = rhs
      std::string field_name(sel->field.name);
      if (auto *ident = std::get_if<IdentifierNode>(&sel->object->data)) {
        auto local_it = locals.find(std::string(ident->name));
        if (local_it != locals.end()) {
          auto sem = semantic_type(*sel->object);
          if (sem && sem->kind == TypeKind::Struct) {
            auto [gep, ftype] =
                struct_field_gep(local_it->second, sem, field_name);
            if (gep) {
              if (node.op == Token::Kind::Assignment) {
                builder.CreateStore(rhs, gep);
              } else {
                auto *cur = builder.CreateLoad(ftype, gep);
                llvm::Value *result = nullptr;
                using K = Token::Kind;
                switch (node.op) {
                case K::AddAssignment: result = builder.CreateAdd(cur, rhs, "add"); break;
                case K::SubAssignment: result = builder.CreateSub(cur, rhs, "sub"); break;
                case K::MulAssignment: result = builder.CreateMul(cur, rhs, "mul"); break;
                case K::DivAssignment: result = builder.CreateSDiv(cur, rhs, "div"); break;
                default: result = rhs; break;
                }
                builder.CreateStore(result, gep);
              }
            }
          }
        }
      }
      continue;
    }

    auto *ident = std::get_if<IdentifierNode>(&node.targets[i]->data);
    if (!ident)
      continue;

    auto it = locals.find(std::string(ident->name));
    if (it == locals.end())
      continue;

    auto *alloca = it->second;

    auto target_sem = semantic_type(*node.targets[i]);

    using K = Token::Kind;
    if (node.op == K::Assignment) {
      // Release old value before overwriting if managed.
      if (target_sem && (target_sem->kind == TypeKind::String ||
                         target_sem->kind == TypeKind::Array)) {
        auto *old = builder.CreateLoad(alloca->getAllocatedType(), alloca);
        emit_release(old, target_sem);
      }
      builder.CreateStore(rhs, alloca);
    } else {
      // Compound assignment: load current, apply op, store.
      auto *cur = builder.CreateLoad(alloca->getAllocatedType(), alloca);
      llvm::Value *result = nullptr;

      // Check if this is a string compound assignment.
      bool is_str = target_sem && target_sem->kind == TypeKind::String;

      if (is_str && node.op == K::AddAssignment) {
        auto *concat_fn = module->getFunction("mc_string_concat");
        result = builder.CreateCall(concat_fn, {cur, rhs}, "concat");
        // Release the old string since concat created a new one.
        emit_release(cur, target_sem);
      } else {
        switch (node.op) {
        case K::AddAssignment: result = builder.CreateAdd(cur, rhs, "add"); break;
        case K::SubAssignment: result = builder.CreateSub(cur, rhs, "sub"); break;
        case K::MulAssignment: result = builder.CreateMul(cur, rhs, "mul"); break;
        case K::DivAssignment: result = builder.CreateSDiv(cur, rhs, "div"); break;
        default: result = rhs; break;
        }
      }
      builder.CreateStore(result, alloca);
    }
  }
}

void CodeGen::emit_return(const ReturnNode &node) {
  if (current_func_is_main) {
    emit_release_locals();
    if (node.values.empty()) {
      builder.CreateRet(
          llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0));
    } else {
      auto *val = emit_expr(*node.values[0]);
      auto *i32_val = builder.CreateTrunc(val, llvm::Type::getInt32Ty(context),
                                          "main_ret");
      builder.CreateRet(i32_val);
    }
    return;
  }

  if (node.values.empty()) {
    emit_release_locals();
    builder.CreateRetVoid();
  } else if (node.values.size() == 1) {
    auto *val = emit_expr(*node.values[0]);
    emit_release_locals();
    if (val)
      builder.CreateRet(val);
    else
      builder.CreateRetVoid();
  } else {
    // Multiple return values — pack into a struct.
    auto *func = builder.GetInsertBlock()->getParent();
    std::string link_name(func->getName());
    auto it = multi_return_types.find(link_name);
    if (it != multi_return_types.end()) {
      auto *ret_st = it->second;
      llvm::Value *agg = llvm::UndefValue::get(ret_st);
      for (size_t i = 0; i < node.values.size(); ++i) {
        auto *val = emit_expr(*node.values[i]);
        if (val)
          agg = builder.CreateInsertValue(agg, val, i, "pack." + std::to_string(i));
      }
      emit_release_locals();
      builder.CreateRet(agg);
    } else {
      emit_release_locals();
      builder.CreateRetVoid();
    }
  }
}

void CodeGen::emit_increment(const IncrementNode &node) {
  auto *ident = std::get_if<IdentifierNode>(&node.operand->data);
  if (!ident) return;
  auto it = locals.find(std::string(ident->name));
  if (it == locals.end()) return;

  auto *alloca = it->second;
  auto *cur = builder.CreateLoad(alloca->getAllocatedType(), alloca);
  auto *one = llvm::ConstantInt::get(i64_type, 1);
  auto *inc = builder.CreateAdd(cur, one, "inc");
  builder.CreateStore(inc, alloca);
}

void CodeGen::emit_decrement(const DecrementNode &node) {
  auto *ident = std::get_if<IdentifierNode>(&node.operand->data);
  if (!ident) return;
  auto it = locals.find(std::string(ident->name));
  if (it == locals.end()) return;

  auto *alloca = it->second;
  auto *cur = builder.CreateLoad(alloca->getAllocatedType(), alloca);
  auto *one = llvm::ConstantInt::get(i64_type, 1);
  auto *dec = builder.CreateSub(cur, one, "dec");
  builder.CreateStore(dec, alloca);
}

// ===========================================================================
// Expression emission
// ===========================================================================

llvm::Value *CodeGen::emit_expr(const Node &node) {
  return std::visit(
      overloaded{
          [&](const IntegerLiteralNode &n) -> llvm::Value * {
            return emit_int_literal(n);
          },
          [&](const FloatLiteralNode &n) -> llvm::Value * {
            return emit_float_literal(n);
          },
          [&](const BoolLiteralNode &n) -> llvm::Value * {
            return emit_bool_literal(n);
          },
          [&](const StringLiteralNode &n) -> llvm::Value * {
            return emit_string_literal(n);
          },
          [&](const BinaryExprNode &n) -> llvm::Value * {
            return emit_binary_expr(n);
          },
          [&](const UnaryExprNode &n) -> llvm::Value * {
            return emit_unary_expr(n);
          },
          [&](const GroupExprNode &n) -> llvm::Value * {
            return emit_group_expr(n);
          },
          [&](const IfExprNode &n) -> llvm::Value * {
            return emit_if_expr(n);
          },
          [&](const ForExprNode &n) -> llvm::Value * {
            return emit_for_expr(n);
          },
          [&](const SwitchExprNode &n) -> llvm::Value * {
            return emit_switch_expr(n);
          },
          [&](const StructLiteralNode &n) -> llvm::Value * {
            return emit_struct_literal(n);
          },
          [&](const SelectorNode &n) -> llvm::Value * {
            return emit_selector(n, node);
          },
          [&](const ArrayLiteralNode &n) -> llvm::Value * {
            return emit_array_literal(n);
          },
          [&](const IndexExprNode &n) -> llvm::Value * {
            return emit_index_expr(n);
          },
          [&](const BreakNode &) -> llvm::Value * {
            if (!loop_stack.empty())
              builder.CreateBr(loop_stack.back().break_bb);
            return nullptr;
          },
          [&](const NextNode &) -> llvm::Value * {
            if (!loop_stack.empty())
              builder.CreateBr(loop_stack.back().next_bb);
            return nullptr;
          },
          [&](const CallExprNode &n) -> llvm::Value * {
            return emit_call_expr(n);
          },
          [&](const IdentifierNode &n) -> llvm::Value * {
            return emit_identifier(n);
          },
          [&](const VarDeclNode &n) -> llvm::Value * {
            emit_var_decl(n);
            return nullptr;
          },
          [&](const DeclAssignNode &n) -> llvm::Value * {
            emit_decl_assign(n);
            return nullptr;
          },
          [&](const AssignNode &n) -> llvm::Value * {
            emit_assign(n);
            return nullptr;
          },
          [&](const ReturnNode &n) -> llvm::Value * {
            emit_return(n);
            return nullptr;
          },
          [&](const IncrementNode &n) -> llvm::Value * {
            emit_increment(n);
            return nullptr;
          },
          [&](const DecrementNode &n) -> llvm::Value * {
            emit_decrement(n);
            return nullptr;
          },
          [&](const auto &) -> llvm::Value * {
            return nullptr;
          },
      },
      node.data);
}

// ===========================================================================
// Literal emission
// ===========================================================================

/// Parse an integer literal string, handling 0b, 0o, 0x prefixes and
/// underscore separators.
static int64_t parse_int_literal(std::string_view lit) {
  // Strip underscores.
  std::string clean;
  clean.reserve(lit.size());
  for (char c : lit) {
    if (c != '_')
      clean += c;
  }

  int base = 10;
  std::string_view digits = clean;
  if (digits.size() > 2 && digits[0] == '0') {
    switch (digits[1]) {
    case 'b': case 'B': base = 2;  digits = digits.substr(2); break;
    case 'o': case 'O': base = 8;  digits = digits.substr(2); break;
    case 'x': case 'X': base = 16; digits = digits.substr(2); break;
    default: break;
    }
  }

  int64_t val = 0;
  std::from_chars(digits.data(), digits.data() + digits.size(), val, base);
  return val;
}

static double parse_float_literal(std::string_view lit) {
  std::string clean;
  clean.reserve(lit.size());
  for (char c : lit) {
    if (c != '_')
      clean += c;
  }
  double val = 0.0;
  std::from_chars(clean.data(), clean.data() + clean.size(), val);
  return val;
}

llvm::Value *CodeGen::emit_int_literal(const IntegerLiteralNode &node) {
  int64_t val = parse_int_literal(node.literal);
  return llvm::ConstantInt::get(i64_type, static_cast<uint64_t>(val),
                                /*isSigned=*/true);
}

llvm::Value *CodeGen::emit_float_literal(const FloatLiteralNode &node) {
  double val = parse_float_literal(node.literal);
  return llvm::ConstantFP::get(f64_type, val);
}

llvm::Value *CodeGen::emit_bool_literal(const BoolLiteralNode &node) {
  bool val = (node.literal == "true");
  return llvm::ConstantInt::get(i1_type, val ? 1 : 0);
}

// ===========================================================================
// String literals
// ===========================================================================

static std::string unescape_fragment(std::string_view raw) {
  if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
    raw = raw.substr(1, raw.size() - 2);
  else if (raw.size() >= 1 && raw.front() == '"')
    raw = raw.substr(1);
  else if (raw.size() >= 1 && raw.back() == '"')
    raw = raw.substr(0, raw.size() - 1);
  if (raw.size() >= 1 && raw.front() == '}')
    raw = raw.substr(1);
  if (raw.size() >= 1 && raw.back() == '{')
    raw = raw.substr(0, raw.size() - 1);

  std::string out;
  out.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '\\' && i + 1 < raw.size()) {
      ++i;
      switch (raw[i]) {
      case 'n':  out += '\n'; break;
      case 't':  out += '\t'; break;
      case '\\': out += '\\'; break;
      case '"':  out += '"';  break;
      case '{':  out += '{';  break;
      default:   out += '\\'; out += raw[i]; break;
      }
    } else {
      out += raw[i];
    }
  }
  return out;
}

/// Convert an LLVM value to an mc_string* based on its semantic type.
llvm::Value *CodeGen::emit_to_string(llvm::Value *val, const TypePtr &sem) {
  if (!val || !sem)
    return val;

  switch (sem->kind) {
  case TypeKind::String:
    return val; // Already a string pointer.
  case TypeKind::Int: {
    auto *fn = module->getFunction("mc_int_to_string");
    return builder.CreateCall(fn, {val}, "istr");
  }
  case TypeKind::Float: {
    auto *fn = module->getFunction("mc_float_to_string");
    return builder.CreateCall(fn, {val}, "fstr");
  }
  case TypeKind::Bool: {
    auto *ext = builder.CreateZExt(val, i64_type, "bext");
    auto *fn = module->getFunction("mc_bool_to_string");
    return builder.CreateCall(fn, {ext}, "bstr");
  }
  default:
    // For types we can't convert, return an empty string placeholder.
    return make_string_constant("");
  }
}

llvm::Value *CodeGen::emit_string_literal(const StringLiteralNode &node) {
  // Check if this is a plain string (no interpolation).
  bool has_interp = false;
  for (auto &frag : node.fragments) {
    if (!std::holds_alternative<StringFragmentNode>(frag->data)) {
      has_interp = true;
      break;
    }
  }

  if (!has_interp) {
    // Plain string — concatenate all text fragments into one constant.
    std::string text;
    for (auto &frag : node.fragments) {
      if (auto *sf = std::get_if<StringFragmentNode>(&frag->data))
        text += unescape_fragment(sf->text);
    }
    return make_string_constant(text);
  }

  // Interpolated string — emit each part and concatenate.
  auto *concat_fn = module->getFunction("mc_string_concat");
  llvm::Value *result = nullptr;

  for (auto &frag : node.fragments) {
    llvm::Value *part = nullptr;

    if (auto *sf = std::get_if<StringFragmentNode>(&frag->data)) {
      std::string text = unescape_fragment(sf->text);
      if (text.empty())
        continue;
      part = make_string_constant(text);
    } else {
      // Interpolated expression — emit it and convert to string.
      auto *val = emit_expr(*frag);
      auto frag_sem = semantic_type(*frag);
      part = emit_to_string(val, frag_sem);
    }

    if (!part)
      continue;

    if (!result) {
      result = part;
    } else {
      result = builder.CreateCall(concat_fn, {result, part}, "interp");
    }
  }

  return result ? result : make_string_constant("");
}

llvm::Value *CodeGen::make_string_constant(const std::string &text) {
  auto it = string_constants.find(text);
  if (it != string_constants.end())
    return it->second;

  auto *char_array =
      llvm::ConstantDataArray::getString(context, text, /*AddNull=*/false);
  auto *raw_global = new llvm::GlobalVariable(
      *module, char_array->getType(), true,
      llvm::GlobalValue::PrivateLinkage, char_array, ".str");
  raw_global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  raw_global->setAlignment(llvm::Align(1));

  auto *data_ptr = llvm::ConstantExpr::getInBoundsGetElementPtr(
      char_array->getType(), raw_global,
      llvm::ArrayRef<llvm::Constant *>{
          llvm::ConstantInt::get(i64_type, 0),
          llvm::ConstantInt::get(i64_type, 0)});
  auto *length = llvm::ConstantInt::get(i64_type, text.size());
  auto *refcount = llvm::ConstantInt::getSigned(i64_type, -1); // static
  auto *str_const =
      llvm::ConstantStruct::get(string_type, {data_ptr, length, refcount});

  auto *str_global = new llvm::GlobalVariable(
      *module, string_type, true,
      llvm::GlobalValue::PrivateLinkage, str_const, ".mc_str");
  str_global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

  string_constants[text] = str_global;
  return str_global;
}

// ===========================================================================
// Semantic type query
// ===========================================================================

TypePtr CodeGen::semantic_type(const Node &node) const {
  auto it = analyzer.node_types.find(&node);
  if (it != analyzer.node_types.end())
    return it->second;
  return nullptr;
}

// ===========================================================================
// Binary expressions
// ===========================================================================

llvm::Value *CodeGen::emit_binary_expr(const BinaryExprNode &node) {
  // Check semantic types to detect string operations.
  auto lhs_sem = semantic_type(*node.lhs);
  bool is_string = lhs_sem && lhs_sem->kind == TypeKind::String;

  // ── String operations ────────────────────────────────────────────────
  if (is_string) {
    auto *lhs = emit_expr(*node.lhs);
    auto *rhs = emit_expr(*node.rhs);
    if (!lhs || !rhs)
      return nullptr;

    using K = Token::Kind;
    switch (node.op) {
    case K::Add: {
      auto *concat_fn = module->getFunction("mc_string_concat");
      return builder.CreateCall(concat_fn, {lhs, rhs}, "concat");
    }
    case K::Equal: {
      auto *cmp_fn = module->getFunction("mc_string_compare");
      auto *cmp = builder.CreateCall(cmp_fn, {lhs, rhs}, "strcmp");
      return builder.CreateICmpEQ(cmp, llvm::ConstantInt::get(i64_type, 0),
                                  "eq");
    }
    case K::NotEqual: {
      auto *cmp_fn = module->getFunction("mc_string_compare");
      auto *cmp = builder.CreateCall(cmp_fn, {lhs, rhs}, "strcmp");
      return builder.CreateICmpNE(cmp, llvm::ConstantInt::get(i64_type, 0),
                                  "ne");
    }
    case K::LessThan: {
      auto *cmp_fn = module->getFunction("mc_string_compare");
      auto *cmp = builder.CreateCall(cmp_fn, {lhs, rhs}, "strcmp");
      return builder.CreateICmpSLT(cmp, llvm::ConstantInt::get(i64_type, 0),
                                   "lt");
    }
    case K::GreaterThan: {
      auto *cmp_fn = module->getFunction("mc_string_compare");
      auto *cmp = builder.CreateCall(cmp_fn, {lhs, rhs}, "strcmp");
      return builder.CreateICmpSGT(cmp, llvm::ConstantInt::get(i64_type, 0),
                                   "gt");
    }
    case K::LessThanEqual: {
      auto *cmp_fn = module->getFunction("mc_string_compare");
      auto *cmp = builder.CreateCall(cmp_fn, {lhs, rhs}, "strcmp");
      return builder.CreateICmpSLE(cmp, llvm::ConstantInt::get(i64_type, 0),
                                   "le");
    }
    case K::GreaterThanEqual: {
      auto *cmp_fn = module->getFunction("mc_string_compare");
      auto *cmp = builder.CreateCall(cmp_fn, {lhs, rhs}, "strcmp");
      return builder.CreateICmpSGE(cmp, llvm::ConstantInt::get(i64_type, 0),
                                   "ge");
    }
    default:
      return nullptr;
    }
  }

  // ── Numeric / bool operations ────────────────────────────────────────
  auto *lhs = emit_expr(*node.lhs);
  auto *rhs = emit_expr(*node.rhs);
  if (!lhs || !rhs)
    return nullptr;

  bool is_float = lhs->getType()->isDoubleTy() || rhs->getType()->isDoubleTy();

  // Int→Float promotion if mixed.
  if (is_float) {
    if (lhs->getType()->isIntegerTy(64))
      lhs = builder.CreateSIToFP(lhs, f64_type, "itof");
    if (rhs->getType()->isIntegerTy(64))
      rhs = builder.CreateSIToFP(rhs, f64_type, "itof");
  }

  using K = Token::Kind;

  // ── Float arithmetic ─────────────────────────────────────────────────
  if (is_float) {
    switch (node.op) {
    case K::Add:      return builder.CreateFAdd(lhs, rhs, "fadd");
    case K::Sub:      return builder.CreateFSub(lhs, rhs, "fsub");
    case K::Multiply: return builder.CreateFMul(lhs, rhs, "fmul");
    case K::Divide:   return builder.CreateFDiv(lhs, rhs, "fdiv");
    case K::Modulo:   return builder.CreateFRem(lhs, rhs, "fmod");
    default: break;
    }
  }

  switch (node.op) {
  // ── Integer arithmetic ─────────────────────────────────────────────
  case K::Add:      return builder.CreateAdd(lhs, rhs, "add");
  case K::Sub:      return builder.CreateSub(lhs, rhs, "sub");
  case K::Multiply: return builder.CreateMul(lhs, rhs, "mul");
  case K::Divide:   return builder.CreateSDiv(lhs, rhs, "div");
  case K::Modulo:   return builder.CreateSRem(lhs, rhs, "mod");
  case K::Pow: {
    // TODO: proper pow intrinsic.
    return llvm::ConstantInt::get(i64_type, 0);
  }

  // ── Comparison ─────────────────────────────────────────────────────
  case K::Equal:
    return is_float ? builder.CreateFCmpOEQ(lhs, rhs, "eq")
                    : builder.CreateICmpEQ(lhs, rhs, "eq");
  case K::NotEqual:
    return is_float ? builder.CreateFCmpONE(lhs, rhs, "ne")
                    : builder.CreateICmpNE(lhs, rhs, "ne");
  case K::LessThan:
    return is_float ? builder.CreateFCmpOLT(lhs, rhs, "lt")
                    : builder.CreateICmpSLT(lhs, rhs, "lt");
  case K::LessThanEqual:
    return is_float ? builder.CreateFCmpOLE(lhs, rhs, "le")
                    : builder.CreateICmpSLE(lhs, rhs, "le");
  case K::GreaterThan:
    return is_float ? builder.CreateFCmpOGT(lhs, rhs, "gt")
                    : builder.CreateICmpSGT(lhs, rhs, "gt");
  case K::GreaterThanEqual:
    return is_float ? builder.CreateFCmpOGE(lhs, rhs, "ge")
                    : builder.CreateICmpSGE(lhs, rhs, "ge");

  // ── Logical ────────────────────────────────────────────────────────
  case K::LogicalAnd: return builder.CreateAnd(lhs, rhs, "and");
  case K::LogicalOr:  return builder.CreateOr(lhs, rhs, "or");

  // ── Bitwise ────────────────────────────────────────────────────────
  case K::BitwiseAnd: return builder.CreateAnd(lhs, rhs, "band");
  case K::BitwiseOr:  return builder.CreateOr(lhs, rhs, "bor");
  case K::BitwiseXor: return builder.CreateXor(lhs, rhs, "bxor");
  case K::LeftShift:  return builder.CreateShl(lhs, rhs, "shl");
  case K::RightShift: return builder.CreateAShr(lhs, rhs, "shr");

  default:
    return nullptr;
  }
}

// ===========================================================================
// Unary expressions
// ===========================================================================

llvm::Value *CodeGen::emit_unary_expr(const UnaryExprNode &node) {
  auto *operand = emit_expr(*node.operand);
  if (!operand)
    return nullptr;

  using K = Token::Kind;
  switch (node.op) {
  case K::Sub:
    if (operand->getType()->isDoubleTy())
      return builder.CreateFNeg(operand, "fneg");
    return builder.CreateNeg(operand, "neg");
  case K::Not:
    return builder.CreateNot(operand, "not");
  case K::BitwiseNot:
    return builder.CreateNot(operand, "bnot");
  default:
    return nullptr;
  }
}

// ===========================================================================
// Group expression
// ===========================================================================

llvm::Value *CodeGen::emit_group_expr(const GroupExprNode &node) {
  return emit_expr(*node.inner);
}

// ===========================================================================
// If/else expression
// ===========================================================================

llvm::Value *CodeGen::emit_if_expr(const IfExprNode &node) {
  auto *cond = emit_expr(*node.condition);
  if (!cond)
    return nullptr;

  // If the condition is an i64 (from a comparison that got widened), truncate
  // to i1.  If it's already i1, use it directly.
  if (!cond->getType()->isIntegerTy(1)) {
    cond = builder.CreateICmpNE(
        cond, llvm::Constant::getNullValue(cond->getType()), "tobool");
  }

  auto *func = builder.GetInsertBlock()->getParent();

  // Create basic blocks.
  auto *then_bb = llvm::BasicBlock::Create(context, "then", func);
  auto *merge_bb = llvm::BasicBlock::Create(context, "merge");
  llvm::BasicBlock *else_bb = nullptr;

  if (node.else_block) {
    else_bb = llvm::BasicBlock::Create(context, "else");
    builder.CreateCondBr(cond, then_bb, else_bb);
  } else {
    builder.CreateCondBr(cond, then_bb, merge_bb);
  }

  // ── Then block ─────────────────────────────────────────────────────
  builder.SetInsertPoint(then_bb);
  auto &then_block = std::get<BlockNode>(node.then_block->data);
  auto *then_val = emit_block(then_block);

  // If the then block didn't terminate, branch to merge.
  bool then_terminated = builder.GetInsertBlock()->getTerminator() != nullptr;
  if (!then_terminated)
    builder.CreateBr(merge_bb);
  // Record the actual ending block (may differ from then_bb if sub-blocks
  // were created).
  auto *then_end_bb = builder.GetInsertBlock();

  // ── Else block ─────────────────────────────────────────────────────
  llvm::Value *else_val = nullptr;
  llvm::BasicBlock *else_end_bb = nullptr;
  bool else_terminated = false;

  if (else_bb) {
    func->insert(func->end(), else_bb);
    builder.SetInsertPoint(else_bb);
    auto &else_block = std::get<BlockNode>((*node.else_block)->data);
    else_val = emit_block(else_block);
    else_terminated = builder.GetInsertBlock()->getTerminator() != nullptr;
    if (!else_terminated)
      builder.CreateBr(merge_bb);
    else_end_bb = builder.GetInsertBlock();
  }

  // ── Merge block ────────────────────────────────────────────────────
  // The merge block is reachable if at least one branch doesn't terminate.
  // With no else block, the false branch always reaches merge.
  bool merge_reachable = !then_terminated || !else_terminated;
  if (!node.else_block)
    merge_reachable = true; // false-branch falls through to merge

  // Only emit merge if both branches with else terminated — then it's dead.
  if (node.else_block && then_terminated && else_terminated) {
    // Both branches returned/broke — merge is unreachable.
    // Don't insert it; just leave the insert point at a terminated block.
    // We need a valid insert point though, so add it and mark unreachable.
    func->insert(func->end(), merge_bb);
    builder.SetInsertPoint(merge_bb);
    builder.CreateUnreachable();
    return nullptr;
  }

  func->insert(func->end(), merge_bb);
  builder.SetInsertPoint(merge_bb);

  // Build a PHI node if both branches produce a value of the same type.
  if (then_val && else_val &&
      then_val->getType() == else_val->getType() &&
      !then_terminated && !else_terminated) {
    auto *phi = builder.CreatePHI(then_val->getType(), 2, "ifval");
    phi->addIncoming(then_val, then_end_bb);
    phi->addIncoming(else_val, else_end_bb);
    return phi;
  }

  // If there's no else branch but the then branch produced a non-void value,
  // create a PHI with the zero value on the false path.
  if (then_val && !node.else_block && !then_terminated &&
      !then_val->getType()->isVoidTy()) {
    auto *zero = llvm::Constant::getNullValue(then_val->getType());
    // The false path comes directly from the entry (before the branch).
    // We need the predecessor of merge that isn't then_end_bb.
    // That's the block that had the conditional branch (entry).
    auto *false_pred = merge_bb->getSinglePredecessor();
    if (!false_pred) {
      // merge has two predecessors: then_end_bb and the original entry.
      for (auto *pred : llvm::predecessors(merge_bb)) {
        if (pred != then_end_bb) {
          false_pred = pred;
          break;
        }
      }
    }
    if (false_pred) {
      auto *phi = builder.CreatePHI(then_val->getType(), 2, "ifval");
      phi->addIncoming(then_val, then_end_bb);
      phi->addIncoming(zero, false_pred);
      return phi;
    }
  }

  return then_val;
}

// ===========================================================================
// Switch expression
// ===========================================================================

llvm::Value *CodeGen::emit_switch_expr(const SwitchExprNode &node) {
  auto *subject_val = emit_expr(*node.subject);
  if (!subject_val)
    return nullptr;

  auto subject_sem = semantic_type(*node.subject);
  bool is_string = subject_sem && subject_sem->kind == TypeKind::String;
  bool is_int_like = !is_string; // Int, Bool, Enum, Float treated as int-like

  auto *func = builder.GetInsertBlock()->getParent();
  auto *merge_bb = llvm::BasicBlock::Create(context, "sw.merge");

  // Collect case info for building PHI nodes.
  struct CaseResult {
    llvm::Value *value;
    llvm::BasicBlock *block;
    bool terminated;
  };
  std::vector<CaseResult> case_results;

  if (is_string) {
    // ── String matching: chained icmp + br ──────────────────────────
    auto *cmp_fn = module->getFunction("mc_string_compare");

    for (size_t i = 0; i < node.arms.size(); ++i) {
      auto &arm = node.arms[i];
      auto *case_bb = llvm::BasicBlock::Create(context,
          "sw.case." + std::to_string(i), func);
      auto *next_bb = llvm::BasicBlock::Create(context,
          "sw.next." + std::to_string(i));

      // Emit the pattern comparison.
      auto *pattern_val = emit_expr(*arm.pattern);
      auto *cmp = builder.CreateCall(cmp_fn, {subject_val, pattern_val}, "strcmp");
      auto *is_eq = builder.CreateICmpEQ(cmp,
          llvm::ConstantInt::get(i64_type, 0), "sw.eq");
      builder.CreateCondBr(is_eq, case_bb, next_bb);

      // Emit the case body.
      builder.SetInsertPoint(case_bb);
      llvm::Value *body_val = nullptr;
      if (auto *block = std::get_if<BlockNode>(&arm.body->data)) {
        body_val = emit_block(*block);
      } else {
        body_val = emit_expr(*arm.body);
      }
      bool terminated = builder.GetInsertBlock()->getTerminator() != nullptr;
      if (!terminated)
        builder.CreateBr(merge_bb);
      case_results.push_back({body_val, builder.GetInsertBlock(), terminated});

      // Continue with the next comparison.
      func->insert(func->end(), next_bb);
      builder.SetInsertPoint(next_bb);
    }

    // Else clause or fall through to merge.
    llvm::Value *else_val = nullptr;
    bool else_terminated = false;
    llvm::BasicBlock *else_end_bb = nullptr;
    if (node.else_body) {
      if (auto *block = std::get_if<BlockNode>(&(*node.else_body)->data)) {
        else_val = emit_block(*block);
      } else {
        else_val = emit_expr(**node.else_body);
      }
      else_terminated = builder.GetInsertBlock()->getTerminator() != nullptr;
      else_end_bb = builder.GetInsertBlock();
      if (!else_terminated)
        builder.CreateBr(merge_bb);
    } else {
      builder.CreateBr(merge_bb);
    }
    if (!else_end_bb)
      else_end_bb = builder.GetInsertBlock();
    case_results.push_back({else_val, else_end_bb, else_terminated});

  } else {
    // ── Integer/Enum/Bool matching: LLVM switch instruction ─────────
    // Create a default block for the else clause.
    auto *default_bb = llvm::BasicBlock::Create(context, "sw.default");

    // Count the number of case arms for the switch.
    auto *sw = builder.CreateSwitch(subject_val, default_bb,
                                    node.arms.size());

    // Emit each case arm.
    for (size_t i = 0; i < node.arms.size(); ++i) {
      auto &arm = node.arms[i];
      auto *case_bb = llvm::BasicBlock::Create(context,
          "sw.case." + std::to_string(i), func);

      // Resolve the case constant. For enum selectors and integer
      // literals this happens at codegen time.
      auto *pattern_val = emit_expr(*arm.pattern);
      if (auto *ci = llvm::dyn_cast_or_null<llvm::ConstantInt>(pattern_val)) {
        // If the subject is i1 (bool) but the constant is i64, truncate.
        if (subject_val->getType()->isIntegerTy(1) &&
            ci->getType()->isIntegerTy(64)) {
          sw->addCase(llvm::ConstantInt::get(
              llvm::Type::getInt1Ty(context),
              ci->getZExtValue() & 1), case_bb);
        } else if (ci->getType() == subject_val->getType()) {
          sw->addCase(ci, case_bb);
        } else {
          // Type mismatch — try an intcast.
          auto *cast = llvm::ConstantInt::get(
              llvm::cast<llvm::IntegerType>(subject_val->getType()),
              ci->getSExtValue());
          sw->addCase(cast, case_bb);
        }
      } else {
        // Non-constant pattern — fall back to chained comparison in default.
        // This shouldn't happen for well-formed programs, but add the
        // block anyway so it's not orphaned.
        sw->addCase(llvm::ConstantInt::get(
            llvm::cast<llvm::IntegerType>(subject_val->getType()), i),
            case_bb);
      }

      // Emit the case body.
      builder.SetInsertPoint(case_bb);
      llvm::Value *body_val = nullptr;
      if (auto *block = std::get_if<BlockNode>(&arm.body->data)) {
        body_val = emit_block(*block);
      } else {
        body_val = emit_expr(*arm.body);
      }
      bool terminated = builder.GetInsertBlock()->getTerminator() != nullptr;
      if (!terminated)
        builder.CreateBr(merge_bb);
      case_results.push_back({body_val, builder.GetInsertBlock(), terminated});
    }

    // Default / else block.
    func->insert(func->end(), default_bb);
    builder.SetInsertPoint(default_bb);
    llvm::Value *else_val = nullptr;
    bool else_terminated = false;
    if (node.else_body) {
      if (auto *block = std::get_if<BlockNode>(&(*node.else_body)->data)) {
        else_val = emit_block(*block);
      } else {
        else_val = emit_expr(**node.else_body);
      }
      else_terminated = builder.GetInsertBlock()->getTerminator() != nullptr;
    }
    if (!else_terminated)
      builder.CreateBr(merge_bb);
    case_results.push_back({else_val, builder.GetInsertBlock(), else_terminated});
  }

  // ── Merge block with optional PHI ──────────────────────────────────
  func->insert(func->end(), merge_bb);
  builder.SetInsertPoint(merge_bb);

  // Check if all results are the same non-void type for PHI.
  llvm::Type *phi_type = nullptr;
  bool all_have_value = true;
  bool any_reaches_merge = false;
  for (auto &cr : case_results) {
    if (!cr.terminated)
      any_reaches_merge = true;
    if (!cr.value || cr.terminated) {
      all_have_value = false;
    } else if (!phi_type) {
      phi_type = cr.value->getType();
    } else if (cr.value->getType() != phi_type) {
      all_have_value = false;
    }
  }

  if (!any_reaches_merge) {
    // All branches terminated (returned) — merge is unreachable.
    builder.CreateUnreachable();
    return nullptr;
  }

  if (all_have_value && phi_type && !phi_type->isVoidTy()) {
    unsigned incoming = 0;
    for (auto &cr : case_results)
      if (!cr.terminated)
        incoming++;
    auto *phi = builder.CreatePHI(phi_type, incoming, "sw.val");
    for (auto &cr : case_results) {
      if (!cr.terminated && cr.value)
        phi->addIncoming(cr.value, cr.block);
    }
    return phi;
  }

  return nullptr;
}

// ===========================================================================
// For loop expression
// ===========================================================================

llvm::Value *CodeGen::emit_for_expr(const ForExprNode &node) {
  auto *func = builder.GetInsertBlock()->getParent();

  // Create basic blocks for the loop structure.
  auto *cond_bb = llvm::BasicBlock::Create(context, "for.cond", func);
  auto *body_bb = llvm::BasicBlock::Create(context, "for.body");
  auto *update_bb = llvm::BasicBlock::Create(context, "for.update");
  auto *exit_bb = llvm::BasicBlock::Create(context, "for.exit");

  // Push loop context for break/next.
  loop_stack.push_back({exit_bb, update_bb});

  if (!node.mode) {
    // ── Infinite loop: for { ... } ─────────────────────────────────
    // No condition — branch directly to body, condition block is just
    // a trampoline.
    builder.CreateBr(body_bb);

    // Condition block (used as the "next" target — just jumps to body).
    // We already branched to body from entry, so cond_bb becomes the
    // "next" landing.  Rewrite: next goes to cond_bb which goes to body.
    builder.SetInsertPoint(cond_bb);
    builder.CreateBr(body_bb);

    // Update the loop context: next → cond_bb (which goes to body).
    loop_stack.back().next_bb = cond_bb;

    // Body.
    func->insert(func->end(), body_bb);
    builder.SetInsertPoint(body_bb);
    auto &body_block = std::get<BlockNode>(node.body->data);
    emit_block(body_block);
    if (!builder.GetInsertBlock()->getTerminator())
      builder.CreateBr(cond_bb);

  } else {
    auto &mode_node = *node.mode;

    if (auto *iter = std::get_if<ForIterClauseNode>(&mode_node->data)) {
      // ── C-style: for init; cond; update { ... } ───────────────────
      // Emit init in the current block.
      emit_expr(*iter->init);

      // Branch to condition.
      builder.CreateBr(cond_bb);

      // Condition block.
      builder.SetInsertPoint(cond_bb);
      auto *cond_val = emit_expr(*iter->condition);
      if (cond_val && !cond_val->getType()->isIntegerTy(1))
        cond_val = builder.CreateICmpNE(
            cond_val, llvm::Constant::getNullValue(cond_val->getType()),
            "tobool");
      if (cond_val)
        builder.CreateCondBr(cond_val, body_bb, exit_bb);
      else
        builder.CreateBr(body_bb);

      // Body.
      func->insert(func->end(), body_bb);
      builder.SetInsertPoint(body_bb);
      auto &body_block = std::get<BlockNode>(node.body->data);
      emit_block(body_block);
      if (!builder.GetInsertBlock()->getTerminator())
        builder.CreateBr(update_bb);

      // Update block.
      func->insert(func->end(), update_bb);
      builder.SetInsertPoint(update_bb);
      emit_expr(*iter->update);
      builder.CreateBr(cond_bb);

    } else if (auto *range = std::get_if<ForRangeClauseNode>(&mode_node->data)) {
      // ── Range-based: for v : iterable { ... } ─────────────────────
      auto *iterable = emit_expr(*range->iterable);
      if (!iterable) {
        builder.CreateBr(exit_bb);
      } else {
        auto iter_sem = semantic_type(*range->iterable);
        bool is_array = iter_sem && iter_sem->kind == TypeKind::Array;

        if (is_array) {
          // Get array size.
          auto *size_fn = module->getFunction("mc_array_size");
          auto *arr_len = builder.CreateCall(size_fn, {iterable}, "arr.len");

          // Create index variable.
          auto *idx_alloca = create_entry_alloca(func, ".idx", i64_type);
          builder.CreateStore(llvm::ConstantInt::get(i64_type, 0), idx_alloca);

          // Create allocas for loop variables.
          llvm::AllocaInst *key_alloca = nullptr;
          llvm::AllocaInst *val_alloca = nullptr;

          auto &arr_info = std::get<ArrayTypeInfo>(iter_sem->detail);
          auto *elem_ll = llvm_type(arr_info.element);

          if (range->vars.size() == 1) {
            val_alloca = create_entry_alloca(
                func, std::string(range->vars[0].name), elem_ll);
            locals[std::string(range->vars[0].name)] = val_alloca;
          } else if (range->vars.size() == 2) {
            key_alloca = create_entry_alloca(
                func, std::string(range->vars[0].name), i64_type);
            locals[std::string(range->vars[0].name)] = key_alloca;
            val_alloca = create_entry_alloca(
                func, std::string(range->vars[1].name), elem_ll);
            locals[std::string(range->vars[1].name)] = val_alloca;
          }

          // Branch to condition.
          builder.CreateBr(cond_bb);

          // Condition: idx < len.
          builder.SetInsertPoint(cond_bb);
          auto *cur_idx = builder.CreateLoad(i64_type, idx_alloca, "idx");
          auto *cmp = builder.CreateICmpSLT(cur_idx, arr_len, "range.cmp");
          builder.CreateCondBr(cmp, body_bb, exit_bb);

          // Body: load element, store to loop var(s).
          func->insert(func->end(), body_bb);
          builder.SetInsertPoint(body_bb);

          auto *at_fn = module->getFunction("mc_array_at");
          auto *body_idx = builder.CreateLoad(i64_type, idx_alloca, "idx");
          auto *elem_ptr =
              builder.CreateCall(at_fn, {iterable, body_idx}, "at");
          auto *elem_val =
              builder.CreateLoad(elem_ll, elem_ptr, "elem");

          if (key_alloca)
            builder.CreateStore(body_idx, key_alloca);
          if (val_alloca)
            builder.CreateStore(elem_val, val_alloca);

          auto &body_block = std::get<BlockNode>(node.body->data);
          emit_block(body_block);
          if (!builder.GetInsertBlock()->getTerminator())
            builder.CreateBr(update_bb);

          // Update: idx++.
          func->insert(func->end(), update_bb);
          builder.SetInsertPoint(update_bb);
          auto *upd_idx = builder.CreateLoad(i64_type, idx_alloca, "idx");
          auto *next_idx =
              builder.CreateAdd(upd_idx, llvm::ConstantInt::get(i64_type, 1),
                                "idx.next");
          builder.CreateStore(next_idx, idx_alloca);
          builder.CreateBr(cond_bb);
        } else {
          // Non-array iterable — not yet supported.
          builder.CreateBr(exit_bb);
        }
      }

    } else {
      // ── Condition-only: for cond { ... } ──────────────────────────
      builder.CreateBr(cond_bb);

      // Condition block.
      builder.SetInsertPoint(cond_bb);
      auto *cond_val = emit_expr(*mode_node);
      if (cond_val && !cond_val->getType()->isIntegerTy(1))
        cond_val = builder.CreateICmpNE(
            cond_val, llvm::Constant::getNullValue(cond_val->getType()),
            "tobool");
      if (cond_val)
        builder.CreateCondBr(cond_val, body_bb, exit_bb);
      else
        builder.CreateBr(body_bb);

      // Body.
      func->insert(func->end(), body_bb);
      builder.SetInsertPoint(body_bb);
      auto &body_block = std::get<BlockNode>(node.body->data);
      emit_block(body_block);
      if (!builder.GetInsertBlock()->getTerminator())
        builder.CreateBr(update_bb);

      // Update block (just loops back to condition).
      func->insert(func->end(), update_bb);
      builder.SetInsertPoint(update_bb);
      builder.CreateBr(cond_bb);
    }
  }

  // Pop loop context.
  loop_stack.pop_back();

  // Exit block.
  func->insert(func->end(), exit_bb);
  builder.SetInsertPoint(exit_bb);

  return nullptr;
}

// ===========================================================================
// Array literals
// ===========================================================================

llvm::Value *CodeGen::emit_array_literal(const ArrayLiteralNode &node) {
  // Determine element size from the semantic type.
  auto sem = semantic_type(
      *reinterpret_cast<const Node *>(&node)); // hack: node is embedded
  // Try to get element type from the first element.
  int64_t elem_size = 8; // default to i64 size
  llvm::Type *elem_ll_type = i64_type;

  if (!node.elements.empty()) {
    auto first_sem = semantic_type(*node.elements[0]);
    if (first_sem) {
      elem_ll_type = llvm_type(first_sem);
      if (elem_ll_type->isDoubleTy())
        elem_size = 8;
      else if (elem_ll_type->isIntegerTy(1))
        elem_size = 1;
      else if (elem_ll_type->isIntegerTy(64))
        elem_size = 8;
      else if (elem_ll_type->isPointerTy())
        elem_size = 8;
    }
  }

  // Create the array: mc_array_new(elem_size, initial_cap)
  auto *new_fn = module->getFunction("mc_array_new");
  auto *arr = builder.CreateCall(
      new_fn,
      {llvm::ConstantInt::get(i64_type, elem_size),
       llvm::ConstantInt::get(i64_type,
                              std::max((int64_t)node.elements.size(), (int64_t)4))},
      "arr");

  // Push each element.
  auto *push_fn = module->getFunction("mc_array_push");
  auto *func = builder.GetInsertBlock()->getParent();

  for (auto &elem_node : node.elements) {
    auto *val = emit_expr(*elem_node);
    if (!val)
      continue;

    // mc_array_push takes a void* to the element.  We need to store the
    // value to a temporary alloca and pass its address.
    auto *tmp = create_entry_alloca(func, "elem.tmp", val->getType());
    builder.CreateStore(val, tmp);
    builder.CreateCall(push_fn, {arr, tmp});
  }

  return arr;
}

// ===========================================================================
// Index expressions
// ===========================================================================

llvm::Value *CodeGen::emit_index_expr(const IndexExprNode &node) {
  auto *obj = emit_expr(*node.object);
  if (!obj)
    return nullptr;

  // Check if this is an array index.
  auto obj_sem = semantic_type(*node.object);
  if (obj_sem && obj_sem->kind == TypeKind::Array) {
    auto *idx = emit_expr(*node.index);
    if (!idx)
      return nullptr;

    auto *at_fn = module->getFunction("mc_array_at");
    auto *elem_ptr = builder.CreateCall(at_fn, {obj, idx}, "at");

    // Determine the element type to load.
    auto &arr_info = std::get<ArrayTypeInfo>(obj_sem->detail);
    auto *elem_ll = llvm_type(arr_info.element);
    return builder.CreateLoad(elem_ll, elem_ptr, "elem");
  }

  // String indexing — deferred for now.
  return nullptr;
}

// ===========================================================================
// Struct literals
// ===========================================================================

llvm::Value *CodeGen::emit_struct_literal(const StructLiteralNode &node) {
  // Resolve the struct's semantic type.
  auto sem = semantic_type(*node.type_expr);
  if (!sem || sem->kind != TypeKind::Struct)
    return nullptr;

  auto &info = std::get<StructTypeInfo>(sem->detail);
  auto st_it = struct_types.find(info.name);
  if (st_it == struct_types.end())
    return nullptr;

  auto *st = st_it->second;
  auto &fields = struct_fields[info.name];

  // Allocate the struct on the stack.
  auto *func = builder.GetInsertBlock()->getParent();
  auto *alloca = create_entry_alloca(func, info.name + ".lit", st);

  // Zero-initialize all fields.
  builder.CreateStore(llvm::Constant::getNullValue(st), alloca);

  // Store each provided field value.
  for (auto &fa : node.fields) {
    std::string fname(fa.name.name);
    // Find the field index.
    int idx = -1;
    for (size_t i = 0; i < fields.size(); ++i) {
      if (fields[i] == fname) {
        idx = static_cast<int>(i);
        break;
      }
    }
    if (idx < 0)
      continue;

    auto *val = emit_expr(*fa.value);
    if (!val)
      continue;

    auto *gep = builder.CreateStructGEP(st, alloca, idx, fname);
    builder.CreateStore(val, gep);
  }

  // Return a pointer to the struct alloca.
  return alloca;
}

// ===========================================================================
// Selector (field access)
// ===========================================================================

std::pair<llvm::Value *, llvm::Type *>
CodeGen::struct_field_gep(llvm::Value *struct_ptr,
                          const TypePtr &struct_sem_type,
                          const std::string &field_name) {
  if (!struct_sem_type || struct_sem_type->kind != TypeKind::Struct)
    return {nullptr, nullptr};

  auto &info = std::get<StructTypeInfo>(struct_sem_type->detail);
  auto st_it = struct_types.find(info.name);
  if (st_it == struct_types.end())
    return {nullptr, nullptr};

  auto *st = st_it->second;
  auto &fields = struct_fields[info.name];

  for (size_t i = 0; i < fields.size(); ++i) {
    if (fields[i] == field_name) {
      auto *gep = builder.CreateStructGEP(st, struct_ptr, i, field_name);
      return {gep, st->getElementType(i)};
    }
  }
  return {nullptr, nullptr};
}

llvm::Value *CodeGen::emit_selector(const SelectorNode &node,
                                    const Node &parent) {
  std::string field_name(node.field.name);

  // Emit the object expression.  For a struct variable this will be a load
  // of a pointer (from the alloca).  But we need the alloca itself to GEP.
  // Check if the object is an identifier referencing a local struct.
  if (auto *ident = std::get_if<IdentifierNode>(&node.object->data)) {
    std::string obj_name(ident->name);
    auto local_it = locals.find(obj_name);
    if (local_it != locals.end()) {
      auto *alloca = local_it->second;
      // Check if this alloca holds a struct type.
      auto sem = semantic_type(*node.object);
      if (sem && sem->kind == TypeKind::Struct) {
        auto &info = std::get<StructTypeInfo>(sem->detail);
        auto st_it = struct_types.find(info.name);
        if (st_it != struct_types.end()) {
          // The alloca might be a ptr to struct (if stored from a literal)
          // or the struct type itself.
          auto *alloca_type = alloca->getAllocatedType();
          if (alloca_type == st_it->second) {
            // Direct struct alloca — GEP into it.
            auto [gep, ftype] = struct_field_gep(alloca, sem, field_name);
            if (gep)
              return builder.CreateLoad(ftype, gep, field_name);
          } else if (alloca_type->isPointerTy()) {
            // Pointer to struct — load the pointer, then GEP.
            auto *ptr = builder.CreateLoad(alloca_type, alloca, obj_name);
            auto [gep, ftype] = struct_field_gep(ptr, sem, field_name);
            if (gep)
              return builder.CreateLoad(ftype, gep, field_name);
          }
        }
      }
    }
  }

  // Fallback: emit the object as a value (pointer), then GEP.
  auto *obj = emit_expr(*node.object);
  if (!obj)
    return nullptr;

  auto sem = semantic_type(*node.object);
  if (sem && sem->kind == TypeKind::Struct) {
    auto [gep, ftype] = struct_field_gep(obj, sem, field_name);
    if (gep)
      return builder.CreateLoad(ftype, gep, field_name);
  }

  // Enum variant access: EnumName.Variant → integer constant.
  if (sem && sem->kind == TypeKind::Enum) {
    auto &info = std::get<EnumTypeInfo>(sem->detail);
    std::string key = info.name + "." + field_name;
    auto ev_it = enum_variants.find(key);
    if (ev_it != enum_variants.end())
      return llvm::ConstantInt::get(i64_type, ev_it->second);
  }

  // Array/other built-in methods accessed via selector (handled in call).
  return nullptr;
}

// ===========================================================================
// Call expressions
// ===========================================================================

llvm::Value *CodeGen::emit_call_expr(const CallExprNode &node) {
  // Check for method calls on objects (selector calls like arr.Size()).
  if (auto *sel = std::get_if<SelectorNode>(&node.callee->data)) {
    std::string method(sel->field.name);
    auto *obj = emit_expr(*sel->object);
    if (!obj)
      return nullptr;

    auto obj_sem = semantic_type(*sel->object);

    // Array methods.
    if (obj_sem && obj_sem->kind == TypeKind::Array) {
      if (method == "Size") {
        auto *size_fn = module->getFunction("mc_array_size");
        return builder.CreateCall(size_fn, {obj}, "size");
      }
      if (method == "Push" && !node.args.empty()) {
        auto *val = emit_expr(*node.args[0]);
        if (!val) return nullptr;
        auto *func = builder.GetInsertBlock()->getParent();
        auto *tmp = create_entry_alloca(func, "push.tmp", val->getType());
        builder.CreateStore(val, tmp);
        auto *push_fn = module->getFunction("mc_array_push");
        builder.CreateCall(push_fn, {obj, tmp});
        return obj; // Push returns the array.
      }
    }

    // String methods.
    if (obj_sem && obj_sem->kind == TypeKind::String) {
      if (method == "Size") {
        // String size = load the len field from mc_string.
        auto *len_gep = builder.CreateStructGEP(string_type, obj, 1, "len.ptr");
        return builder.CreateLoad(i64_type, len_gep, "len");
      }
    }

    // Int/Float/Bool .String() method — convert to string.
    if (method == "String" && obj_sem) {
      if (obj_sem->kind == TypeKind::Int) {
        return builder.CreateCall(
            module->getFunction("mc_int_to_string"), {obj}, "str");
      }
      if (obj_sem->kind == TypeKind::Float) {
        return builder.CreateCall(
            module->getFunction("mc_float_to_string"), {obj}, "str");
      }
      if (obj_sem->kind == TypeKind::Bool) {
        auto *ext = builder.CreateZExt(obj, i64_type, "bext");
        return builder.CreateCall(
            module->getFunction("mc_bool_to_string"), {ext}, "str");
      }
    }

    // Struct method call: obj.Method(args) → StructName.Method(obj, args).
    if (obj_sem && obj_sem->kind == TypeKind::Struct) {
      auto &info = std::get<StructTypeInfo>(obj_sem->detail);
      std::string link_name = info.name + "." + method;
      auto *callee = module->getFunction(link_name);
      if (callee) {
        // Build argument list: self + explicit args.
        // For struct methods, self is a pointer to the struct.
        // obj might be a loaded pointer or a direct alloca — we need
        // the alloca (pointer) for self.
        llvm::Value *self_ptr = obj;

        // If the object is an identifier referencing a local, use the alloca.
        if (auto *id = std::get_if<IdentifierNode>(&sel->object->data)) {
          auto local_it = locals.find(std::string(id->name));
          if (local_it != locals.end()) {
            auto *alloca = local_it->second;
            auto *alloca_type = alloca->getAllocatedType();
            auto st_it = struct_types.find(info.name);
            if (st_it != struct_types.end() && alloca_type == st_it->second) {
              self_ptr = alloca; // Direct struct alloca.
            } else if (alloca_type->isPointerTy()) {
              self_ptr = builder.CreateLoad(alloca_type, alloca, "self.ptr");
            }
          }
        }

        std::vector<llvm::Value *> args;
        args.push_back(self_ptr);
        for (auto &arg_node : node.args) {
          auto *val = emit_expr(*arg_node);
          if (val)
            args.push_back(val);
        }
        if (callee->getReturnType()->isVoidTy()) {
          builder.CreateCall(callee, args);
          return nullptr;
        }
        return builder.CreateCall(callee, args, "mcall");
      }
    }

    // Interface dynamic dispatch: obj.Method(args) via vtable.
    if (obj_sem && obj_sem->kind == TypeKind::Interface) {
      auto &iface_info = std::get<InterfaceTypeInfo>(obj_sem->detail);
      std::string iface_name = iface_info.name;

      auto mn_it = iface_method_names.find(iface_name);
      auto vt_it = iface_vtable_types.find(iface_name);
      if (mn_it != iface_method_names.end() &&
          vt_it != iface_vtable_types.end()) {
        auto &methods = mn_it->second;
        auto *vtable_st = vt_it->second;

        // Find the method index.
        int method_idx = -1;
        for (size_t i = 0; i < methods.size(); ++i) {
          if (methods[i] == method) {
            method_idx = static_cast<int>(i);
            break;
          }
        }
        if (method_idx >= 0) {
          // obj is a ptr to mc_iface { ptr data, ptr vtable }.
          // Load data and vtable pointers.
          auto *ptr_type = llvm::PointerType::getUnqual(context);

          // If obj is an identifier, get the alloca for the fat pointer.
          llvm::Value *fat_ptr = obj;
          if (auto *id = std::get_if<IdentifierNode>(&sel->object->data)) {
            auto local_it = locals.find(std::string(id->name));
            if (local_it != locals.end()) {
              auto *alloca = local_it->second;
              if (alloca->getAllocatedType() == iface_fat_ptr_type) {
                fat_ptr = alloca;
              } else if (alloca->getAllocatedType()->isPointerTy()) {
                fat_ptr = builder.CreateLoad(ptr_type, alloca, "fat.load");
              }
            }
          }

          auto *data_gep = builder.CreateStructGEP(
              iface_fat_ptr_type, fat_ptr, 0, "iface.data.ptr");
          auto *data_ptr = builder.CreateLoad(ptr_type, data_gep, "data");

          auto *vtable_gep = builder.CreateStructGEP(
              iface_fat_ptr_type, fat_ptr, 1, "iface.vtable.ptr");
          auto *vtable_ptr = builder.CreateLoad(ptr_type, vtable_gep, "vtable");

          // Load the function pointer from the vtable.
          auto *fn_gep = builder.CreateStructGEP(
              vtable_st, vtable_ptr, method_idx, "vfn.ptr");
          auto *fn_ptr = builder.CreateLoad(ptr_type, fn_gep, "vfn");

          // Build the function type for the indirect call.
          // First param is the data pointer (self), then explicit args.
          std::vector<llvm::Type *> param_ll_types;
          param_ll_types.push_back(ptr_type); // self

          std::vector<llvm::Value *> args;
          args.push_back(data_ptr);

          for (auto &arg_node : node.args) {
            auto *val = emit_expr(*arg_node);
            if (val) {
              args.push_back(val);
              param_ll_types.push_back(val->getType());
            }
          }

          // Determine return type from the interface method signature.
          llvm::Type *ret_ll = void_ll_type;
          for (auto &im : iface_info.methods) {
            if (im.name == method && im.signature) {
              auto &fi = std::get<FuncTypeInfo>(im.signature->detail);
              if (!fi.returns.empty())
                ret_ll = llvm_type(fi.returns[0]);
              break;
            }
          }

          auto *fn_type = llvm::FunctionType::get(ret_ll, param_ll_types, false);

          if (ret_ll->isVoidTy()) {
            builder.CreateCall(fn_type, fn_ptr, args);
            return nullptr;
          }
          return builder.CreateCall(fn_type, fn_ptr, args, "iface.call");
        }
      }
    }

    return nullptr;
  }

  // Direct function call.
  auto *ident = std::get_if<IdentifierNode>(&node.callee->data);
  if (!ident)
    return nullptr;

  std::string name(ident->name);
  std::string link_name;
  if (name == "intrinsic_print")
    link_name = "mc_intrinsic_print";
  else
    link_name = name;

  auto *callee = module->getFunction(link_name);
  if (!callee)
    return nullptr;

  std::vector<llvm::Value *> args;
  for (auto &arg_node : node.args) {
    auto *val = emit_expr(*arg_node);
    if (val)
      args.push_back(val);
  }

  return builder.CreateCall(callee, args);
}

// ===========================================================================
// Identifier expressions
// ===========================================================================

llvm::Value *CodeGen::emit_identifier(const IdentifierNode &node) {
  std::string name(node.name);

  // Check local variables.
  auto it = locals.find(name);
  if (it != locals.end()) {
    return builder.CreateLoad(it->second->getAllocatedType(), it->second,
                              name);
  }

  // Builtin constants.
  if (name == "true")
    return llvm::ConstantInt::get(i1_type, 1);
  if (name == "false")
    return llvm::ConstantInt::get(i1_type, 0);

  // Enum type names — return a sentinel so selectors can access variants.
  if (enum_types.count(name))
    return llvm::ConstantInt::get(i64_type, 0);

  return nullptr;
}

// ===========================================================================
// Output
// ===========================================================================

// ===========================================================================
// Reference counting
// ===========================================================================

void CodeGen::track_managed(const std::string &name, const TypePtr &sem) {
  if (!sem) return;
  if (sem->kind == TypeKind::String)
    managed_locals.push_back({name, ManagedKind::String});
  else if (sem->kind == TypeKind::Array)
    managed_locals.push_back({name, ManagedKind::Array});
}

void CodeGen::emit_retain(llvm::Value *val, const TypePtr &sem) {
  if (!val || !sem) return;
  if (sem->kind == TypeKind::String) {
    builder.CreateCall(module->getFunction("mc_retain_string"), {val});
  } else if (sem->kind == TypeKind::Array) {
    builder.CreateCall(module->getFunction("mc_retain_array"), {val});
  }
}

void CodeGen::emit_release(llvm::Value *val, const TypePtr &sem) {
  if (!val || !sem) return;
  if (sem->kind == TypeKind::String) {
    builder.CreateCall(module->getFunction("mc_release_string"), {val});
  } else if (sem->kind == TypeKind::Array) {
    builder.CreateCall(module->getFunction("mc_release_array"), {val});
  }
}

void CodeGen::emit_release_locals() {
  for (auto &ml : managed_locals) {
    auto it = locals.find(ml.name);
    if (it == locals.end()) continue;
    auto *alloca = it->second;
    auto *val = builder.CreateLoad(alloca->getAllocatedType(), alloca);
    if (ml.kind == ManagedKind::String)
      builder.CreateCall(module->getFunction("mc_release_string"), {val});
    else if (ml.kind == ManagedKind::Array)
      builder.CreateCall(module->getFunction("mc_release_array"), {val});
  }
}

// ===========================================================================
// Output
// ===========================================================================

void CodeGen::dump() const { module->print(llvm::errs(), nullptr); }

bool CodeGen::write_ir(const std::string &path) const {
  std::error_code ec;
  llvm::raw_fd_ostream out(path, ec, llvm::sys::fs::OF_Text);
  if (ec)
    return false;
  module->print(out, nullptr);
  return true;
}

bool CodeGen::write_object(const std::string &path) {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();

  auto target_triple = llvm::sys::getDefaultTargetTriple();
  module->setTargetTriple(target_triple);

  std::string error_str;
  auto *target =
      llvm::TargetRegistry::lookupTarget(target_triple, error_str);
  if (!target)
    return false;

  auto *target_machine = target->createTargetMachine(
      target_triple, "generic", "", llvm::TargetOptions{},
      std::nullopt);
  module->setDataLayout(target_machine->createDataLayout());

  std::error_code ec;
  llvm::raw_fd_ostream out(path, ec, llvm::sys::fs::OF_None);
  if (ec)
    return false;

  llvm::legacy::PassManager pass;
  if (target_machine->addPassesToEmitFile(
          pass, out, nullptr, llvm::CodeGenFileType::ObjectFile))
    return false;

  pass.run(*module);
  out.flush();
  return true;
}

} // namespace mc
