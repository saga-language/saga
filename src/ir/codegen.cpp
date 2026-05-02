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

namespace saga {

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
  // Empty origin means a built-in type with no package (e.g. Error, Comparison).
  return pkg.empty() ? name : (pkg + "__" + name);
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

  // Determine sret lowering for struct returns.
  llvm::Type *sret_struct_ty = nullptr;
  llvm::Type *ret_ll = void_ll_type;
  if (!fi.returns.empty() && fi.returns[0]->kind != TypeKind::Void) {
    if (fi.returns.size() == 1) {
      auto *r = llvm_type(fi.returns[0]);
      if (r && r->isStructTy()) {
        sret_struct_ty = r;
        ret_ll = void_ll_type;
      } else {
        ret_ll = r;
      }
    } else {
      // Multi-return: create a struct type.
      std::vector<llvm::Type *> ret_types;
      for (auto &r : fi.returns)
        ret_types.push_back(llvm_type(r));
      auto *st = llvm::StructType::create(context, ret_types,
                                           "saga.ret." + link_name);
      multi_return_types[link_name] = st;
      multi_return_counts[link_name] = fi.returns.size();
      ret_ll = st;
    }
  }

  // Param types: structs lowered to ptr (byval applied below).
  auto *ptr_ty = llvm::PointerType::getUnqual(context);
  std::vector<llvm::Type *> param_types;
  std::vector<llvm::Type *> byval_attached(fi.params.size(), nullptr);
  if (sret_struct_ty)
    param_types.push_back(ptr_ty);
  for (size_t i = 0; i < fi.params.size(); ++i) {
    auto *p = llvm_type(fi.params[i]);
    if (p && p->isStructTy()) {
      byval_attached[i] = p;
      param_types.push_back(ptr_ty);
    } else {
      param_types.push_back(p);
    }
  }

  auto *fn_type = llvm::FunctionType::get(ret_ll, param_types, /*isVarArg=*/false);
  auto *func = llvm::Function::Create(
      fn_type, llvm::Function::ExternalLinkage, link_name, module.get());

  unsigned idx = 0;
  if (sret_struct_ty) {
    llvm::AttrBuilder ab(context);
    ab.addStructRetAttr(sret_struct_ty);
    ab.addAlignmentAttr(
        module->getDataLayout().getABITypeAlign(sret_struct_ty));
    func->addParamAttrs(idx++, ab);
  }
  for (size_t i = 0; i < fi.params.size(); ++i) {
    if (byval_attached[i]) {
      llvm::AttrBuilder ab(context);
      ab.addByValAttr(byval_attached[i]);
      ab.addAlignmentAttr(
          module->getDataLayout().getABITypeAlign(byval_attached[i]));
      func->addParamAttrs(idx, ab);
    }
    ++idx;
  }
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

  // saga_runtime_string = { i8*, i64, i64 }  — data, len, refcount
  string_type = llvm::StructType::create(
      context,
      {llvm::PointerType::getUnqual(context), i64_type, i64_type},
      "saga_runtime_string");

  // Interface fat pointer: { ptr data, ptr vtable }
  auto *ptr_ty = llvm::PointerType::getUnqual(context);
  iface_fat_ptr_type = llvm::StructType::create(
      context, {ptr_ty, ptr_ty}, "saga_runtime_iface");

  // Closure fat pointer: { ptr fn, ptr env }
  closure_fat_ptr_type = llvm::StructType::create(
      context, {ptr_ty, ptr_ty}, "saga_runtime_closure");

  // Register built-in enums with the current package as origin key.
  // key_for("", "Comparison") resolves to mangle(package_name, "Comparison").
  std::string cmp_key = mangle(package_name, "Comparison");
  enum_types[cmp_key] = true;
  enum_variants[cmp_key + ".Less"] = 0;
  enum_variants[cmp_key + ".Equal"] = 1;
  enum_variants[cmp_key + ".Greater"] = 2;
}

std::string CodeGen::struct_cache_key(const StructTypeInfo &info) const {
  std::string base = key_for(info.origin_package, info.name);
  if (info.type_args.empty())
    return base;
  base += "<";
  for (size_t i = 0; i < info.type_args.size(); ++i) {
    if (i)
      base += ",";
    base += type_to_string(info.type_args[i]);
  }
  base += ">";
  return base;
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
    return llvm::PointerType::getUnqual(context); // ptr to saga_runtime_string
  case TypeKind::Void:
    return void_ll_type;
  case TypeKind::Enum:
    return i64_type; // Enums are represented as i64 tags.
  case TypeKind::Struct: {
    auto &info = std::get<StructTypeInfo>(t->detail);
    std::string key = struct_cache_key(info);
    auto it = struct_types.find(key);
    llvm::StructType *st = nullptr;
    if (it != struct_types.end()) {
      st = it->second;
      if (!st->isOpaque())
        return st;
    } else {
      // Forward declare opaque first; insert before recursing into fields
      // so any self-reference resolves to this same opaque shell.
      st = llvm::StructType::create(context, "saga." + key);
      struct_types[key] = st;
    }
    // Fill body eagerly using semantic field info, so callers that need
    // size/alignment (unions, byval/sret lowering) get a sized struct.
    std::vector<llvm::Type *> ftypes;
    std::vector<std::string> fnames;
    for (auto &f : info.fields) {
      ftypes.push_back(llvm_type(f.type));
      fnames.push_back(f.name);
    }
    st->setBody(ftypes);
    if (struct_fields.find(key) == struct_fields.end())
      struct_fields[key] = std::move(fnames);
    if (named_sem_types.find(key) == named_sem_types.end())
      named_sem_types[key] = t;
    return st;
  }
  case TypeKind::Interface:
    // Interfaces are represented as a fat pointer struct.
    return llvm::PointerType::getUnqual(context); // ptr to saga_runtime_iface
  case TypeKind::Union: {
    auto *st = get_union_llvm_type(t);
    if (st)
      return st;
    return void_ll_type;
  }
  case TypeKind::Array:
    return llvm::PointerType::getUnqual(context); // ptr to saga_runtime_array
  case TypeKind::Map:
    return llvm::PointerType::getUnqual(context); // ptr to saga_runtime_map
  case TypeKind::Func:
    return llvm::PointerType::getUnqual(context); // ptr to saga_runtime_closure
  case TypeKind::TypeParam:
    // Unresolved generic type parameter (e.g. T in stdlib [T] methods).
    // At runtime, generic values are passed as opaque pointers.
    return llvm::PointerType::getUnqual(context);
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

  if (init_function_needed)
    emit_init_function();
}

