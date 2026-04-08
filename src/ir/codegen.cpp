// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"

#include <charconv>

#include <llvm/IR/Constants.h>
#include <llvm/IR/InlineAsm.h>
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

bool CodeGen::is_string_key_type(const TypePtr &t) {
  return t && t->kind == TypeKind::String;
}

// ===========================================================================
// Construction
// ===========================================================================

CodeGen::CodeGen(const std::string &module_name, Analyzer &analyzer)
    : module(std::make_unique<llvm::Module>(module_name, context)),
      builder(context),
      analyzer(analyzer),
      package_name(module_name) {
  // Set a default data layout so LLVM can compute type alignments.
  // This is overridden by write_object() with the actual target layout.
  module->setDataLayout("e-m:e-p270:32:32-p271:32:32-p272:64:64-"
                        "i64:64-i128:128-f80:128-n8:16:32:64-S128");
  init_types();
  declare_runtime();
}

// ===========================================================================
// Symbol mangling
// ===========================================================================

std::string CodeGen::mangle(const std::string &name) const {
  return mangle(package_name, name);
}

std::string CodeGen::mangle(const std::string &pkg, const std::string &name) {
  return pkg + "__" + name;
}

llvm::Function *CodeGen::declare_import(const std::string &pkg_name,
                                         const std::string &symbol_name,
                                         const TypePtr &func_type) {
  std::string link_name = mangle(pkg_name, symbol_name);

  // Return existing declaration if already present.
  if (auto *existing = module->getFunction(link_name))
    return existing;

  // Build the LLVM function type from the semantic type.
  auto &fi = std::get<FuncTypeInfo>(func_type->detail);

  std::vector<llvm::Type *> param_types;
  for (auto &p : fi.params)
    param_types.push_back(llvm_type(p));

  llvm::Type *ret_ll = void_ll_type;
  if (!fi.returns.empty() && fi.returns[0]->kind != TypeKind::Void) {
    if (fi.returns.size() == 1) {
      ret_ll = llvm_type(fi.returns[0]);
    } else {
      // Multi-return: create a struct type.
      std::vector<llvm::Type *> ret_types;
      for (auto &r : fi.returns)
        ret_types.push_back(llvm_type(r));
      auto *st = llvm::StructType::create(context, ret_types,
                                           "mc.ret." + link_name);
      multi_return_types[link_name] = st;
      multi_return_counts[link_name] = fi.returns.size();
      ret_ll = st;
    }
  }

  auto *fn_type = llvm::FunctionType::get(ret_ll, param_types, /*isVarArg=*/false);
  auto *func = llvm::Function::Create(
      fn_type, llvm::Function::ExternalLinkage, link_name, module.get());
  return func;
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

  // Closure fat pointer: { ptr fn, ptr env }
  closure_fat_ptr_type = llvm::StructType::create(
      context, {ptr_ty, ptr_ty}, "mc_closure");

  // Register built-in enums.
  enum_types["Comparison"] = true;
  enum_variants["Comparison.Less"] = 0;
  enum_variants["Comparison.Equal"] = 1;
  enum_variants["Comparison.Greater"] = 2;
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
  case TypeKind::Union: {
    auto *st = get_union_llvm_type(t);
    if (st)
      return st;
    return void_ll_type;
  }
  case TypeKind::Array:
    return llvm::PointerType::getUnqual(context); // ptr to mc_array
  case TypeKind::Map:
    return llvm::PointerType::getUnqual(context); // ptr to mc_map
  case TypeKind::Func:
    return llvm::PointerType::getUnqual(context); // ptr to mc_closure
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
  // Pass 1: register all type declarations (structs, enums, interfaces)
  // across every source file before emitting any function bodies. This
  // ensures cross-file references within the same package resolve correctly.
  for (auto &src : pkg.sources) {
    auto &s = std::get<SourceNode>(src->data);
    declare_structs(s);
    declare_enums(s);
    declare_interfaces(s);
  }

  // Pass 2: emit each source file (functions, methods, etc.).
  for (auto &src : pkg.sources)
    emit_source(std::get<SourceNode>(src->data));
}

/// Pre-scan declarations for spawn expressions to set has_spawn early.
static bool source_has_spawn(const Node &node) {
  bool found = false;
  auto visitor = [&](const auto &self, const Node &n) -> void {
    if (found) return;
    std::visit(overloaded{
        [&](const SpawnExprNode &) { found = true; },
        [&](const BlockNode &b) {
          for (auto &s : b.stmts) self(self, *s);
        },
        [&](const FuncDeclNode &fn) {
          if (fn.body) self(self, *fn.body);
        },
        [&](const SourceNode &s) {
          for (auto &d : s.declarations) self(self, *d);
        },
        [&](const DeclAssignNode &d) { self(self, *d.value); },
        [&](const VarDeclNode &d) { if (d.init) self(self, **d.init); },
        [&](const IfExprNode &e) {
          self(self, *e.then_block);
          if (e.else_block) self(self, **e.else_block);
        },
        [&](const ForExprNode &e) { self(self, *e.body); },
        [&](const auto &) {},
    }, n.data);
  };
  visitor(visitor, node);
  return found;
}

void CodeGen::emit_source(const SourceNode &src) {
  // Pre-scan for spawn expressions so executor init/shutdown can be placed.
  if (!has_spawn) {
    // Build a temporary Node wrapping the SourceNode for the visitor.
    // Instead, scan declarations directly.
    for (auto &decl : src.declarations) {
      if (source_has_spawn(*decl)) {
        has_spawn = true;
        break;
      }
    }
  }

  // Pass 1: create LLVM struct types, register enums and interfaces.
  declare_structs(src);
  declare_enums(src);
  declare_interfaces(src);

  // Pass 2: forward-declare all functions and struct methods.
  declare_functions(src);
  declare_struct_methods(src);

  // Pass 2b: emit package-level constants as globals.
  for (auto &decl : src.declarations) {
    std::visit(
        overloaded{
            [&](const ConstDeclNode &c) { emit_const_decl(c); },
            [&](const auto &) {},
        },
        decl->data);
  }

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
