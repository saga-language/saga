// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#pragma once

#include "frontend/ast.hpp"
#include "frontend/error_list.hpp"
#include "semantic/analyzer.hpp"

#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <memory>
#include <string>

namespace mc {

// ---------------------------------------------------------------------------
// CodeGen — lowers a type-checked AST to LLVM IR.
//
// Usage:
//   CodeGen codegen("module_name", analyzer);
//   codegen.emit(root_node);
//   codegen.dump();                     // print IR to stderr
//   codegen.write_object("out.o");      // write native object file
//   codegen.write_ir("out.ll");         // write textual IR
// ---------------------------------------------------------------------------

struct CodeGen {
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;
  llvm::IRBuilder<> builder;

  Analyzer &analyzer;
  ErrorList errors;

  // ── Construction ─────────────────────────────────────────────────────

  CodeGen(const std::string &module_name, Analyzer &analyzer);

  // ── Entry point ──────────────────────────────────────────────────────

  /// Emit LLVM IR for the entire AST.
  void emit(const Node &root);

  // ── Output ───────────────────────────────────────────────────────────

  /// Print the module IR to stderr.
  void dump() const;

  /// Write textual LLVM IR to a file.  Returns false on I/O error.
  bool write_ir(const std::string &path) const;

  /// Write a native object file.  Returns false on error.
  bool write_object(const std::string &path);

private:
  // ── Visitors ─────────────────────────────────────────────────────────

  void emit_source(const SourceNode &node);
  void emit_package(const PackageNode &node);
  void emit_func_decl(const FuncDeclNode &node);
};

} // namespace mc
