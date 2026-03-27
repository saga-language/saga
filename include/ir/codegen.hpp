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
#include <vector>

namespace mc {

// ---------------------------------------------------------------------------
// CodeGen — lowers a type-checked AST to LLVM IR.
// ---------------------------------------------------------------------------

struct CodeGen {
  llvm::LLVMContext context;
  std::unique_ptr<llvm::Module> module;
  llvm::IRBuilder<> builder;

  Analyzer &analyzer;
  ErrorList errors;

  // ── LLVM type cache ──────────────────────────────────────────────────

  llvm::StructType *string_type = nullptr;   // { ptr, i64 }
  llvm::Type *i64_type = nullptr;
  llvm::Type *f64_type = nullptr;
  llvm::Type *i1_type = nullptr;
  llvm::Type *void_ll_type = nullptr;

  // ── String constant deduplication ────────────────────────────────────

  std::unordered_map<std::string, llvm::Value *> string_constants;

  // ── Local variable storage (per-function) ────────────────────────────

  /// Maps local variable names to their alloca instructions.
  std::unordered_map<std::string, llvm::AllocaInst *> locals;

  /// True when the current function is Main (return type is i32).
  bool current_func_is_main = false;

  // ── Loop context (for break/next) ────────────────────────────────────

  struct LoopContext {
    llvm::BasicBlock *break_bb;   // target of `break`
    llvm::BasicBlock *next_bb;    // target of `next`
  };
  std::vector<LoopContext> loop_stack;

  // ── Construction ─────────────────────────────────────────────────────

  CodeGen(const std::string &module_name, Analyzer &analyzer);

  // ── Entry point ──────────────────────────────────────────────────────

  void emit(const Node &root);

  // ── Output ───────────────────────────────────────────────────────────

  void dump() const;
  bool write_ir(const std::string &path) const;
  bool write_object(const std::string &path);

private:
  // ── Type helpers ─────────────────────────────────────────────────────

  void init_types();
  void declare_runtime();

  /// Get the LLVM type corresponding to a semantic TypePtr.
  llvm::Type *llvm_type(const TypePtr &t);

  /// Create an alloca at the function entry block.
  llvm::AllocaInst *create_entry_alloca(llvm::Function *fn,
                                        const std::string &name,
                                        llvm::Type *type);

  // ── Visitors ─────────────────────────────────────────────────────────

  void emit_source(const SourceNode &node);
  void emit_package(const PackageNode &node);

  /// Forward-declare all functions in a source (signatures only).
  void declare_functions(const SourceNode &src);

  /// Build the LLVM FunctionType for a Saga function declaration.
  llvm::FunctionType *build_func_type(const FuncDeclNode &fn);

  /// Emit a full function definition (entry block + body).
  void emit_func_decl(const FuncDeclNode &node);

  // ── Block / statement emission ───────────────────────────────────────

  /// Emit a block, returning the value of the last expression (or nullptr).
  llvm::Value *emit_block(const BlockNode &block);
  void emit_stmt(const Node &node);

  // ── Statement emitters ───────────────────────────────────────────────

  void emit_var_decl(const VarDeclNode &node);
  void emit_decl_assign(const DeclAssignNode &node);
  void emit_assign(const AssignNode &node);
  void emit_return(const ReturnNode &node);
  void emit_increment(const IncrementNode &node);
  void emit_decrement(const DecrementNode &node);

  // ── Expression emission ──────────────────────────────────────────────

  llvm::Value *emit_expr(const Node &node);
  llvm::Value *emit_int_literal(const IntegerLiteralNode &node);
  llvm::Value *emit_float_literal(const FloatLiteralNode &node);
  llvm::Value *emit_bool_literal(const BoolLiteralNode &node);
  llvm::Value *emit_string_literal(const StringLiteralNode &node);
  llvm::Value *emit_call_expr(const CallExprNode &node);
  llvm::Value *emit_identifier(const IdentifierNode &node);
  llvm::Value *emit_binary_expr(const BinaryExprNode &node);
  llvm::Value *emit_unary_expr(const UnaryExprNode &node);
  llvm::Value *emit_group_expr(const GroupExprNode &node);
  llvm::Value *emit_if_expr(const IfExprNode &node);
  llvm::Value *emit_for_expr(const ForExprNode &node);

  // ── Type query helpers ───────────────────────────────────────────────

  /// Look up the semantic type of an AST node (recorded by the analyzer).
  TypePtr semantic_type(const Node &node) const;

  // ── String helpers ───────────────────────────────────────────────────

  llvm::Value *make_string_constant(const std::string &text);
};

} // namespace mc
