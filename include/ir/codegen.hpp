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
#include <unordered_map>

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

  // ── LLVM type cache ──────────────────────────────────────────────────

  /// The canonical mc_string struct type: { ptr, i64 }
  llvm::StructType *string_type = nullptr;

  // ── String constant deduplication ────────────────────────────────────

  std::unordered_map<std::string, llvm::Value *> string_constants;

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
  // ── Type helpers ─────────────────────────────────────────────────────

  /// Initialise cached LLVM types (string struct, etc.).
  void init_types();

  /// Declare external runtime functions (mc_intrinsic_print, etc.).
  void declare_runtime();

  // ── Visitors ─────────────────────────────────────────────────────────

  void emit_source(const SourceNode &node);
  void emit_package(const PackageNode &node);
  void emit_func_decl(const FuncDeclNode &node);

  // ── Block / statement emission ───────────────────────────────────────

  void emit_block(const BlockNode &block);
  void emit_stmt(const Node &node);

  // ── Expression emission ──────────────────────────────────────────────

  /// Emit an expression and return its LLVM value (may be nullptr for Void).
  llvm::Value *emit_expr(const Node &node);
  llvm::Value *emit_string_literal(const StringLiteralNode &node);
  llvm::Value *emit_call_expr(const CallExprNode &node);
  llvm::Value *emit_identifier(const IdentifierNode &node);

  // ── String helpers ───────────────────────────────────────────────────

  /// Create a global constant mc_string struct for the given text.
  llvm::Value *make_string_constant(const std::string &text);
};

} // namespace mc