llvm::Function *CodeGen::declare_void_init(const std::string &link_name) {
  if (auto *existing = module->getFunction(link_name))
    return existing;
  auto *fn_type = llvm::FunctionType::get(void_ll_type, {}, /*isVarArg=*/false);
  return llvm::Function::Create(
      fn_type, llvm::Function::ExternalLinkage, link_name, module.get());
}

void CodeGen::emit_init_calls() {
  for (auto &sym : imported_init_symbols)
    builder.CreateCall(declare_void_init(sym), {});
  if (init_function_needed)
    builder.CreateCall(declare_void_init(package_name + "__init__"), {});
}

void CodeGen::emit_init_function() {
  // Symbol convention: `<pkg>__init__`.  Built directly rather than via
  // mangle() because mangle would inject a second `__` separator.  May
  // already exist as a forward-declaration created by the Main wrapper.
  llvm::Function *func =
      declare_void_init(package_name + "__init__");
  if (!func->empty())
    return;

  auto *entry = llvm::BasicBlock::Create(context, "entry", func);
  builder.SetInsertPoint(entry);

  locals.clear();
  managed_locals.clear();
  current_func_is_main = false;
  current_func_return_sem = nullptr;

  for (const ConstDeclNode *node : deferred_const_inits_) {
    std::string global_name = mangle(std::string(node->name.name));
    auto *gv = module->getGlobalVariable(global_name);
    if (!gv)
      continue;
    auto *val = emit_expr(*node->value);
    if (!val)
      continue;
    builder.CreateStore(val, gv);
  }

  builder.CreateRetVoid();
  llvm::verifyFunction(*func);
}

// ===========================================================================
// Top-level visitors
// ===========================================================================

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

void CodeGen::emit_package(const PackageNode &pkg) {
  // Pass 0: materialize imported module types before any local passes.
  for (auto &src : pkg.sources) {
    auto &s = std::get<SourceNode>(src->data);
    materialize_imports_from_source(s);
  }

  // Pass 1: register all type declarations (structs, enums, interfaces)
  // across every source file before emitting any function bodies. This
  // ensures cross-file references within the same package resolve correctly.
  for (auto &src : pkg.sources) {
    auto &s = std::get<SourceNode>(src->data);
    declare_structs(s);
    declare_enums(s);
    declare_interfaces(s);
  }

  // Pass 2: forward-declare functions and methods across all files so that
  // bodies in any source can reference symbols declared in any other.
  for (auto &src : pkg.sources) {
    auto &s = std::get<SourceNode>(src->data);
    if (!has_spawn) {
      for (auto &decl : s.declarations) {
        if (source_has_spawn(*decl)) { has_spawn = true; break; }
      }
    }
    declare_functions(s);
    declare_struct_method_symbols(s);
    register_struct_method_links(s);
    declare_intrinsic_methods(s);
  }

  // Pass 2b: emit package-level constants across all files.  Must run
  // before any function body so `init_function_needed` is set when the
  // Main wrapper is generated (Main may live in a different source file
  // than the collection consts that need a runtime initialiser).
  for (auto &src : pkg.sources) {
    auto &s = std::get<SourceNode>(src->data);
    for (auto &decl : s.declarations) {
      if (auto *c = std::get_if<ConstDeclNode>(&decl->data))
        emit_const_decl(*c);
    }
  }

  // Pass 3: emit function and method bodies.
  for (auto &src : pkg.sources) {
    auto &s = std::get<SourceNode>(src->data);
    for (auto &decl : s.declarations) {
      if (auto *fn = std::get_if<FuncDeclNode>(&decl->data))
        emit_func_decl(*fn);
    }
    emit_struct_methods(s);
    emit_intrinsic_methods(s);
  }
}

void CodeGen::emit_source(const SourceNode &src) {
  // Pre-scan for spawn expressions so executor init/shutdown can be placed.
  if (!has_spawn) {
    for (auto &decl : src.declarations) {
      if (source_has_spawn(*decl)) {
        has_spawn = true;
        break;
      }
    }
  }

  // Pass 0: materialize imported module types before local declare passes.
  materialize_imports_from_source(src);

  // Pass 1: create LLVM struct types, register enums and interfaces.
  declare_structs(src);
  declare_enums(src);
  declare_interfaces(src);

  // Pass 2: forward-declare all functions, struct methods, and intrinsic
  // type methods.
  declare_functions(src);
  declare_struct_method_symbols(src);
  register_struct_method_links(src);
  declare_intrinsic_methods(src);

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
  emit_intrinsic_methods(src);
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

} // namespace saga
