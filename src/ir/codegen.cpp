// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"

#include <charconv>

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
// Forward-declared helpers
// ===========================================================================

/// Helper: determine if a semantic type represents string keys (for mc_map).
static bool is_string_key_type(const TypePtr &t) {
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

  auto *fn_type = llvm::FunctionType::get(ret_ll, param_types, fi.is_variadic);
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

  // mc_map* mc_map_new(i64 key_size, i64 val_size)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {i64_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "mc_map_new", module.get());

  // void mc_map_set(mc_map* m, void* key, void* value, i64 is_string_key)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type, ptr_type, ptr_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "mc_map_set", module.get());

  // void* mc_map_get(mc_map* m, void* key, i64 is_string_key)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type, ptr_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "mc_map_get", module.get());

  // i64 mc_map_has(mc_map* m, void* key, i64 is_string_key)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type, ptr_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "mc_map_has", module.get());

  // void mc_map_remove(mc_map* m, void* key, i64 is_string_key)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type, ptr_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "mc_map_remove", module.get());

  // i64 mc_map_size(mc_map* m)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_map_size", module.get());

  // void* mc_map_key_at(mc_map* m, i64 index)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "mc_map_key_at", module.get());

  // void* mc_map_value_at(mc_map* m, i64 index)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "mc_map_value_at", module.get());

  // void mc_retain_map(mc_map* m)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_retain_map", module.get());

  // void mc_release_map(mc_map* m)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_release_map", module.get());

  // ── Spawn / Actor runtime functions ────────────────────────────────

  // void mc_executor_init(i64 num_workers)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {i64_type}, false),
      llvm::Function::ExternalLinkage, "mc_executor_init", module.get());

  // void mc_executor_shutdown()
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {}, false),
      llvm::Function::ExternalLinkage, "mc_executor_shutdown", module.get());

  // mc_actor* mc_executor_spawn(void(*entry)(mc_actor*), void* closure,
  //                             i64 closure_size, i64 arena_max)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type,
                              {ptr_type, ptr_type, i64_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "mc_executor_spawn", module.get());

  // void mc_executor_schedule(mc_actor* actor)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_executor_schedule", module.get());

  // mc_channel* mc_channel_new(i64 elem_size, i64 capacity)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {i64_type, i64_type}, false),
      llvm::Function::ExternalLinkage, "mc_channel_new", module.get());

  // int mc_channel_recv(mc_channel* ch, void* out_buf)
  llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt32Ty(context),
                              {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_channel_recv", module.get());

  // void mc_channel_close(mc_channel* ch)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_channel_close", module.get());

  // void mc_channel_destroy(mc_channel* ch)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_channel_destroy", module.get());

  // i64 mc_task_alive(mc_actor* a)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_task_alive", module.get());

  // void mc_task_cancel(mc_actor* a)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_task_cancel", module.get());

  // void mc_task_term(mc_actor* a)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_task_term", module.get());

  // void* mc_task_wait(mc_actor* a, i64* out_status)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_task_wait", module.get());

  // void mc_task_drop(mc_actor* a)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_task_drop", module.get());

  // i64 mc_context_cancelled(mc_actor* a)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_context_cancelled", module.get());

  // void mc_context_exit(mc_actor* a, void* value, i64 size)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type, ptr_type, i64_type},
                              false),
      llvm::Function::ExternalLinkage, "mc_context_exit", module.get());

  // int mc_context_send(mc_actor* a, void* data)
  llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getInt32Ty(context),
                              {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_context_send", module.get());

  // void mc_reduction_tick(mc_actor* a)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_reduction_tick", module.get());

  // void mc_actor_yield(mc_actor* a)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_actor_yield", module.get());

  // void mc_actor_trap(mc_actor* a, mc_string* reason)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_actor_trap", module.get());
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
// Function forward-declarations
// ===========================================================================

void CodeGen::declare_functions(const SourceNode &src) {
  for (auto &decl : src.declarations) {
    if (auto *fn = std::get_if<FuncDeclNode>(&decl->data)) {
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
  auto sem_type = analyzer.node_types.count(&*node.value)
                      ? analyzer.node_types.at(&*node.value)
                      : nullptr;
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
    std::string link_name = mangle(struct_name + "__" + iface_method);
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
  std::string link_name = is_main ? "main" : mangle(std::string(fn.name.name));

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
  std::string link_name = is_main ? "main" : mangle(name);

  auto *func = module->getFunction(link_name);
  if (!func)
    return; // Should have been forward-declared.

  auto *entry = llvm::BasicBlock::Create(context, "entry", func);
  builder.SetInsertPoint(entry);

  // Reset per-function state.
  locals.clear();
  managed_locals.clear();
  current_func_is_main = is_main;

  // If this is Main and we have spawn expressions, init the executor.
  if (is_main && has_spawn) {
    builder.CreateCall(module->getFunction("mc_executor_init"),
                       {llvm::ConstantInt::get(i64_type, 0)});
  }

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
      if (has_spawn)
        builder.CreateCall(module->getFunction("mc_executor_shutdown"), {});
      builder.CreateRet(
          llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0));
    } else if (ret_type->isVoidTy()) {
      builder.CreateRetVoid();
    } else if (tail_val && tail_val->getType() == ret_type) {
      builder.CreateRet(tail_val);
    } else if (tail_val && ret_type->isStructTy() &&
               tail_val->getType()->isPointerTy()) {
      // Union tail: pointer to union struct — load and return.
      auto *st = llvm::cast<llvm::StructType>(ret_type);
      if (st->getNumElements() == 2 &&
          st->getElementType(0)->isIntegerTy(8) &&
          st->getElementType(1)->isArrayTy()) {
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
    // Check node_types first (recorded during analysis), then fallback.
    auto nt_it = analyzer.node_types.find(&**node.type);
    if (nt_it != analyzer.node_types.end()) {
      var_type = llvm_type(nt_it->second);
    } else {
      auto sem_type = analyzer.resolve_type(**node.type);
      var_type = llvm_type(sem_type);
    }
  } else if (node.init) {
    // Infer from the init expression's semantic type.
    auto it = analyzer.node_types.find(&**node.init);
    if (it != analyzer.node_types.end())
      var_type = llvm_type(it->second);
  }

  // Determine semantic type for refcount tracking and interface boxing.
  TypePtr sem_type_ptr = nullptr;
  if (node.type) {
    // Look up from the node_types map first (recorded during analysis).
    auto it = analyzer.node_types.find(&**node.type);
    if (it != analyzer.node_types.end()) {
      sem_type_ptr = it->second;
    } else {
      // Fall back to resolve_type (works for builtins).
      sem_type_ptr = analyzer.resolve_type(**node.type);
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

    // Union boxing: declared type is a union, init is a concrete type.
    if (val && sem_type_ptr && sem_type_ptr->kind == TypeKind::Union) {
      auto init_sem = semantic_type(**node.init);
      if (init_sem && init_sem->kind != TypeKind::Union) {
        auto *wrapped = emit_union_wrap(val, init_sem, sem_type_ptr);
        if (wrapped && llvm::isa<llvm::AllocaInst>(wrapped)) {
          auto *alloca = llvm::cast<llvm::AllocaInst>(wrapped);
          alloca->setName(name);
          locals[name] = alloca;
          track_managed(name, sem_type_ptr);
          return;
        }
      }
      // If the init already produces a union (e.g. from a call), alias it.
      if (init_sem && init_sem->kind == TypeKind::Union &&
          val && llvm::isa<llvm::AllocaInst>(val)) {
        auto *alloca = llvm::cast<llvm::AllocaInst>(val);
        alloca->setName(name);
        locals[name] = alloca;
        track_managed(name, sem_type_ptr);
        return;
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

    // If the init produces a union alloca, alias it.
    if (val && llvm::isa<llvm::AllocaInst>(val)) {
      auto init_sem = semantic_type(**node.init);
      if (init_sem && init_sem->kind == TypeKind::Union) {
        auto *alloca = llvm::cast<llvm::AllocaInst>(val);
        alloca->setName(name);
        locals[name] = alloca;
        track_managed(name, sem_type_ptr);
        return;
      }
    }

    // If the init is a closure alloca, alias it directly.
    if (val && llvm::isa<llvm::AllocaInst>(val)) {
      auto *alloca = llvm::cast<llvm::AllocaInst>(val);
      if (alloca->getAllocatedType() == closure_fat_ptr_type) {
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
    // Zero-initialize with proper language zero values.
    // The language specifies: Int=0, Float=0.0, Bool=false, String="",
    // [T]=[], {K:V}={}, Struct=all-fields-zero.
    if (sem_type_ptr && sem_type_ptr->kind == TypeKind::String) {
      // String zero value: empty string ""
      auto *empty_str = make_string_constant("");
      auto *alloca = create_entry_alloca(func, name, var_type);
      locals[name] = alloca;
      builder.CreateStore(empty_str, alloca);
    } else if (sem_type_ptr && sem_type_ptr->kind == TypeKind::Array) {
      // Array zero value: empty array []
      auto &arr_info = std::get<ArrayTypeInfo>(sem_type_ptr->detail);
      int64_t elem_size = 8; // default
      if (arr_info.element) {
        auto *elem_ll = llvm_type(arr_info.element);
        if (elem_ll->isIntegerTy(1))
          elem_size = 1;
        else if (elem_ll->isDoubleTy() || elem_ll->isIntegerTy(64) ||
                 elem_ll->isPointerTy())
          elem_size = 8;
      }
      auto *new_fn = module->getFunction("mc_array_new");
      auto *arr = builder.CreateCall(
          new_fn,
          {llvm::ConstantInt::get(i64_type, elem_size),
           llvm::ConstantInt::get(i64_type, 4)},
          "arr");
      auto *alloca = create_entry_alloca(func, name, var_type);
      locals[name] = alloca;
      builder.CreateStore(arr, alloca);
    } else if (sem_type_ptr && sem_type_ptr->kind == TypeKind::Map) {
      // Map zero value: empty map {}
      auto &map_info = std::get<MapTypeInfo>(sem_type_ptr->detail);
      int64_t key_size = 8;
      int64_t val_size = 8;
      bool string_keys = is_string_key_type(map_info.key);
      if (map_info.key) {
        auto *key_ll = llvm_type(map_info.key);
        if (key_ll->isIntegerTy(1))
          key_size = 1;
      }
      if (map_info.value) {
        auto *val_ll = llvm_type(map_info.value);
        if (val_ll->isIntegerTy(1))
          val_size = 1;
      }
      auto *new_fn = module->getFunction("mc_map_new");
      int64_t key_size_arg = string_keys ? -1 : key_size;
      auto *map = builder.CreateCall(
          new_fn,
          {llvm::ConstantInt::get(i64_type, key_size_arg),
           llvm::ConstantInt::get(i64_type, val_size)},
          "map");
      auto *alloca = create_entry_alloca(func, name, var_type);
      locals[name] = alloca;
      builder.CreateStore(map, alloca);
    } else if (sem_type_ptr && sem_type_ptr->kind == TypeKind::Struct) {
      // Struct zero value: allocate struct, zero-initialize all fields.
      auto &info = std::get<StructTypeInfo>(sem_type_ptr->detail);
      auto st_it = struct_types.find(info.name);
      if (st_it != struct_types.end()) {
        auto *st_type = st_it->second;
        auto *alloca = create_entry_alloca(func, name, st_type);
        locals[name] = alloca;
        builder.CreateStore(llvm::Constant::getNullValue(st_type), alloca);
      } else {
        auto *alloca = create_entry_alloca(func, name, var_type);
        locals[name] = alloca;
        builder.CreateStore(llvm::Constant::getNullValue(var_type), alloca);
      }
    } else {
      // Scalar types (Int, Float, Bool, Enum, etc.): getNullValue is correct.
      auto *alloca = create_entry_alloca(func, name, var_type);
      locals[name] = alloca;
      builder.CreateStore(llvm::Constant::getNullValue(var_type), alloca);
    }
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
    std::string link_name = (callee_name == "Main") ? "main" : mangle(callee_name);
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

    // If the value is a struct, union, or closure alloca, alias it directly.
    if (val && llvm::isa<llvm::AllocaInst>(val)) {
      auto *alloca = llvm::cast<llvm::AllocaInst>(val);
      auto sem = semantic_type(*node.value);
      if (sem && (sem->kind == TypeKind::Struct ||
                  sem->kind == TypeKind::Union)) {
        alloca->setName(name);
        locals[name] = alloca;
        continue;
      }
      // Closure fat pointer — alias directly.
      if (alloca->getAllocatedType() == closure_fat_ptr_type) {
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

    // If a pending channel alloca exists from a spawn expression,
    // create a companion local "<name>.channel" for for-range iteration.
    if (pending_channel_alloca_) {
      std::string ch_name = name + ".channel";
      locals[ch_name] = pending_channel_alloca_;
      pending_channel_alloca_ = nullptr;
    }
  }
}

void CodeGen::emit_assign(const AssignNode &node) {
  for (size_t i = 0; i < node.targets.size() && i < node.values.size(); ++i) {
    auto *rhs = emit_expr(*node.values[i]);
    if (!rhs)
      continue;

    // Target can be an identifier, selector, or index expression.
    if (auto *idx_expr = std::get_if<IndexExprNode>(&node.targets[i]->data)) {
      // Index assignment: obj[key] = rhs
      auto *obj = emit_expr(*idx_expr->object);
      auto *key = emit_expr(*idx_expr->index);
      if (!obj || !key)
        continue;

      auto obj_sem = semantic_type(*idx_expr->object);
      if (obj_sem && obj_sem->kind == TypeKind::Map) {
        auto &map_info = std::get<MapTypeInfo>(obj_sem->detail);
        bool string_keys = is_string_key_type(map_info.key);
        auto *is_str_key = llvm::ConstantInt::get(i64_type, string_keys ? 1 : 0);

        auto *func = builder.GetInsertBlock()->getParent();
        auto *key_tmp = create_entry_alloca(func, "map.asgn.key", key->getType());
        builder.CreateStore(key, key_tmp);
        auto *val_tmp = create_entry_alloca(func, "map.asgn.val", rhs->getType());
        builder.CreateStore(rhs, val_tmp);

        auto *set_fn = module->getFunction("mc_map_set");
        builder.CreateCall(set_fn, {obj, key_tmp, val_tmp, is_str_key});
      } else if (obj_sem && obj_sem->kind == TypeKind::Array) {
        // Array index assignment: arr[idx] = rhs
        // TODO: implement mc_array_set when available
      }
      continue;
    }

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
                         target_sem->kind == TypeKind::Array ||
                         target_sem->kind == TypeKind::Map)) {
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
    if (has_spawn)
      builder.CreateCall(module->getFunction("mc_executor_shutdown"), {});
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
    auto *func = builder.GetInsertBlock()->getParent();
    auto *ret_type = func->getReturnType();

    // Handle union return types: wrap concrete values or load from alloca.
    if (val && ret_type->isStructTy() && val->getType()->isPointerTy()) {
      auto *st = llvm::cast<llvm::StructType>(ret_type);
      // Check if return type is a union struct: { i8, [N x i8] }
      if (st->getNumElements() == 2 &&
          st->getElementType(0)->isIntegerTy(8) &&
          st->getElementType(1)->isArrayTy()) {
        // val is a pointer to the union alloca — load the struct value.
        if (auto *ai = llvm::dyn_cast<llvm::AllocaInst>(val)) {
          if (ai->getAllocatedType() == ret_type) {
            val = builder.CreateLoad(ret_type, val, "ret.union");
          }
        }
      }
    }
    // If val is a concrete value but ret_type is a union struct, wrap it.
    if (val && ret_type->isStructTy() && !val->getType()->isStructTy()) {
      auto *st = llvm::cast<llvm::StructType>(ret_type);
      if (st->getNumElements() == 2 &&
          st->getElementType(0)->isIntegerTy(8) &&
          st->getElementType(1)->isArrayTy()) {
        // Need to find the semantic return type and value type.
        auto val_sem = semantic_type(*node.values[0]);
        // Look up the function's semantic return type from the scope.
        TypePtr ret_sem = nullptr;
        for (auto &[key, union_st] : union_llvm_types) {
          if (union_st == st) {
            // Reconstruct semantic type is complex; use the analyzer's
            // return types from the current scope instead.
            break;
          }
        }
        // Use the analyzer's scope to get return types.
        if (!ret_sem && !analyzer.current_scope->return_types.empty()) {
          ret_sem = analyzer.current_scope->return_types[0];
        }
        if (val_sem && ret_sem && ret_sem->kind == TypeKind::Union) {
          auto *wrapped = emit_union_wrap(val, val_sem, ret_sem);
          if (wrapped)
            val = builder.CreateLoad(ret_type, wrapped, "ret.union");
        }
      }
    }

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
            return emit_binary_expr(n, node);
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
          [&](const MapLiteralNode &n) -> llvm::Value * {
            return emit_map_literal(n);
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
          [&](const OrExprNode &n) -> llvm::Value * {
            return emit_or_expr(n);
          },
          [&](const CallExprNode &n) -> llvm::Value * {
            return emit_call_expr(n);
          },
          [&](const FuncExprNode &n) -> llvm::Value * {
            return emit_func_expr(n, node);
          },
          [&](const SpawnExprNode &n) -> llvm::Value * {
            return emit_spawn_expr(n, node);
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

// ===========================================================================
// Struct operator overloading
// ===========================================================================

llvm::Value *CodeGen::emit_struct_binary_op(const BinaryExprNode &node,
                                             const Node &parent,
                                             const TypePtr &lhs_sem,
                                             const std::string &method) {
  auto &info = std::get<StructTypeInfo>(lhs_sem->detail);
  auto *ptr_type = llvm::PointerType::getUnqual(context);

  // ── Resolve the mangled link name for the method ──────────────────────────
  std::string link_name;
  {
    auto ml_it = struct_method_links.find(info.name);
    if (ml_it != struct_method_links.end()) {
      for (auto &[lname, mname] : ml_it->second) {
        if (mname == method) {
          link_name = lname;
          break;
        }
      }
    }
    // If not found in links (e.g. cross-package), fall back to current-package
    // mangling so the linker can resolve it.
    if (link_name.empty())
      link_name = mangle(info.name + "__" + method);
  }

  // ── Find or forward-declare the LLVM function ─────────────────────────────
  auto *callee = module->getFunction(link_name);
  if (!callee) {
    // Determine the return LLVM type from the method name.
    llvm::Type *ret_ll;
    if (method == "Compare") {
      ret_ll = i64_type; // Comparison enum
    } else if (method == "Equals" || method == "Equal") {
      ret_ll = i1_type; // Bool
    } else if (method == "Div") {
      // Div returns T | Error; we return the union struct ptr.
      auto union_sem =
          make_union_type({lhs_sem, analyzer.builtins.error_iface});
      auto *union_st = get_union_llvm_type(union_sem);
      ret_ll = union_st ? static_cast<llvm::Type *>(union_st) : ptr_type;
    } else {
      // Add, Sub, Mul: returns same struct type as self.
      auto st_it = struct_types.find(info.name);
      ret_ll = (st_it != struct_types.end())
                   ? static_cast<llvm::Type *>(st_it->second)
                   : ptr_type;
    }

    // Determine the RHS parameter type.
    auto rhs_sem = semantic_type(*node.rhs);
    llvm::Type *rhs_ll;
    if (rhs_sem && rhs_sem->kind == TypeKind::Struct) {
      auto rhs_st_it = struct_types.find(
          std::get<StructTypeInfo>(rhs_sem->detail).name);
      rhs_ll = (rhs_st_it != struct_types.end())
                   ? static_cast<llvm::Type *>(rhs_st_it->second)
                   : ptr_type;
    } else {
      rhs_ll = rhs_sem ? llvm_type(rhs_sem) : ptr_type;
    }

    auto *fn_type =
        llvm::FunctionType::get(ret_ll, {ptr_type, rhs_ll}, false);
    callee = llvm::Function::Create(
        fn_type, llvm::Function::ExternalLinkage, link_name, module.get());
  }
  if (!callee)
    return nullptr;

  // ── Build self_ptr for the LHS ─────────────────────────────────────────────
  // Prefer passing the alloca directly so the method gets a mutable ptr.
  llvm::Value *self_ptr = nullptr;
  if (auto *id = std::get_if<IdentifierNode>(&node.lhs->data)) {
    auto local_it = locals.find(std::string(id->name));
    if (local_it != locals.end()) {
      auto *alloca = local_it->second;
      auto st_it = struct_types.find(info.name);
      if (st_it != struct_types.end() &&
          alloca->getAllocatedType() == st_it->second) {
        self_ptr = alloca; // direct struct alloca — ideal
      }
    }
  }
  if (!self_ptr) {
    // Emit the expression and spill to a temp alloca.
    auto *lhs_val = emit_expr(*node.lhs);
    if (!lhs_val)
      return nullptr;
    auto st_it = struct_types.find(info.name);
    if (st_it != struct_types.end() &&
        lhs_val->getType() == st_it->second) {
      auto *func = builder.GetInsertBlock()->getParent();
      auto *tmp =
          create_entry_alloca(func, "op.self.tmp", st_it->second);
      builder.CreateStore(lhs_val, tmp);
      self_ptr = tmp;
    } else {
      self_ptr = lhs_val; // already a pointer
    }
  }

  // ── Emit the RHS argument ────────────────────────────────────────────────
  auto *rhs_val = emit_expr(*node.rhs);
  if (!rhs_val)
    return nullptr;

  // If the RHS is a struct value (not a pointer), spill it too.
  {
    auto rhs_sem = semantic_type(*node.rhs);
    if (rhs_sem && rhs_sem->kind == TypeKind::Struct) {
      auto &rinfo = std::get<StructTypeInfo>(rhs_sem->detail);
      auto st_it = struct_types.find(rinfo.name);
      if (st_it != struct_types.end() &&
          rhs_val->getType() == st_it->second) {
        auto *func = builder.GetInsertBlock()->getParent();
        auto *tmp =
            create_entry_alloca(func, "op.rhs.tmp", st_it->second);
        builder.CreateStore(rhs_val, tmp);
        rhs_val = tmp;
      }
    }
  }

  // ── Call the method ─────────────────────────────────────────────────────────
  auto *result = builder.CreateCall(callee, {self_ptr, rhs_val}, "op.res");

  // ── Post-process result based on the operator and method ────────────────
  using K = Token::Kind;

  if (method == "Compare") {
    // Compare returns Comparison enum: Less=0, Equal=1, Greater=2.
    auto *zero = llvm::ConstantInt::get(i64_type, 0); // Less
    auto *one  = llvm::ConstantInt::get(i64_type, 1); // Equal
    auto *two  = llvm::ConstantInt::get(i64_type, 2); // Greater
    switch (node.op) {
    case K::LessThan:
      return builder.CreateICmpEQ(result, zero, "lt");
    case K::LessThanEqual:
      // Less or Equal ⇔ result != Greater
      return builder.CreateICmpNE(result, two, "le");
    case K::GreaterThan:
      return builder.CreateICmpEQ(result, two, "gt");
    case K::GreaterThanEqual:
      // Greater or Equal ⇔ result != Less
      return builder.CreateICmpNE(result, zero, "ge");
    case K::Equal:
      return builder.CreateICmpEQ(result, one, "eq");
    case K::NotEqual:
      return builder.CreateICmpNE(result, one, "ne");
    default:
      return result;
    }
  }

  // Equals / Equal return Bool (i1). Negate for !=.
  if ((method == "Equals" || method == "Equal") && node.op == K::NotEqual)
    return builder.CreateNot(result, "ne");

  // Add, Sub, Mul, Div: result is already the correct type.
  return result;
}

llvm::Value *CodeGen::emit_binary_expr(const BinaryExprNode &node,
                                        const Node &parent) {
  // Check semantic types to detect string operations.
  auto lhs_sem = semantic_type(*node.lhs);
  bool is_string = lhs_sem && lhs_sem->kind == TypeKind::String;

  // ── Struct operator overloading ────────────────────────────────────────────
  if (lhs_sem && lhs_sem->kind == TypeKind::Struct) {
    auto it = analyzer.struct_operator_methods.find(&parent);
    if (it != analyzer.struct_operator_methods.end())
      return emit_struct_binary_op(node, parent, lhs_sem, it->second);
  }

  // ── Type matching on union types ─────────────────────────────────────
  // Pattern: `union_value == TypeName` → compare the tag byte.
  using K = Token::Kind;
  if (lhs_sem && lhs_sem->kind == TypeKind::Union &&
      (node.op == K::Equal || node.op == K::NotEqual)) {
    if (auto *rhs_ident = std::get_if<IdentifierNode>(&node.rhs->data)) {
      // Check if RHS is a type name by looking at the analyzer's symbol.
      auto rhs_sym_it = analyzer.node_symbols.find(node.rhs.get());
      bool is_type_sym = false;
      if (rhs_sym_it != analyzer.node_symbols.end() &&
          rhs_sym_it->second.kind == SymbolKind::Type) {
        is_type_sym = true;
      }
      // Also check via name lookup for built-in types.
      if (!is_type_sym) {
        auto sym = analyzer.lookup(std::string(rhs_ident->name));
        if (sym && sym->kind == SymbolKind::Type)
          is_type_sym = true;
      }
      if (is_type_sym) {
        auto rhs_sem = semantic_type(*node.rhs);
        int tag = union_tag_for_type(rhs_sem, lhs_sem);
        if (tag >= 0) {
          auto *lhs_val = emit_expr(*node.lhs);
          if (!lhs_val) return nullptr;

          // lhs_val is a pointer to the union alloca.
          auto *union_st = get_union_llvm_type(lhs_sem);
          auto *tag_gep = builder.CreateStructGEP(union_st, lhs_val, 0,
                                                   "match.tag.ptr");
          auto *tag_val = builder.CreateLoad(
              llvm::Type::getInt8Ty(context), tag_gep, "match.tag");
          auto *tag_const = llvm::ConstantInt::get(
              llvm::Type::getInt8Ty(context), tag);

          if (node.op == K::Equal)
            return builder.CreateICmpEQ(tag_val, tag_const, "type.eq");
          else
            return builder.CreateICmpNE(tag_val, tag_const, "type.ne");
        }
      }
    }
  }

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
    case K::Divide: {
      auto *result = builder.CreateFDiv(lhs, rhs, "fdiv");
      auto node_sem = semantic_type(parent);
      if (node_sem && node_sem->kind == TypeKind::Union) {
        auto val_t = analyzer.builtins.float_type;
        return emit_union_wrap(result, val_t, node_sem);
      }
      return result;
    }
    case K::Modulo:   return builder.CreateFRem(lhs, rhs, "fmod");
    default: break;
    }
  }

  switch (node.op) {
  // ── Integer arithmetic ─────────────────────────────────────────────
  case K::Add:      return builder.CreateAdd(lhs, rhs, "add");
  case K::Sub:      return builder.CreateSub(lhs, rhs, "sub");
  case K::Multiply: return builder.CreateMul(lhs, rhs, "mul");
  case K::Divide: {
    auto *result = builder.CreateSDiv(lhs, rhs, "div");
    auto node_sem = semantic_type(parent);
    if (node_sem && node_sem->kind == TypeKind::Union) {
      auto val_t = analyzer.builtins.int_type;
      return emit_union_wrap(result, val_t, node_sem);
    }
    return result;
  }
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

  // ── Detect type-matching pattern for narrowing ─────────────────────
  // If condition is `value == TypeName`, extract the narrowed value.
  std::string narrowed_var_name;
  llvm::AllocaInst *saved_alloca = nullptr;
  if (auto *binop = std::get_if<BinaryExprNode>(&node.condition->data)) {
    if (binop->op == Token::Kind::Equal) {
      if (auto *lhs_id = std::get_if<IdentifierNode>(&binop->lhs->data)) {
        auto lhs_sem = semantic_type(*binop->lhs);
        if (lhs_sem && lhs_sem->kind == TypeKind::Union) {
          auto rhs_sem = semantic_type(*binop->rhs);
          if (rhs_sem) {
            narrowed_var_name = std::string(lhs_id->name);
          }
        }
      }
    }
  }

  // ── Then block ─────────────────────────────────────────────────────
  builder.SetInsertPoint(then_bb);

  // If type-matching, narrow the variable by extracting from the union.
  if (!narrowed_var_name.empty()) {
    auto local_it = locals.find(narrowed_var_name);
    if (local_it != locals.end()) {
      auto lhs_sem = semantic_type(*std::get<BinaryExprNode>(
          node.condition->data).lhs);
      auto rhs_sem = semantic_type(*std::get<BinaryExprNode>(
          node.condition->data).rhs);
      if (lhs_sem && rhs_sem) {
        auto *union_ptr = local_it->second;
        auto *extracted = emit_union_extract(union_ptr, rhs_sem, lhs_sem);
        if (extracted) {
          auto *ll_type = llvm_type(rhs_sem);
          auto *narrowed_alloca = create_entry_alloca(
              func, narrowed_var_name + ".narrowed", ll_type);
          builder.CreateStore(extracted, narrowed_alloca);
          // Temporarily replace the local.
          saved_alloca = local_it->second;
          locals[narrowed_var_name] = narrowed_alloca;
        }
      }
    }
  }

  auto &then_block = std::get<BlockNode>(node.then_block->data);
  auto *then_val = emit_block(then_block);

  // Restore the original alloca after the then-block.
  if (saved_alloca) {
    locals[narrowed_var_name] = saved_alloca;
  }

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
  bool is_union = subject_sem && subject_sem->kind == TypeKind::Union;
  bool is_int_like = !is_string && !is_union;

  auto *func = builder.GetInsertBlock()->getParent();
  auto *merge_bb = llvm::BasicBlock::Create(context, "sw.merge");

  // Collect case info for building PHI nodes.
  struct CaseResult {
    llvm::Value *value;
    llvm::BasicBlock *block;
    bool terminated;
  };
  std::vector<CaseResult> case_results;

  if (is_union) {
    // ── Union type matching: switch on the tag byte ─────────────────
    auto *union_st = get_union_llvm_type(subject_sem);
    llvm::Value *union_ptr = subject_val;

    auto *tag_gep = builder.CreateStructGEP(union_st, union_ptr, 0,
                                             "sw.union.tag.ptr");
    auto *tag_val = builder.CreateLoad(llvm::Type::getInt8Ty(context),
                                        tag_gep, "sw.union.tag");
    auto *i8_ty = llvm::Type::getInt8Ty(context);

    auto *default_bb = llvm::BasicBlock::Create(context, "sw.default");
    auto *sw = builder.CreateSwitch(tag_val, default_bb, node.arms.size());

    // Determine the subject variable name for narrowing.
    std::string subject_var;
    if (auto *id = std::get_if<IdentifierNode>(&node.subject->data))
      subject_var = std::string(id->name);

    for (size_t i = 0; i < node.arms.size(); ++i) {
      auto &arm = node.arms[i];
      auto *case_bb = llvm::BasicBlock::Create(context,
          "sw.case." + std::to_string(i), func);

      // Resolve the pattern's semantic type (should be a type name).
      auto pattern_sem = semantic_type(*arm.pattern);
      int tag = -1;
      if (pattern_sem)
        tag = union_tag_for_type(pattern_sem, subject_sem);
      if (tag >= 0)
        sw->addCase(llvm::ConstantInt::get(i8_ty, tag), case_bb);
      else
        sw->addCase(llvm::ConstantInt::get(i8_ty, i), case_bb);

      builder.SetInsertPoint(case_bb);

      // Narrow the subject variable for this arm.
      llvm::AllocaInst *saved = nullptr;
      if (!subject_var.empty() && pattern_sem) {
        auto local_it = locals.find(subject_var);
        if (local_it != locals.end()) {
          auto *extracted = emit_union_extract(union_ptr, pattern_sem,
                                                subject_sem);
          if (extracted) {
            auto *narrowed = create_entry_alloca(
                func, subject_var + ".case", llvm_type(pattern_sem));
            builder.CreateStore(extracted, narrowed);
            saved = local_it->second;
            locals[subject_var] = narrowed;
          }
        }
      }

      llvm::Value *body_val = nullptr;
      if (auto *block = std::get_if<BlockNode>(&arm.body->data)) {
        body_val = emit_block(*block);
      } else {
        body_val = emit_expr(*arm.body);
      }

      // Restore original local.
      if (saved)
        locals[subject_var] = saved;

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
    case_results.push_back({else_val, builder.GetInsertBlock(),
                            else_terminated});

  } else if (is_string) {
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
    if (current_actor)
      builder.CreateCall(module->getFunction("mc_reduction_tick"),
                         {current_actor});
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
      if (current_actor)
        builder.CreateCall(module->getFunction("mc_reduction_tick"),
                           {current_actor});
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
          if (current_actor)
            builder.CreateCall(module->getFunction("mc_reduction_tick"),
                               {current_actor});

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
        } else if (iter_sem && iter_sem->kind == TypeKind::Map) {
          // ── Map iteration: for [k,] v : map { ... } ───────────────
          auto &map_info = std::get<MapTypeInfo>(iter_sem->detail);
          bool string_keys = is_string_key_type(map_info.key);

          // Get map size.
          auto *size_fn = module->getFunction("mc_map_size");
          auto *map_len = builder.CreateCall(size_fn, {iterable}, "map.len");

          // Create index variable for iterating occupied slots.
          auto *idx_alloca = create_entry_alloca(func, ".map.idx", i64_type);
          builder.CreateStore(llvm::ConstantInt::get(i64_type, 0), idx_alloca);

          // Create allocas for loop variables.
          auto *key_ll = llvm_type(map_info.key);
          auto *val_ll = llvm_type(map_info.value);
          llvm::AllocaInst *key_alloca = nullptr;
          llvm::AllocaInst *val_alloca = nullptr;

          if (range->vars.size() == 1) {
            // Single variable gets the value.
            val_alloca = create_entry_alloca(
                func, std::string(range->vars[0].name), val_ll);
            locals[std::string(range->vars[0].name)] = val_alloca;
          } else if (range->vars.size() == 2) {
            key_alloca = create_entry_alloca(
                func, std::string(range->vars[0].name), key_ll);
            locals[std::string(range->vars[0].name)] = key_alloca;
            val_alloca = create_entry_alloca(
                func, std::string(range->vars[1].name), val_ll);
            locals[std::string(range->vars[1].name)] = val_alloca;
          }

          // Branch to condition.
          builder.CreateBr(cond_bb);

          // Condition: idx < size.
          builder.SetInsertPoint(cond_bb);
          auto *cur_idx = builder.CreateLoad(i64_type, idx_alloca, "map.idx");
          auto *cmp = builder.CreateICmpSLT(cur_idx, map_len, "map.cmp");
          builder.CreateCondBr(cmp, body_bb, exit_bb);

          // Body: load key and value at current index.
          func->insert(func->end(), body_bb);
          builder.SetInsertPoint(body_bb);
          if (current_actor)
            builder.CreateCall(module->getFunction("mc_reduction_tick"),
                               {current_actor});

          auto *body_idx = builder.CreateLoad(i64_type, idx_alloca, "map.idx");

          if (key_alloca) {
            auto *key_at_fn = module->getFunction("mc_map_key_at");
            auto *key_ptr = builder.CreateCall(
                key_at_fn, {iterable, body_idx}, "map.key.ptr");
            auto *key_val = builder.CreateLoad(key_ll, key_ptr, "map.key");
            builder.CreateStore(key_val, key_alloca);
          }

          if (val_alloca) {
            auto *val_at_fn = module->getFunction("mc_map_value_at");
            auto *val_ptr = builder.CreateCall(
                val_at_fn, {iterable, body_idx}, "map.val.ptr");
            auto *val_val = builder.CreateLoad(val_ll, val_ptr, "map.val");
            builder.CreateStore(val_val, val_alloca);
          }

          auto &body_block = std::get<BlockNode>(node.body->data);
          emit_block(body_block);
          if (!builder.GetInsertBlock()->getTerminator())
            builder.CreateBr(update_bb);

          // Update: idx++.
          func->insert(func->end(), update_bb);
          builder.SetInsertPoint(update_bb);
          auto *upd_idx = builder.CreateLoad(i64_type, idx_alloca, "map.idx");
          auto *next_idx = builder.CreateAdd(
              upd_idx, llvm::ConstantInt::get(i64_type, 1), "map.idx.next");
          builder.CreateStore(next_idx, idx_alloca);
          builder.CreateBr(cond_bb);
        } else if (iter_sem && iter_sem->kind == TypeKind::Struct) {
          // Check for Task iteration: for msg : task { ... }
          auto &st_info = std::get<StructTypeInfo>(iter_sem->detail);
          if (st_info.name == "Task") {
            // Get the channel pointer from the actor:
            // actor->channel is at a known offset.  We pass the actor ptr
            // to mc_channel_recv which expects (ch, buf).
            // The actor struct layout has `channel` at a specific offset.
            // We use a helper: load actor->channel as a ptr.
            //
            // actor->channel offset: we GEP into the raw struct.
            // However, codegen doesn't have the C struct layout.  Instead,
            // we store the channel pointer at the spawn site into a local
            // variable, and iterate on it.
            //
            // For now, we emit a runtime call pattern:
            //   actor ptr = iterable (the Task handle)
            //   channel ptr was stored alongside the task in a companion
            //   local variable named "<task>.channel".

            // Look for companion channel variable.
            std::string task_name;
            if (auto *iter_id = std::get_if<IdentifierNode>(
                    &range->iterable->data))
              task_name = std::string(iter_id->name);

            llvm::Value *ch_ptr = nullptr;
            if (!task_name.empty()) {
              auto ch_it = locals.find(task_name + ".channel");
              if (ch_it != locals.end())
                ch_ptr = builder.CreateLoad(
                    llvm::PointerType::getUnqual(context),
                    ch_it->second, "ch.ptr");
            }

            if (ch_ptr) {
              // Determine message type.  Default to i64 if unknown.
              llvm::Type *msg_ll = i64_type;
              // TODO: Extract element type from generic type parameter.

              // Create message buffer alloca.
              llvm::AllocaInst *msg_alloca = nullptr;
              if (range->vars.size() >= 1) {
                std::string vname(range->vars[0].name);
                msg_alloca = create_entry_alloca(func, vname, msg_ll);
                locals[vname] = msg_alloca;
              } else {
                msg_alloca = create_entry_alloca(func, ".msg.buf", msg_ll);
              }

              // Branch to condition (recv loop).
              builder.CreateBr(cond_bb);

              // Condition: call mc_channel_recv; break if -1.
              builder.SetInsertPoint(cond_bb);
              auto *recv_fn = module->getFunction("mc_channel_recv");
              auto *rc = builder.CreateCall(recv_fn, {ch_ptr, msg_alloca},
                                             "recv.rc");
              auto *eof = builder.CreateICmpEQ(
                  rc, llvm::ConstantInt::get(
                      llvm::Type::getInt32Ty(context), -1),
                  "recv.eof");
              builder.CreateCondBr(eof, exit_bb, body_bb);

              // Body.
              func->insert(func->end(), body_bb);
              builder.SetInsertPoint(body_bb);
              if (current_actor)
                builder.CreateCall(
                    module->getFunction("mc_reduction_tick"),
                    {current_actor});
              auto &body_block = std::get<BlockNode>(node.body->data);
              emit_block(body_block);
              if (!builder.GetInsertBlock()->getTerminator())
                builder.CreateBr(update_bb);

              // Update block: just loop back to recv.
              func->insert(func->end(), update_bb);
              builder.SetInsertPoint(update_bb);
              builder.CreateBr(cond_bb);
            } else {
              // No channel found — skip.
              builder.CreateBr(exit_bb);
            }
          } else {
            // Non-Task struct iterable — not yet supported.
            builder.CreateBr(exit_bb);
          }
        } else {
          // Non-array/map/task iterable — not yet supported.
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
      if (current_actor)
        builder.CreateCall(module->getFunction("mc_reduction_tick"),
                           {current_actor});
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
// Map literals
// ===========================================================================

llvm::Value *CodeGen::emit_map_literal(const MapLiteralNode &node) {
  // Determine key/value sizes from semantic types.
  int64_t key_size = 8;  // default to i64 size
  int64_t val_size = 8;
  llvm::Type *key_ll_type = i64_type;
  llvm::Type *val_ll_type = i64_type;
  bool string_keys = false;

  // Get semantic type of the map literal node itself.
  // We look through the entries to determine types.
  if (!node.entries.empty()) {
    auto key_sem = semantic_type(*node.entries[0].key);
    auto val_sem = semantic_type(*node.entries[0].value);
    if (key_sem) {
      key_ll_type = llvm_type(key_sem);
      string_keys = is_string_key_type(key_sem);
      if (key_ll_type->isDoubleTy())
        key_size = 8;
      else if (key_ll_type->isIntegerTy(1))
        key_size = 1;
      else if (key_ll_type->isIntegerTy(64))
        key_size = 8;
      else if (key_ll_type->isPointerTy())
        key_size = 8;
    }
    if (val_sem) {
      val_ll_type = llvm_type(val_sem);
      if (val_ll_type->isDoubleTy())
        val_size = 8;
      else if (val_ll_type->isIntegerTy(1))
        val_size = 1;
      else if (val_ll_type->isIntegerTy(64))
        val_size = 8;
      else if (val_ll_type->isPointerTy())
        val_size = 8;
    }
  }

  // Create the map: mc_map_new(key_size, val_size)
  // For string keys, pass -1 as key_size sentinel.
  auto *new_fn = module->getFunction("mc_map_new");
  int64_t key_size_arg = string_keys ? -1 : key_size;
  auto *map = builder.CreateCall(
      new_fn,
      {llvm::ConstantInt::get(i64_type, key_size_arg),
       llvm::ConstantInt::get(i64_type, val_size)},
      "map");

  // Insert each entry.
  auto *set_fn = module->getFunction("mc_map_set");
  auto *func = builder.GetInsertBlock()->getParent();
  auto *is_str_key = llvm::ConstantInt::get(i64_type, string_keys ? 1 : 0);

  for (auto &entry : node.entries) {
    auto *key_val = emit_expr(*entry.key);
    auto *val_val = emit_expr(*entry.value);
    if (!key_val || !val_val)
      continue;

    // Store key and value to temporaries for passing by pointer.
    auto *key_tmp = create_entry_alloca(func, "map.key.tmp", key_val->getType());
    builder.CreateStore(key_val, key_tmp);
    auto *val_tmp = create_entry_alloca(func, "map.val.tmp", val_val->getType());
    builder.CreateStore(val_val, val_tmp);

    builder.CreateCall(set_fn, {map, key_tmp, val_tmp, is_str_key});
  }

  return map;
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

  // Map indexing: map[key] → loads value from mc_map_get.
  if (obj_sem && obj_sem->kind == TypeKind::Map) {
    auto *idx = emit_expr(*node.index);
    if (!idx)
      return nullptr;

    auto &map_info = std::get<MapTypeInfo>(obj_sem->detail);
    bool string_keys = is_string_key_type(map_info.key);
    auto *is_str_key = llvm::ConstantInt::get(i64_type, string_keys ? 1 : 0);

    auto *func = builder.GetInsertBlock()->getParent();

    // Store key to a temporary for passing by pointer.
    auto *key_tmp = create_entry_alloca(func, "map.idx.key", idx->getType());
    builder.CreateStore(idx, key_tmp);

    auto *get_fn = module->getFunction("mc_map_get");
    auto *val_ptr = builder.CreateCall(get_fn, {obj, key_tmp, is_str_key}, "map.get");

    // Load the value from the returned pointer.
    auto *val_ll = llvm_type(map_info.value);
    return builder.CreateLoad(val_ll, val_ptr, "map.val");
  }

  // String indexing — deferred for now.
  return nullptr;
}

// ===========================================================================
// Or expression (error stripping)
// ===========================================================================

llvm::Value *CodeGen::emit_or_expr(const OrExprNode &node) {
  // Emit the expression that may produce a union with Error.
  auto *expr_val = emit_expr(*node.expr);
  if (!expr_val)
    return nullptr;

  auto expr_sem = semantic_type(*node.expr);
  if (!expr_sem)
    return expr_val;

  // If the expression is not a union, just return the value.
  if (expr_sem->kind != TypeKind::Union)
    return expr_val;

  // Check if this is an impure union (contains Error).
  if (!is_impure_union(expr_sem))
    return expr_val;

  auto *union_st = get_union_llvm_type(expr_sem);
  if (!union_st)
    return expr_val;

  auto *func = builder.GetInsertBlock()->getParent();

  // The expr_val should be an alloca (pointer to the union struct).
  // If it's not already a pointer to the union, we need to handle that.
  llvm::Value *union_ptr = expr_val;

  // If union_ptr is a loaded value (struct type, not pointer), store it.
  if (!union_ptr->getType()->isPointerTy() ||
      (llvm::isa<llvm::LoadInst>(union_ptr))) {
    auto *tmp = create_entry_alloca(func, "or.union", union_st);
    builder.CreateStore(expr_val, tmp);
    union_ptr = tmp;
  }

  // Load the tag.
  auto *tag_gep = builder.CreateStructGEP(union_st, union_ptr, 0, "or.tag");
  auto *tag = builder.CreateLoad(llvm::Type::getInt8Ty(context), tag_gep,
                                  "or.tag.val");

  // Find which tag values correspond to Error types.
  auto &info = std::get<UnionTypeInfo>(expr_sem->detail);
  std::vector<int> error_tags;
  std::vector<int> non_error_tags;
  for (size_t i = 0; i < info.alternatives.size(); ++i) {
    auto &alt = info.alternatives[i];
    if (alt->kind == TypeKind::Interface) {
      auto &iface = std::get<InterfaceTypeInfo>(alt->detail);
      if (iface.name == "Error") {
        error_tags.push_back(static_cast<int>(i));
        continue;
      }
    }
    non_error_tags.push_back(static_cast<int>(i));
  }

  // Create basic blocks.
  auto *ok_bb = llvm::BasicBlock::Create(context, "or.ok", func);
  auto *err_bb = llvm::BasicBlock::Create(context, "or.err");
  auto *merge_bb = llvm::BasicBlock::Create(context, "or.merge");

  // Branch based on whether the tag is an error tag.
  // If there's only one error tag, simple comparison.
  // For multiple error tags, we'd need an or-chain, but typically there's
  // just one Error interface in the union.
  if (error_tags.size() == 1) {
    auto *is_err = builder.CreateICmpEQ(
        tag,
        llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), error_tags[0]),
        "or.is_err");
    builder.CreateCondBr(is_err, err_bb, ok_bb);
  } else {
    // Multiple error tags — build an OR chain.
    llvm::Value *is_err = llvm::ConstantInt::get(
        llvm::Type::getInt1Ty(context), 0);
    for (int et : error_tags) {
      auto *cmp = builder.CreateICmpEQ(
          tag,
          llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), et),
          "or.cmp");
      is_err = builder.CreateOr(is_err, cmp, "or.any_err");
    }
    builder.CreateCondBr(is_err, err_bb, ok_bb);
  }

  // ── OK block: extract the non-error value ──────────────────────────
  builder.SetInsertPoint(ok_bb);

  // Determine the purified result type.
  TypePtr purified = strip_error_from_union(expr_sem);
  llvm::Value *ok_val = nullptr;

  if (purified && purified->kind == TypeKind::Union) {
    // Multiple non-error alternatives remain — result is still a union.
    // Re-wrap into the purified union type.
    auto *purified_st = get_union_llvm_type(purified);
    auto *purified_alloca = create_entry_alloca(func, "or.purified",
                                                 purified_st);
    builder.CreateStore(llvm::Constant::getNullValue(purified_st),
                        purified_alloca);

    // We need to remap the tag. The original tag corresponds to the position
    // in the full union; we need the position in the purified union.
    auto &pur_info = std::get<UnionTypeInfo>(purified->detail);
    auto *i8_ty = llvm::Type::getInt8Ty(context);

    // Build a switch to remap tags and copy the payload.
    auto *remap_default = llvm::BasicBlock::Create(context, "or.remap.def");
    auto *remap_merge = llvm::BasicBlock::Create(context, "or.remap.merge");
    auto *sw = builder.CreateSwitch(tag, remap_default, non_error_tags.size());

    std::vector<std::pair<llvm::Value *, llvm::BasicBlock *>> phi_entries;

    for (int orig_tag : non_error_tags) {
      auto *case_bb = llvm::BasicBlock::Create(
          context, "or.remap." + std::to_string(orig_tag), func);
      sw->addCase(llvm::ConstantInt::get(i8_ty, orig_tag), case_bb);

      builder.SetInsertPoint(case_bb);

      // Find the new tag index in the purified union.
      int new_tag = -1;
      for (size_t pi = 0; pi < pur_info.alternatives.size(); ++pi) {
        if (types_equal(pur_info.alternatives[pi],
                        info.alternatives[orig_tag])) {
          new_tag = static_cast<int>(pi);
          break;
        }
      }
      if (new_tag < 0) new_tag = 0;

      // Set the new tag.
      auto *ptag_gep = builder.CreateStructGEP(purified_st, purified_alloca,
                                                 0, "pur.tag");
      builder.CreateStore(llvm::ConstantInt::get(i8_ty, new_tag), ptag_gep);

      // Copy the payload bytes.
      auto *src_payload = builder.CreateStructGEP(union_st, union_ptr, 1,
                                                   "src.payload");
      auto *dst_payload = builder.CreateStructGEP(purified_st,
                                                   purified_alloca, 1,
                                                   "dst.payload");
      uint64_t pay_sz = union_payload_size(purified);
      builder.CreateMemCpy(dst_payload, llvm::Align(1),
                           src_payload, llvm::Align(1), pay_sz);

      builder.CreateBr(remap_merge);
      phi_entries.push_back({purified_alloca, builder.GetInsertBlock()});
    }

    func->insert(func->end(), remap_default);
    builder.SetInsertPoint(remap_default);
    builder.CreateBr(remap_merge);

    func->insert(func->end(), remap_merge);
    builder.SetInsertPoint(remap_merge);

    // Load the purified union struct for the PHI.
    ok_val = builder.CreateLoad(purified_st, purified_alloca, "or.ok.val");
  } else if (purified) {
    // Single non-error alternative — extract it directly.
    ok_val = emit_union_extract(union_ptr, purified, expr_sem);
  }

  if (!ok_val)
    ok_val = llvm::Constant::getNullValue(
        purified ? llvm_type(purified) : i64_type);

  builder.CreateBr(merge_bb);
  auto *ok_end_bb = builder.GetInsertBlock();

  // ── Error block: emit fallback ─────────────────────────────────────
  func->insert(func->end(), err_bb);
  builder.SetInsertPoint(err_bb);

  // If the or-clause has a pipe variable, bind it.
  // For now, we pass a null pointer as the error value (full error
  // extraction would need interface boxing). This is enough for the
  // fallback block to work.
  if (node.pipe) {
    std::string pipe_name(node.pipe->name);
    auto *err_alloca = create_entry_alloca(
        func, pipe_name, llvm::PointerType::getUnqual(context));
    builder.CreateStore(
        llvm::ConstantPointerNull::get(
            llvm::PointerType::getUnqual(context)),
        err_alloca);
    locals[pipe_name] = err_alloca;
  }

  auto &fallback_block = std::get<BlockNode>(node.fallback->data);
  auto *fallback_val = emit_block(fallback_block);

  // The fallback value must match the purified type.
  if (!fallback_val && ok_val)
    fallback_val = llvm::Constant::getNullValue(ok_val->getType());

  // Coerce fallback to match ok_val type if needed.
  if (fallback_val && ok_val &&
      fallback_val->getType() != ok_val->getType()) {
    // If the result is a union but fallback is a concrete value, wrap it.
    if (purified && purified->kind == TypeKind::Union && fallback_val) {
      // Try to find the semantic type of the fallback.
      auto fb_sem = semantic_type(*node.fallback);
      if (fb_sem && fb_sem->kind != TypeKind::Union) {
        auto *wrapped = emit_union_wrap(fallback_val, fb_sem, purified);
        if (wrapped) {
          auto *purified_st = get_union_llvm_type(purified);
          fallback_val = builder.CreateLoad(purified_st, wrapped,
                                             "or.fb.union");
        }
      }
    } else {
      // Type mismatch — use null of ok type.
      fallback_val = llvm::Constant::getNullValue(ok_val->getType());
    }
  }

  bool err_terminated = builder.GetInsertBlock()->getTerminator() != nullptr;
  if (!err_terminated)
    builder.CreateBr(merge_bb);
  auto *err_end_bb = builder.GetInsertBlock();

  // Clean up pipe variable.
  if (node.pipe) {
    locals.erase(std::string(node.pipe->name));
  }

  // ── Merge block ────────────────────────────────────────────────────
  func->insert(func->end(), merge_bb);
  builder.SetInsertPoint(merge_bb);

  if (ok_val && fallback_val &&
      ok_val->getType() == fallback_val->getType() && !err_terminated) {
    auto *phi = builder.CreatePHI(ok_val->getType(), 2, "or.result");
    phi->addIncoming(ok_val, ok_end_bb);
    phi->addIncoming(fallback_val, err_end_bb);
    return phi;
  }

  return ok_val;
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

  // Module selector: mod.Symbol — used for constants, enum variants, etc.
  // Function calls are handled in emit_call_expr, so here we handle
  // non-call access (e.g., module constants, enum variant tags).
  auto obj_sem = semantic_type(*node.object);
  if (obj_sem && obj_sem->kind == TypeKind::Module) {
    auto &mod = std::get<ModuleTypeInfo>(obj_sem->detail);
    for (auto &exp : mod.exports) {
      if (exp.name == field_name) {
        if (exp.type && exp.type->kind == TypeKind::Func) {
          // Function reference (not a call) — declare and return.
          auto *fn = declare_import(mod.name, field_name, exp.type);
          return fn;
        }
        // For enum variants from an imported module, look up the tag.
        if (exp.type && exp.type->kind == TypeKind::Enum) {
          auto &einfo = std::get<EnumTypeInfo>(exp.type->detail);
          // The field might be accessing a variant of the enum.
          // This would be mod.EnumName which is the type, not a value.
          // Variant access would be mod.EnumName.Variant — handled elsewhere.
          break;
        }

        // Non-function, non-enum export: a module-level constant.
        // Declare (or find) an external global and load from it.
        {
          std::string gv_name = mangle(mod.name, field_name);
          auto *ll = llvm_type(exp.type);

          // For struct-typed constants, ensure the LLVM struct type exists.
          if (exp.type && exp.type->kind == TypeKind::Struct) {
            auto &sinfo = std::get<StructTypeInfo>(exp.type->detail);
            if (!struct_types.count(sinfo.name)) {
              // Register a minimal LLVM struct from the semantic info.
              std::vector<llvm::Type *> ftypes;
              std::vector<std::string> fnames;
              for (auto &f : sinfo.fields) {
                ftypes.push_back(llvm_type(f.type));
                fnames.push_back(f.name);
              }
              auto *st = llvm::StructType::create(context, ftypes,
                                                   "mc." + sinfo.name);
              struct_types[sinfo.name] = st;
              struct_fields[sinfo.name] = std::move(fnames);
            }
            ll = struct_types[sinfo.name];
          }

          auto *gv = module->getGlobalVariable(gv_name);
          if (!gv) {
            gv = new llvm::GlobalVariable(
                *module, ll, /*isConstant=*/true,
                llvm::GlobalValue::ExternalLinkage,
                /*Initializer=*/nullptr, gv_name);
          }
          return builder.CreateLoad(ll, gv, field_name);
        }
        break;
      }
    }
    return nullptr;
  }

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

    auto obj_sem = semantic_type(*sel->object);

    // ── Module function call: mod.Func(args) ────────────────────────
    if (obj_sem && obj_sem->kind == TypeKind::Module) {
      auto &mod = std::get<ModuleTypeInfo>(obj_sem->detail);
      // Find the export.
      TypePtr func_type;
      for (auto &exp : mod.exports) {
        if (exp.name == method) {
          func_type = exp.type;
          break;
        }
      }
      if (!func_type || func_type->kind != TypeKind::Func)
        return nullptr;

      auto *callee = declare_import(mod.name, method, func_type);
      if (!callee)
        return nullptr;

      std::vector<llvm::Value *> args;
      for (auto &arg_node : node.args) {
        auto *val = emit_expr(*arg_node);
        if (val)
          args.push_back(val);
      }

      if (callee->getReturnType()->isVoidTy()) {
        builder.CreateCall(callee, args);
        return nullptr;
      }
      return builder.CreateCall(callee, args, "pkg.call");
    }

    auto *obj = emit_expr(*sel->object);
    if (!obj)
      return nullptr;

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

    // Map methods.
    if (obj_sem && obj_sem->kind == TypeKind::Map) {
      auto &map_info = std::get<MapTypeInfo>(obj_sem->detail);
      bool string_keys = is_string_key_type(map_info.key);
      auto *is_str_key = llvm::ConstantInt::get(i64_type, string_keys ? 1 : 0);

      if (method == "Size") {
        auto *size_fn = module->getFunction("mc_map_size");
        return builder.CreateCall(size_fn, {obj}, "map.size");
      }
      if (method == "Set" && node.args.size() >= 2) {
        auto *key_val = emit_expr(*node.args[0]);
        auto *val_val = emit_expr(*node.args[1]);
        if (!key_val || !val_val) return nullptr;
        auto *func = builder.GetInsertBlock()->getParent();
        auto *key_tmp = create_entry_alloca(func, "map.set.key", key_val->getType());
        builder.CreateStore(key_val, key_tmp);
        auto *val_tmp = create_entry_alloca(func, "map.set.val", val_val->getType());
        builder.CreateStore(val_val, val_tmp);
        auto *set_fn = module->getFunction("mc_map_set");
        builder.CreateCall(set_fn, {obj, key_tmp, val_tmp, is_str_key});
        return nullptr;
      }
      if (method == "At" && node.args.size() >= 1) {
        auto *key_val = emit_expr(*node.args[0]);
        if (!key_val) return nullptr;
        auto *func = builder.GetInsertBlock()->getParent();
        auto *key_tmp = create_entry_alloca(func, "map.at.key", key_val->getType());
        builder.CreateStore(key_val, key_tmp);
        auto *get_fn = module->getFunction("mc_map_get");
        auto *val_ptr = builder.CreateCall(get_fn, {obj, key_tmp, is_str_key}, "map.at");
        auto *val_ll = llvm_type(map_info.value);
        return builder.CreateLoad(val_ll, val_ptr, "map.at.val");
      }
      if ((method == "Key?" || method == "Has") && node.args.size() >= 1) {
        auto *key_val = emit_expr(*node.args[0]);
        if (!key_val) return nullptr;
        auto *func = builder.GetInsertBlock()->getParent();
        auto *key_tmp = create_entry_alloca(func, "map.has.key", key_val->getType());
        builder.CreateStore(key_val, key_tmp);
        auto *has_fn = module->getFunction("mc_map_has");
        auto *result = builder.CreateCall(has_fn, {obj, key_tmp, is_str_key}, "map.has");
        return builder.CreateICmpNE(result, llvm::ConstantInt::get(i64_type, 0), "map.has.bool");
      }
      if (method == "Remove" && node.args.size() >= 1) {
        auto *key_val = emit_expr(*node.args[0]);
        if (!key_val) return nullptr;
        auto *func = builder.GetInsertBlock()->getParent();
        auto *key_tmp = create_entry_alloca(func, "map.rm.key", key_val->getType());
        builder.CreateStore(key_val, key_tmp);
        auto *rm_fn = module->getFunction("mc_map_remove");
        builder.CreateCall(rm_fn, {obj, key_tmp, is_str_key});
        return nullptr;
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

    // ── Task method calls ─────────────────────────────────────────────
    // Task is a semantic struct wrapping mc_actor*.  obj is the actor ptr.
    if (obj_sem && obj_sem->kind == TypeKind::Struct) {
      auto &sinfo = std::get<StructTypeInfo>(obj_sem->detail);
      if (sinfo.name == "Task") {
        if (method == "Alive?") {
          auto *fn = module->getFunction("mc_task_alive");
          auto *result = builder.CreateCall(fn, {obj}, "alive");
          return builder.CreateICmpNE(result,
              llvm::ConstantInt::get(i64_type, 0), "alive.bool");
        }
        if (method == "Cancel") {
          builder.CreateCall(module->getFunction("mc_task_cancel"), {obj});
          return nullptr;
        }
        if (method == "Term") {
          builder.CreateCall(module->getFunction("mc_task_term"), {obj});
          return nullptr;
        }
        if (method == "Wait") {
          auto *func = builder.GetInsertBlock()->getParent();
          auto *status_alloca = create_entry_alloca(func, "wait.status",
                                                     i64_type);
          builder.CreateStore(llvm::ConstantInt::get(i64_type, 0),
                              status_alloca);
          auto *result_ptr = builder.CreateCall(
              module->getFunction("mc_task_wait"),
              {obj, status_alloca}, "wait.result");
          // Load the status to check for error.
          auto *status = builder.CreateLoad(i64_type, status_alloca,
                                             "wait.status");
          // MC_ACTOR_COMPLETED == 2
          auto *is_ok = builder.CreateICmpEQ(status,
              llvm::ConstantInt::get(i64_type, 2), "wait.ok");

          // The result type depends on the generic type parameter.
          // For now return the raw pointer — the or-expr handler will
          // wrap it into a union if needed.  If the parent expects a
          // specific type, the result_ptr points to a copy of it.
          // We return result_ptr as an opaque ptr.
          return result_ptr;
        }
        // Fall through to generic struct method handling if not matched.
      }

      // ── Context method calls (inside spawn body) ─────────────────
      if (sinfo.name == "Context") {
        if (method == "Cancelled?") {
          auto *fn = module->getFunction("mc_context_cancelled");
          auto *result = builder.CreateCall(fn, {obj}, "cancelled");
          return builder.CreateICmpNE(result,
              llvm::ConstantInt::get(i64_type, 0), "cancelled.bool");
        }
        if (method == "Send" && !node.args.empty()) {
          auto *val = emit_expr(*node.args[0]);
          if (!val) return nullptr;
          auto *func = builder.GetInsertBlock()->getParent();
          auto *tmp = create_entry_alloca(func, "send.tmp", val->getType());
          builder.CreateStore(val, tmp);
          builder.CreateCall(module->getFunction("mc_context_send"),
                             {obj, tmp});
          return nullptr;
        }
        if (method == "Exit") {
          auto *func = builder.GetInsertBlock()->getParent();
          if (!node.args.empty()) {
            auto *val = emit_expr(*node.args[0]);
            if (val) {
              auto *tmp = create_entry_alloca(func, "exit.tmp",
                                               val->getType());
              builder.CreateStore(val, tmp);
              auto &dl = module->getDataLayout();
              uint64_t sz = dl.getTypeAllocSize(val->getType());
              builder.CreateCall(
                  module->getFunction("mc_context_exit"),
                  {obj, tmp,
                   llvm::ConstantInt::get(i64_type, sz)});
            }
          } else {
            auto *null_ptr = llvm::ConstantPointerNull::get(
                llvm::PointerType::getUnqual(context));
            builder.CreateCall(
                module->getFunction("mc_context_exit"),
                {obj, null_ptr, llvm::ConstantInt::get(i64_type, 0)});
          }
          return nullptr;
        }
        // Fall through to generic struct method handling if not matched.
      }
    }

    // Struct method call: obj.Method(args) → StructName.Method(obj, args).
    if (obj_sem && obj_sem->kind == TypeKind::Struct) {
      auto &info = std::get<StructTypeInfo>(obj_sem->detail);

      // Determine the package that defines this struct.
      // If the struct exists in our local struct_types, use package_name.
      // Otherwise, check if the object originates from a module selector
      // and use that module's name.
      // Determine the package that defines this struct.
      // Walk the selector chain to find if the object originates from an
      // imported module. For chained selectors like os.Stdout.Write,
      // the inner selector's object is the module.
      std::string origin_pkg = package_name;
      {
        const Node *walk = sel->object.get();
        while (walk) {
          if (auto *inner_sel = std::get_if<SelectorNode>(&walk->data)) {
            auto inner_sem = semantic_type(*inner_sel->object);
            if (inner_sem && inner_sem->kind == TypeKind::Module) {
              origin_pkg = std::get<ModuleTypeInfo>(inner_sem->detail).name;
              break;
            }
            walk = inner_sel->object.get();
          } else if (auto *id = std::get_if<IdentifierNode>(&walk->data)) {
            auto id_sem = semantic_type(*walk);
            if (id_sem && id_sem->kind == TypeKind::Module) {
              origin_pkg = std::get<ModuleTypeInfo>(id_sem->detail).name;
            }
            break;
          } else {
            break;
          }
        }
      }

      std::string link_name = mangle(origin_pkg, info.name + "__" + method);
      auto *callee = module->getFunction(link_name);

      // If the method isn't in this module (cross-package struct), declare it.
      if (!callee) {
        for (auto &m : info.methods) {
          if (m.name == method && m.signature &&
              m.signature->kind == TypeKind::Func) {
            auto &finfo = std::get<FuncTypeInfo>(m.signature->detail);
            auto *ptr_type = llvm::PointerType::getUnqual(context);
            std::vector<llvm::Type *> param_ll;
            param_ll.push_back(ptr_type); // self
            for (auto &p : finfo.params)
              param_ll.push_back(llvm_type(p));
            llvm::Type *ret_ll = finfo.returns.empty()
                                     ? void_ll_type
                                     : llvm_type(finfo.returns[0]);
            auto *ft = llvm::FunctionType::get(ret_ll, param_ll, false);
            callee = llvm::Function::Create(
                ft, llvm::Function::ExternalLinkage, link_name, module.get());
            break;
          }
        }
      }

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

        // If self_ptr is a struct value (not a pointer/alloca), we need
        // to spill it to a temporary alloca so the method gets a ptr.
        auto st_it2 = struct_types.find(info.name);
        if (st_it2 != struct_types.end() &&
            self_ptr->getType() == st_it2->second) {
          auto *func = builder.GetInsertBlock()->getParent();
          auto *tmp = create_entry_alloca(func, "self.tmp", st_it2->second);
          builder.CreateStore(self_ptr, tmp);
          self_ptr = tmp;
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

  // ── Concurrency intrinsics ──────────────────────────────────────────
  // These need special handling because they inject the current_actor
  // pointer or emit inline LLVM instructions.

  if (name == "intrinsic_yield") {
    // intrinsic_yield() → mc_actor_yield(current_actor)
    if (current_actor) {
      builder.CreateCall(module->getFunction("mc_actor_yield"),
                         {current_actor});
    }
    // Outside a spawn block this is a no-op.
    return llvm::Constant::getNullValue(
        llvm::PointerType::getUnqual(context));
  }

  if (name == "intrinsic_atomic_add") {
    // intrinsic_atomic_add(ptr, val) → atomicrmw add i64* %ptr, i64 %val
    // The first argument is treated as a pointer to an i64.
    // We need to get the *address* of the first argument, not its value.
    auto *val = emit_expr(*node.args[1]);
    // Get the address of the first argument (must be a variable).
    auto *ptr_ident = std::get_if<IdentifierNode>(&node.args[0]->data);
    llvm::Value *ptr = nullptr;
    if (ptr_ident) {
      auto it = locals.find(std::string(ptr_ident->name));
      if (it != locals.end())
        ptr = it->second; // alloca — this IS the pointer
    }
    if (!ptr) {
      // Fallback: emit the expression as a value (won't be atomic, but
      // won't crash).  Semantic analysis should catch misuse.
      return llvm::Constant::getNullValue(i64_type);
    }
    return builder.CreateAtomicRMW(llvm::AtomicRMWInst::Add, ptr, val,
                                   llvm::MaybeAlign(),
                                   llvm::AtomicOrdering::SequentiallyConsistent);
  }

  if (name == "intrinsic_trap") {
    // intrinsic_trap(reason) → mc_actor_trap(current_actor, reason)
    auto *reason = emit_expr(*node.args[0]);
    if (current_actor) {
      builder.CreateCall(module->getFunction("mc_actor_trap"),
                         {current_actor, reason});
    }
    return llvm::Constant::getNullValue(
        llvm::PointerType::getUnqual(context));
  }

  // ── Regular function dispatch ───────────────────────────────────────
  std::string link_name;
  if (name == "intrinsic_print")
    link_name = "mc_intrinsic_print";
  else
    link_name = mangle(name);

  auto *callee = module->getFunction(link_name);

  // If not a known module function, check if it's a closure variable.
  if (!callee) {
    auto local_it = locals.find(name);
    if (local_it != locals.end()) {
      auto *alloca = local_it->second;
      // Check if this local holds a closure fat pointer (either directly
      // as closure_fat_ptr_type, or as a ptr to one for function parameters).
      auto callee_sem = semantic_type(*node.callee);
      bool is_func_typed = callee_sem && callee_sem->kind == TypeKind::Func;
      if (alloca->getAllocatedType() == closure_fat_ptr_type || is_func_typed) {
        // Determine the closure struct pointer.
        // If the alloca directly holds a closure struct, use it.
        // If the alloca holds a ptr (function parameter), load first.
        auto *ptr_type = llvm::PointerType::getUnqual(context);
        llvm::Value *closure_ptr = alloca;
        if (alloca->getAllocatedType() != closure_fat_ptr_type) {
          // Load the pointer to the closure struct.
          closure_ptr = builder.CreateLoad(ptr_type, alloca, "cl.load");
        }

        // Load fn_ptr and env_ptr from the closure.
        auto *fn_gep = builder.CreateStructGEP(closure_fat_ptr_type,
                                                closure_ptr, 0, "cl.fn.ptr");
        auto *fn_ptr = builder.CreateLoad(ptr_type, fn_gep, "cl.fn");
        auto *env_gep = builder.CreateStructGEP(closure_fat_ptr_type,
                                                  closure_ptr, 1, "cl.env.ptr");
        auto *env_ptr = builder.CreateLoad(ptr_type, env_gep, "cl.env");

        // Build the indirect call: fn(env, args...)
        std::vector<llvm::Value *> args;
        args.push_back(env_ptr);
        for (auto &arg_node : node.args) {
          auto *val = emit_expr(*arg_node);
          if (val)
            args.push_back(val);
        }

        // Build the function type from the semantic type.
        auto callee_sem = semantic_type(*node.callee);
        std::vector<llvm::Type *> param_types;
        param_types.push_back(ptr_type); // env
        llvm::Type *ret_ll = void_ll_type;

        if (callee_sem && callee_sem->kind == TypeKind::Func) {
          auto &fi = std::get<FuncTypeInfo>(callee_sem->detail);
          for (auto &pt : fi.params)
            param_types.push_back(llvm_type(pt));
          if (!fi.returns.empty())
            ret_ll = llvm_type(fi.returns[0]);
        } else {
          // Fallback: infer from args.
          for (size_t i = 1; i < args.size(); ++i)
            param_types.push_back(args[i]->getType());
        }

        auto *fn_type = llvm::FunctionType::get(ret_ll, param_types, false);
        if (ret_ll->isVoidTy()) {
          builder.CreateCall(fn_type, fn_ptr, args);
          return nullptr;
        }
        return builder.CreateCall(fn_type, fn_ptr, args, "cl.call");
      }
    }
    return nullptr;
  }

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

// ===========================================================================
// Function expressions (closures)
// ===========================================================================

llvm::Value *CodeGen::emit_func_expr(const FuncExprNode &node,
                                      const Node &parent) {
  auto *ptr_type = llvm::PointerType::getUnqual(context);
  auto *enclosing_func = builder.GetInsertBlock()->getParent();

  // Generate a unique name for this closure.
  std::string closure_name = "mc.closure." + std::to_string(next_closure_id++);

  // ── Look up captured variables ─────────────────────────────────────
  std::vector<Analyzer::CaptureInfo> captures;
  auto cap_it = analyzer.node_captures.find(&parent);
  if (cap_it != analyzer.node_captures.end())
    captures = cap_it->second;

  // ── Build the environment struct type ──────────────────────────────
  // The env struct holds one field per captured variable.
  std::vector<llvm::Type *> env_field_types;
  std::vector<std::string> env_field_names;
  for (auto &cap : captures) {
    auto *ll = llvm_type(cap.type);
    env_field_types.push_back(ll);
    env_field_names.push_back(cap.name);
  }

  llvm::StructType *env_type = nullptr;
  if (!env_field_types.empty()) {
    env_type = llvm::StructType::create(context, env_field_types,
                                         closure_name + ".env");
  }

  // ── Build the trampoline function ──────────────────────────────────
  // Signature: ret_type trampoline(ptr env, param1, param2, ...)
  std::vector<llvm::Type *> tramp_param_types;
  tramp_param_types.push_back(ptr_type); // env pointer (always first)

  for (auto &param : node.signature.params) {
    auto *ll = resolve_type_node(*param.type);
    for (size_t i = 0; i < param.names.identifiers.size(); ++i)
      tramp_param_types.push_back(ll);
  }

  llvm::Type *ret_type = void_ll_type;
  if (!node.signature.returns.empty())
    ret_type = resolve_type_node(*node.signature.returns[0]);

  auto *tramp_fn_type = llvm::FunctionType::get(ret_type, tramp_param_types,
                                                  false);
  auto *tramp_fn = llvm::Function::Create(
      tramp_fn_type, llvm::Function::InternalLinkage, closure_name,
      module.get());

  // Name the arguments.
  tramp_fn->getArg(0)->setName("env");
  size_t arg_idx = 1;
  for (auto &param : node.signature.params)
    for (auto &ident : param.names.identifiers)
      if (arg_idx < tramp_fn->arg_size())
        tramp_fn->getArg(arg_idx++)->setName(std::string(ident.name));

  // ── Emit the trampoline body ───────────────────────────────────────
  // Save current insertion state.
  auto saved_block = builder.GetInsertBlock();
  auto saved_point = builder.GetInsertPoint();
  auto saved_locals = locals;
  auto saved_managed = managed_locals;
  auto saved_is_main = current_func_is_main;

  auto *entry = llvm::BasicBlock::Create(context, "entry", tramp_fn);
  builder.SetInsertPoint(entry);

  locals.clear();
  managed_locals.clear();
  current_func_is_main = false;

  // Unpack environment struct into local variables.
  if (env_type) {
    auto *env_ptr = tramp_fn->getArg(0);
    for (size_t i = 0; i < captures.size(); ++i) {
      auto *field_gep = builder.CreateStructGEP(env_type, env_ptr, i,
                                                  captures[i].name + ".cap");
      auto *field_type = env_field_types[i];
      auto *val = builder.CreateLoad(field_type, field_gep,
                                      captures[i].name);
      auto *alloca = create_entry_alloca(tramp_fn, captures[i].name,
                                          field_type);
      builder.CreateStore(val, alloca);
      locals[captures[i].name] = alloca;
    }
  }

  // Create allocas for parameters.
  arg_idx = 1;
  for (auto &param : node.signature.params) {
    auto *ll = resolve_type_node(*param.type);
    for (auto &ident : param.names.identifiers) {
      std::string pname(ident.name);
      auto *alloca = create_entry_alloca(tramp_fn, pname, ll);
      builder.CreateStore(tramp_fn->getArg(arg_idx++), alloca);
      locals[pname] = alloca;
    }
  }

  // Emit the closure body.
  auto &block = std::get<BlockNode>(node.body->data);
  auto *tail_val = emit_block(block);

  // Add terminator if needed.
  if (!builder.GetInsertBlock()->getTerminator()) {
    emit_release_locals();
    if (ret_type->isVoidTy()) {
      builder.CreateRetVoid();
    } else if (tail_val && tail_val->getType() == ret_type) {
      builder.CreateRet(tail_val);
    } else {
      builder.CreateRet(llvm::Constant::getNullValue(ret_type));
    }
  }

  llvm::verifyFunction(*tramp_fn);

  // ── Restore the enclosing function's state ─────────────────────────
  builder.SetInsertPoint(saved_block, saved_point);
  locals = saved_locals;
  managed_locals = saved_managed;
  current_func_is_main = saved_is_main;

  // ── Allocate and populate the environment struct ───────────────────
  llvm::Value *env_ptr = llvm::ConstantPointerNull::get(ptr_type);
  if (env_type) {
    auto *env_alloca = create_entry_alloca(enclosing_func,
                                            closure_name + ".env",
                                            env_type);
    // Store each captured variable's current value into the env.
    for (size_t i = 0; i < captures.size(); ++i) {
      auto *field_gep = builder.CreateStructGEP(env_type, env_alloca, i,
                                                  captures[i].name + ".env.f");
      // Load the current value of the captured variable.
      auto local_it = locals.find(captures[i].name);
      if (local_it != locals.end()) {
        auto *alloca = local_it->second;
        auto *val = builder.CreateLoad(env_field_types[i], alloca,
                                        captures[i].name + ".cap.val");
        builder.CreateStore(val, field_gep);
      } else {
        // Zero-initialize if not found (shouldn't happen after analysis).
        builder.CreateStore(
            llvm::Constant::getNullValue(env_field_types[i]), field_gep);
      }
    }
    env_ptr = env_alloca;
  }

  // ── Build the closure fat pointer { fn_ptr, env_ptr } ─────────────
  auto *closure_alloca = create_entry_alloca(enclosing_func,
                                              closure_name + ".val",
                                              closure_fat_ptr_type);
  auto *fn_gep = builder.CreateStructGEP(closure_fat_ptr_type,
                                          closure_alloca, 0, "closure.fn");
  builder.CreateStore(tramp_fn, fn_gep);
  auto *env_gep = builder.CreateStructGEP(closure_fat_ptr_type,
                                            closure_alloca, 1, "closure.env");
  builder.CreateStore(env_ptr, env_gep);

  return closure_alloca;
}

// ===========================================================================
// Spawn expression
// ===========================================================================

llvm::Value *CodeGen::emit_spawn_expr(const SpawnExprNode &node,
                                       const Node &parent) {
  has_spawn = true;

  auto *ptr_type = llvm::PointerType::getUnqual(context);
  auto *enclosing_func = builder.GetInsertBlock()->getParent();

  // Generate a unique name for this spawn's outlined function.
  std::string spawn_name = "mc.spawn." + std::to_string(next_spawn_id++);

  // ── Look up captured variables ─────────────────────────────────────
  std::vector<Analyzer::SpawnCaptureInfo> captures;
  auto cap_it = analyzer.spawn_captures.find(&parent);
  if (cap_it != analyzer.spawn_captures.end())
    captures = cap_it->second;

  // ── Build the closure struct type ──────────────────────────────────
  // Holds one field per captured variable.
  std::vector<llvm::Type *> closure_field_types;
  std::vector<std::string> closure_field_names;
  for (auto &cap : captures) {
    auto *ll = llvm_type(cap.type);
    closure_field_types.push_back(ll);
    closure_field_names.push_back(cap.name);
  }

  llvm::StructType *closure_st = nullptr;
  uint64_t closure_size = 0;
  if (!closure_field_types.empty()) {
    closure_st = llvm::StructType::create(context, closure_field_types,
                                           spawn_name + ".closure");
    closure_size = module->getDataLayout().getTypeAllocSize(closure_st);
  }

  // ── Build the outlined function: void(mc_actor*) ───────────────────
  auto *outlined_fn_type = llvm::FunctionType::get(
      void_ll_type, {ptr_type}, false);
  auto *outlined_fn = llvm::Function::Create(
      outlined_fn_type, llvm::Function::InternalLinkage, spawn_name,
      module.get());
  outlined_fn->getArg(0)->setName("actor");

  // ── Emit the outlined function body ────────────────────────────────
  auto saved_block = builder.GetInsertBlock();
  auto saved_point = builder.GetInsertPoint();
  auto saved_locals = locals;
  auto saved_managed = managed_locals;
  auto saved_is_main = current_func_is_main;
  auto saved_actor = current_actor;

  auto *entry = llvm::BasicBlock::Create(context, "entry", outlined_fn);
  builder.SetInsertPoint(entry);

  locals.clear();
  managed_locals.clear();
  current_func_is_main = false;
  current_actor = outlined_fn->getArg(0);

  // Unpack closure data from actor->closure_data.
  // The runtime copies the closure struct into the actor's arena, so we
  // just read it back from the pointer stored in the mc_actor.  However,
  // we don't have the C struct layout in LLVM IR, so the runtime passes
  // the closure_data pointer.  We store it in a local and GEP into the
  // closure struct type.
  //
  // The mc_actor struct has closure_data at a known offset.  We'll use
  // a dedicated runtime helper or a fixed byte offset.  Since mc_actor
  // is opaque to codegen, we pass the closure_data as the entry arg's
  // associated data.  The runtime stores it in actor->closure_data and
  // the worker loop calls entry(actor).  We load it via:
  //   closure_data = *(void**)(actor + offsetof(mc_actor, closure_data))
  //
  // offsetof(mc_actor, closure_data) needs to match the C struct.
  // mc_actor layout (64-bit): refcount(8) result(8) result_size(8)
  //   status(8) cancelled(8) lock(40) done_cond(48) arena(8)
  //   entry(8) closure_data(8) ...
  // That's fragile.  Instead, generate a tiny C helper.
  //
  // Simpler approach: add a runtime function mc_actor_closure_data(actor)
  // that returns actor->closure_data.
  //
  // For now, let's define the offset based on the mc_actor C struct.
  // A more robust approach: declare mc_actor_closure_data as a runtime fn.
  //
  // Actually, the cleanest approach: we already have the closure_data
  // pointer passed via mc_actor_new.  The outlined function receives the
  // actor*, and the closure_data was memcpy'd into the arena.
  // We need to access it.  Let's add a trivial runtime accessor.

  if (closure_st && !captures.empty()) {
    // Declare or get the accessor: void* mc_actor_get_closure(mc_actor*)
    auto *accessor = module->getFunction("mc_actor_get_closure");
    if (!accessor) {
      accessor = llvm::Function::Create(
          llvm::FunctionType::get(ptr_type, {ptr_type}, false),
          llvm::Function::ExternalLinkage, "mc_actor_get_closure",
          module.get());
    }

    auto *closure_ptr = builder.CreateCall(accessor, {current_actor},
                                            "closure.ptr");

    for (size_t i = 0; i < captures.size(); ++i) {
      auto *field_gep = builder.CreateStructGEP(
          closure_st, closure_ptr, i, captures[i].name + ".cap");
      auto *val = builder.CreateLoad(closure_field_types[i], field_gep,
                                      captures[i].name);
      auto *alloca = create_entry_alloca(outlined_fn, captures[i].name,
                                          closure_field_types[i]);
      builder.CreateStore(val, alloca);
      locals[captures[i].name] = alloca;

      // Retain shared refcounted captures.
      if (captures[i].kind == Analyzer::SpawnCaptureKind::Share) {
        // The runtime already memcpy'd the data, but we need to retain
        // the refcounted objects so they aren't freed while we use them.
        TypePtr cap_type = captures[i].type;
        emit_retain(val, cap_type);
      }
    }
  }

  // If the spawn has a pipe variable (|task|), bind it to the actor ptr.
  if (node.pipe) {
    std::string pipe_name(node.pipe->name);
    auto *alloca = create_entry_alloca(outlined_fn, pipe_name, ptr_type);
    builder.CreateStore(current_actor, alloca);
    locals[pipe_name] = alloca;
  }

  // Emit the spawn body.
  if (auto *block = std::get_if<BlockNode>(&node.body->data)) {
    emit_block(*block);
  } else {
    // Identifier body (function reference) — call it as a trampoline.
    auto *body_val = emit_expr(*node.body);
    // If it returned a value, we could use context_exit, but for
    // simplicity we just ignore it; the function should use task.Exit().
    (void)body_val;
  }

  // Add terminator if needed.
  if (!builder.GetInsertBlock()->getTerminator())
    builder.CreateRetVoid();

  llvm::verifyFunction(*outlined_fn);

  // ── Restore enclosing function state ───────────────────────────────
  builder.SetInsertPoint(saved_block, saved_point);
  locals = saved_locals;
  managed_locals = saved_managed;
  current_func_is_main = saved_is_main;
  current_actor = saved_actor;

  // ── Pack closure data at the spawn site ────────────────────────────
  llvm::Value *closure_ptr_val =
      llvm::ConstantPointerNull::get(ptr_type);

  if (closure_st && !captures.empty()) {
    auto *closure_alloca = create_entry_alloca(
        enclosing_func, spawn_name + ".closure", closure_st);

    for (size_t i = 0; i < captures.size(); ++i) {
      auto *field_gep = builder.CreateStructGEP(
          closure_st, closure_alloca, i, captures[i].name + ".pack");

      auto local_it = locals.find(captures[i].name);
      if (local_it != locals.end()) {
        auto *alloca = local_it->second;
        auto *val = builder.CreateLoad(closure_field_types[i], alloca,
                                        captures[i].name + ".cap.val");
        builder.CreateStore(val, field_gep);

        // Retain shared refcounted captures at the spawn site.
        if (captures[i].kind == Analyzer::SpawnCaptureKind::Share)
          emit_retain(val, captures[i].type);
      } else {
        builder.CreateStore(
            llvm::Constant::getNullValue(closure_field_types[i]),
            field_gep);
      }
    }
    closure_ptr_val = closure_alloca;
  }

  // ── Create channel if generic type present ─────────────────────────
  llvm::Value *channel_ptr = nullptr;
  int64_t channel_elem_size = 0;
  if (node.generic && !node.generic->type_params.empty()) {
    // Resolve the channel element type.
    auto &type_param_node = *node.generic->type_params[0];
    auto chan_elem_sem = analyzer.resolve_type(type_param_node);
    auto *chan_elem_ll = llvm_type(chan_elem_sem);
    channel_elem_size = static_cast<int64_t>(
        module->getDataLayout().getTypeAllocSize(chan_elem_ll));

    auto *ch = builder.CreateCall(
        module->getFunction("mc_channel_new"),
        {llvm::ConstantInt::get(i64_type, channel_elem_size),
         llvm::ConstantInt::get(i64_type, 0)}, // 0 = default capacity
        "channel");
    channel_ptr = ch;
  }

  // ── Spawn the actor ────────────────────────────────────────────────
  auto *actor = builder.CreateCall(
      module->getFunction("mc_executor_spawn"),
      {outlined_fn,
       closure_ptr_val,
       llvm::ConstantInt::get(i64_type, static_cast<int64_t>(closure_size)),
       llvm::ConstantInt::get(i64_type, 0)}, // 0 = default arena max
      "actor");

  // Attach channel to actor if present.
  // We need to store ch into actor->channel.  Use a runtime helper.
  if (channel_ptr) {
    auto *attach_fn = module->getFunction("mc_actor_set_channel");
    if (!attach_fn) {
      attach_fn = llvm::Function::Create(
          llvm::FunctionType::get(void_ll_type, {ptr_type, ptr_type}, false),
          llvm::Function::ExternalLinkage, "mc_actor_set_channel",
          module.get());
    }
    builder.CreateCall(attach_fn, {actor, channel_ptr});
  }

  // ── Store the actor ptr as the Task handle ─────────────────────────
  // The actor ptr IS the Task — mc_actor* is our runtime Task handle.
  // Store it in a local alloca (caller will bind it via DeclAssign).

  // If there's a channel, also store it as a companion local so the
  // for-range iteration can find it.  The DeclAssign handler will bind
  // the task name, and we store the channel under "<name>.channel".
  // Since we don't know the target name yet, we use a temporary.
  // The channel is returned alongside the actor via a small wrapper.

  // For channel-based spawns, store the channel ptr in a pseudo-local
  // that the for-range handler can look up.
  if (channel_ptr) {
    // We'll store it with a predictable name.  The DeclAssign that
    // receives this spawn result will rename the actor alloca.  We
    // stash the channel in a local keyed by the spawn id.
    std::string ch_local = spawn_name + ".channel";
    auto *ch_alloca = create_entry_alloca(enclosing_func, ch_local, ptr_type);
    builder.CreateStore(channel_ptr, ch_alloca);
    locals[ch_local] = ch_alloca;

    // Also, we'll need to make the DeclAssign handler aware.
    // Store a mapping from the actor value to the channel local name.
    // For simplicity, we use a side-channel: after the DeclAssign binds
    // the task name, we create a "<taskname>.channel" alias.
    // We handle this by storing the channel under a known key and
    // having a post-decl-assign hook.  Instead, let's just store the
    // channel alloca indexed by the actor pointer value — but that's
    // fragile with LLVM values.
    //
    // Simplest approach: return a tagged pair.  But since our codegen
    // returns a single llvm::Value*, we need another mechanism.
    //
    // Pragmatic solution: stash the pending channel in a member variable.
    // The next DeclAssign that receives this actor ptr will pick it up.
    pending_channel_alloca_ = ch_alloca;
  }

  return actor;
}

llvm::Value *CodeGen::emit_identifier(const IdentifierNode &node) {
  std::string name(node.name);

  // Check local variables.
  auto it = locals.find(name);
  if (it != locals.end()) {
    // Union types are passed as pointers to their tagged struct.
    // Return the alloca directly rather than loading.
    auto *alloca = it->second;
    auto *alloc_ty = alloca->getAllocatedType();
    if (alloc_ty->isStructTy()) {
      auto *st = llvm::cast<llvm::StructType>(alloc_ty);
      // Check if this is a union struct: { i8, [N x i8] }
      if (st->getNumElements() == 2 &&
          st->getElementType(0)->isIntegerTy(8) &&
          st->getElementType(1)->isArrayTy()) {
        return alloca; // Return pointer to union struct.
      }
      // Closure fat pointer — return the alloca pointer.
      if (st == closure_fat_ptr_type) {
        return alloca;
      }
    }
    return builder.CreateLoad(alloc_ty, alloca, name);
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
// Union helpers
// ===========================================================================

llvm::StructType *CodeGen::get_union_llvm_type(const TypePtr &union_sem) {
  if (!union_sem || union_sem->kind != TypeKind::Union)
    return nullptr;

  std::string key = type_to_string(union_sem);
  auto it = union_llvm_types.find(key);
  if (it != union_llvm_types.end())
    return it->second;

  uint64_t payload = union_payload_size(union_sem);
  // { i8 tag, [payload x i8] }
  auto *payload_ty = llvm::ArrayType::get(
      llvm::Type::getInt8Ty(context), payload);
  auto *st = llvm::StructType::create(
      context,
      {llvm::Type::getInt8Ty(context), payload_ty},
      "mc.union." + key);
  union_llvm_types[key] = st;
  return st;
}

uint64_t CodeGen::union_payload_size(const TypePtr &union_sem) {
  if (!union_sem || union_sem->kind != TypeKind::Union)
    return 8;
  auto &info = std::get<UnionTypeInfo>(union_sem->detail);
  uint64_t max_size = 0;
  auto &dl = module->getDataLayout();
  for (auto &alt : info.alternatives) {
    auto *ll = llvm_type(alt);
    if (ll->isVoidTy())
      continue;
    uint64_t sz = dl.getTypeAllocSize(ll);
    if (sz > max_size)
      max_size = sz;
  }
  return max_size > 0 ? max_size : 8;
}

int CodeGen::union_tag_for_type(const TypePtr &alt_type,
                                 const TypePtr &union_type) {
  if (!union_type || union_type->kind != TypeKind::Union)
    return -1;
  auto &info = std::get<UnionTypeInfo>(union_type->detail);
  for (size_t i = 0; i < info.alternatives.size(); ++i) {
    if (types_equal(info.alternatives[i], alt_type))
      return static_cast<int>(i);
    // Also check interface satisfaction (e.g. Missing satisfies Error).
    if (info.alternatives[i]->kind == TypeKind::Interface &&
        is_assignable_to(alt_type, info.alternatives[i]))
      return static_cast<int>(i);
  }
  return -1;
}

llvm::Value *CodeGen::emit_union_wrap(llvm::Value *val,
                                       const TypePtr &val_type,
                                       const TypePtr &union_type) {
  if (!val || !union_type || union_type->kind != TypeKind::Union)
    return val;

  auto *union_st = get_union_llvm_type(union_type);
  if (!union_st)
    return val;

  int tag = union_tag_for_type(val_type, union_type);
  if (tag < 0)
    return val; // Type not in union — shouldn't happen after analysis.

  auto *func = builder.GetInsertBlock()->getParent();
  auto *alloca = create_entry_alloca(func, "union.tmp", union_st);

  // Zero-initialize the whole struct so padding bytes are clean.
  builder.CreateStore(llvm::Constant::getNullValue(union_st), alloca);

  // Store the tag.
  auto *tag_gep = builder.CreateStructGEP(union_st, alloca, 0, "union.tag");
  builder.CreateStore(
      llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), tag), tag_gep);

  // Store the value into the payload.
  if (!val->getType()->isVoidTy()) {
    auto *payload_gep = builder.CreateStructGEP(union_st, alloca, 1,
                                                 "union.payload");
    auto *cast = builder.CreateBitOrPointerCast(
        payload_gep, llvm::PointerType::getUnqual(context), "union.pcast");
    builder.CreateStore(val, cast);
  }

  return alloca;
}

llvm::Value *CodeGen::emit_union_extract(llvm::Value *union_ptr,
                                          const TypePtr &alt_type,
                                          const TypePtr &union_type) {
  if (!union_ptr || !union_type || union_type->kind != TypeKind::Union)
    return nullptr;

  auto *union_st = get_union_llvm_type(union_type);
  if (!union_st)
    return nullptr;

  auto *ll_alt = llvm_type(alt_type);
  if (ll_alt->isVoidTy())
    return nullptr;

  auto *payload_gep = builder.CreateStructGEP(union_st, union_ptr, 1,
                                               "union.payload");
  auto *cast = builder.CreateBitOrPointerCast(
      payload_gep, llvm::PointerType::getUnqual(context), "union.ecast");
  return builder.CreateLoad(ll_alt, cast, "union.val");
}

bool CodeGen::is_impure_union(const TypePtr &t) const {
  if (!t || t->kind != TypeKind::Union)
    return false;
  auto &info = std::get<UnionTypeInfo>(t->detail);
  for (auto &alt : info.alternatives) {
    if (alt->kind == TypeKind::Interface) {
      auto &iface = std::get<InterfaceTypeInfo>(alt->detail);
      if (iface.name == "Error")
        return true;
    }
  }
  return false;
}

TypePtr CodeGen::strip_error_from_union(const TypePtr &t) const {
  if (!t || t->kind != TypeKind::Union)
    return t;
  auto &info = std::get<UnionTypeInfo>(t->detail);
  std::vector<TypePtr> purified;
  for (auto &alt : info.alternatives) {
    if (alt->kind == TypeKind::Interface) {
      auto &iface = std::get<InterfaceTypeInfo>(alt->detail);
      if (iface.name == "Error")
        continue;
    }
    purified.push_back(alt);
  }
  if (purified.empty())
    return nullptr;
  if (purified.size() == 1)
    return purified[0];
  return make_union_type(std::move(purified));
}

// ===========================================================================
// Reference counting
// ===========================================================================

void CodeGen::track_managed(const std::string &name, const TypePtr &sem) {
  if (!sem) return;
  if (sem->kind == TypeKind::String)
    managed_locals.push_back({name, ManagedKind::String});
  else if (sem->kind == TypeKind::Array)
    managed_locals.push_back({name, ManagedKind::Array});
  else if (sem->kind == TypeKind::Map)
    managed_locals.push_back({name, ManagedKind::Map});
  else if (sem->kind == TypeKind::Struct) {
    auto &info = std::get<StructTypeInfo>(sem->detail);
    if (info.name == "Task")
      managed_locals.push_back({name, ManagedKind::Task});
  }
}

void CodeGen::emit_retain(llvm::Value *val, const TypePtr &sem) {
  if (!val || !sem) return;
  if (sem->kind == TypeKind::String) {
    builder.CreateCall(module->getFunction("mc_retain_string"), {val});
  } else if (sem->kind == TypeKind::Array) {
    builder.CreateCall(module->getFunction("mc_retain_array"), {val});
  } else if (sem->kind == TypeKind::Map) {
    builder.CreateCall(module->getFunction("mc_retain_map"), {val});
  }
}

void CodeGen::emit_release(llvm::Value *val, const TypePtr &sem) {
  if (!val || !sem) return;
  if (sem->kind == TypeKind::String) {
    builder.CreateCall(module->getFunction("mc_release_string"), {val});
  } else if (sem->kind == TypeKind::Array) {
    builder.CreateCall(module->getFunction("mc_release_array"), {val});
  } else if (sem->kind == TypeKind::Map) {
    builder.CreateCall(module->getFunction("mc_release_map"), {val});
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
    else if (ml.kind == ManagedKind::Map)
      builder.CreateCall(module->getFunction("mc_release_map"), {val});
    else if (ml.kind == ManagedKind::Task)
      builder.CreateCall(module->getFunction("mc_task_drop"), {val});
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
