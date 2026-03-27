// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"

#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>

namespace mc {

// ===========================================================================
// Construction
// ===========================================================================

CodeGen::CodeGen(const std::string &module_name, Analyzer &analyzer)
    : module(std::make_unique<llvm::Module>(module_name, context)),
      builder(context),
      analyzer(analyzer) {}

// ===========================================================================
// Entry point
// ===========================================================================

void CodeGen::emit(const Node &root) {
  std::visit(
      overloaded{
          [&](const PackageNode &pkg) { emit_package(pkg); },
          [&](const SourceNode &src) { emit_source(src); },
          [&](const auto &) {
            // Nothing to emit for other root types.
          },
      },
      root.data);
}

// ===========================================================================
// Top-level visitors
// ===========================================================================

void CodeGen::emit_package(const PackageNode &pkg) {
  for (auto &src : pkg.sources) {
    auto &src_node = std::get<SourceNode>(src->data);
    emit_source(src_node);
  }
}

void CodeGen::emit_source(const SourceNode &src) {
  for (auto &decl : src.declarations) {
    std::visit(
        overloaded{
            [&](const FuncDeclNode &fn) { emit_func_decl(fn); },
            [&](const auto &) {
              // Other declarations deferred to later slices.
            },
        },
        decl->data);
  }
}

// ===========================================================================
// Function emission
// ===========================================================================

void CodeGen::emit_func_decl(const FuncDeclNode &fn) {
  std::string name(fn.name.name);

  // Determine the LLVM return type.
  // For now we only handle Void functions.  The return type is derived
  // from the analyzer's resolved signature, but for Slice 1 we hard-code
  // Void → i32 for Main (C ABI) and void for everything else.
  llvm::Type *ret_type = llvm::Type::getVoidTy(context);
  bool is_main = (name == "Main");

  if (is_main) {
    // C entry point must return i32.
    ret_type = llvm::Type::getInt32Ty(context);
  }

  // Build the function type — no parameters for now.
  auto *fn_type = llvm::FunctionType::get(ret_type, /*isVarArg=*/false);

  // Use "main" as the linker symbol for Main.
  std::string link_name = is_main ? "main" : name;

  auto *func = llvm::Function::Create(
      fn_type, llvm::Function::ExternalLinkage, link_name, module.get());

  // Create the entry basic block.
  auto *entry = llvm::BasicBlock::Create(context, "entry", func);
  builder.SetInsertPoint(entry);

  // Emit the body.  For Slice 1 the body is an empty block, so we just
  // emit the terminator.
  if (is_main) {
    builder.CreateRet(llvm::ConstantInt::get(
        llvm::Type::getInt32Ty(context), 0));
  } else {
    builder.CreateRetVoid();
  }

  // Verify the function.
  llvm::verifyFunction(*func);
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
  // Initialise native target.
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
