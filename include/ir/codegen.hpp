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

  /// Package name for symbol mangling (e.g. "io", "os").
  std::string package_name;

  // ── LLVM type cache ──────────────────────────────────────────────────

  llvm::StructType *string_type = nullptr;   // { ptr, i64 }
  llvm::Type *i64_type = nullptr;
  llvm::Type *f64_type = nullptr;
  llvm::Type *i1_type = nullptr;
  llvm::Type *void_ll_type = nullptr;

  // ── Struct type registry ──────────────────────────────────────────

  /// Maps struct name → LLVM struct type.
  std::unordered_map<std::string, llvm::StructType *> struct_types;

  /// Maps struct name → ordered list of field names (matches LLVM layout).
  std::unordered_map<std::string, std::vector<std::string>> struct_fields;

  // ── Enum registry ────────────────────────────────────────────────────

  /// Maps "EnumName.VariantName" → integer tag value.
  std::unordered_map<std::string, int64_t> enum_variants;

  /// Maps enum name → true (tracks which enums have been declared).
  std::unordered_map<std::string, bool> enum_types;

  // ── Multi-return registry ────────────────────────────────────────────

  /// Maps function link-name → LLVM struct type used for multi-return.
  std::unordered_map<std::string, llvm::StructType *> multi_return_types;

  /// Maps function link-name → number of return values (only for multi).
  std::unordered_map<std::string, size_t> multi_return_counts;

  // ── Interface registry ──────────────────────────────────────────────

  /// The fat pointer type for interface values: { ptr data, ptr vtable }.
  llvm::StructType *iface_fat_ptr_type = nullptr;

  /// Maps interface name → vtable struct type (struct of fn ptrs).
  std::unordered_map<std::string, llvm::StructType *> iface_vtable_types;

  /// Maps interface name → ordered list of method names.
  std::unordered_map<std::string, std::vector<std::string>> iface_method_names;

  /// Maps "StructName::InterfaceName" → vtable global constant.
  std::unordered_map<std::string, llvm::GlobalVariable *> vtable_globals;

  /// Maps interface/struct name → semantic TypePtr (for codegen type lookup).
  std::unordered_map<std::string, TypePtr> named_sem_types;

  /// Maps struct name → list of {mangled_link_name, method_name} pairs.
  std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>>
      struct_method_links;

  // ── Union type registry ──────────────────────────────────────────────

  /// Maps canonical union type string → LLVM struct type { i8 tag, [N x i8] }.
  std::unordered_map<std::string, llvm::StructType *> union_llvm_types;

  // ── String constant deduplication ────────────────────────────────────

  std::unordered_map<std::string, llvm::Value *> string_constants;

  // ── Local variable storage (per-function) ────────────────────────────

  /// Maps local variable names to their alloca instructions.
  std::unordered_map<std::string, llvm::AllocaInst *> locals;

  /// Tracks which locals need release at scope exit and their kind.
  enum class ManagedKind { String, Array, Map, Task, Closeable };
  struct ManagedLocal {
    std::string name;
    ManagedKind kind;
  };
  std::vector<ManagedLocal> managed_locals;

  /// True when the current function is Main (return type is i32).
  bool current_func_is_main = false;

  // ── Closure support ──────────────────────────────────────────────────

  /// The fat pointer type for closures: { ptr fn, ptr env }.
  llvm::StructType *closure_fat_ptr_type = nullptr;

  /// Counter for generating unique closure names.
  int next_closure_id = 0;

  /// Counter for generating unique spawn outlined function names.
  int next_spawn_id = 0;

  // ── Spawn support ─────────────────────────────────────────────────────

  /// When non-null, we are inside a spawn outlined function.
  /// Points to the mc_actor* parameter of the outlined function.
  llvm::Value *current_actor = nullptr;

  /// True if the current module contains any spawn expressions
  /// (triggers executor init/shutdown in main).
  bool has_spawn = false;

  /// Pending channel alloca from the most recent spawn with a generic type.
  /// Picked up by the next DeclAssign to create a companion ".channel" local.
  llvm::AllocaInst *pending_channel_alloca_ = nullptr;

  // ── Loop context (for break/next) ────────────────────────────────────

  struct LoopContext {
    llvm::BasicBlock *break_bb;   // target of `break`
    llvm::BasicBlock *next_bb;    // target of `next`
  };
  std::vector<LoopContext> loop_stack;

  // ── Construction ─────────────────────────────────────────────────────

  CodeGen(const std::string &module_name, Analyzer &analyzer);

  /// Compute the mangled link name for a symbol in this package.
  std::string mangle(const std::string &name) const;

  /// Compute the mangled link name for a symbol in another package.
  static std::string mangle(const std::string &pkg, const std::string &name);

  /// Declare an external function from an imported package.
  /// Returns the LLVM Function*, creating it if needed.
  llvm::Function *declare_import(const std::string &pkg_name,
                                  const std::string &symbol_name,
                                  const TypePtr &func_type);

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

  /// Create LLVM struct types for all struct declarations.
  void declare_structs(const SourceNode &src);
  void emit_struct_decl(const StructDeclNode &node);

  /// Emit a package-level constant as an LLVM global variable.
  void emit_const_decl(const ConstDeclNode &node);

  /// Register enum variant tags.
  void declare_enums(const SourceNode &src);
  void emit_enum_decl(const EnumDeclNode &node);

  /// Register interface vtable types.
  void declare_interfaces(const SourceNode &src);
  void emit_interface_decl(const InterfaceDeclNode &node);

  /// Declare struct methods as regular functions with mangled names.
  void declare_struct_methods(const SourceNode &src);

  /// Emit struct method bodies.
  void emit_struct_methods(const SourceNode &src);

  /// Get or create a vtable for a concrete struct implementing an interface.
  llvm::GlobalVariable *get_or_create_vtable(const std::string &struct_name,
                                              const std::string &iface_name);

  /// Box a concrete value into an interface fat pointer.
  llvm::Value *emit_interface_box(llvm::Value *concrete_val,
                                   const TypePtr &concrete_type,
                                   const TypePtr &iface_type);

  /// Build the LLVM FunctionType for a Saga function declaration.
  llvm::FunctionType *build_func_type(const FuncDeclNode &fn);

  /// Resolve a type annotation node to an LLVM type.
  llvm::Type *resolve_type_node(const Node &type_node);

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
  llvm::Value *emit_binary_expr(const BinaryExprNode &node,
                                const Node &parent);

  /// Emit a binary operator that was resolved to a struct method call
  /// (operator overloading). `method` is e.g. "Add", "Compare", "Equals".
  llvm::Value *emit_struct_binary_op(const BinaryExprNode &node,
                                     const Node &parent,
                                     const TypePtr &lhs_sem,
                                     const std::string &method);
  llvm::Value *emit_unary_expr(const UnaryExprNode &node);
  llvm::Value *emit_group_expr(const GroupExprNode &node);
  llvm::Value *emit_if_expr(const IfExprNode &node);
  llvm::Value *emit_for_expr(const ForExprNode &node);
  llvm::Value *emit_struct_literal(const StructLiteralNode &node);
  llvm::Value *emit_selector(const SelectorNode &node, const Node &parent);
  llvm::Value *emit_switch_expr(const SwitchExprNode &node);
  llvm::Value *emit_array_literal(const ArrayLiteralNode &node);
  llvm::Value *emit_map_literal(const MapLiteralNode &node);
  llvm::Value *emit_index_expr(const IndexExprNode &node);
  llvm::Value *emit_or_expr(const OrExprNode &node);
  llvm::Value *emit_func_expr(const FuncExprNode &node, const Node &parent);
  llvm::Value *emit_spawn_expr(const SpawnExprNode &node, const Node &parent);

  /// Get a GEP to a struct field. Returns {ptr to field, field LLVM type}.
  std::pair<llvm::Value *, llvm::Type *>
  struct_field_gep(llvm::Value *struct_ptr, const TypePtr &struct_sem_type,
                   const std::string &field_name);

  // ── Type query helpers ───────────────────────────────────────────────

  /// Look up the semantic type of an AST node (recorded by the analyzer).
  TypePtr semantic_type(const Node &node) const;

  // ── Union helpers ────────────────────────────────────────────────────

  /// Get or create the LLVM struct type for a union: { i8 tag, [N x i8] payload }.
  llvm::StructType *get_union_llvm_type(const TypePtr &union_sem);

  /// Compute the payload byte size for a union (max of all alternatives).
  uint64_t union_payload_size(const TypePtr &union_sem);

  /// Wrap a concrete value into a union alloca, returning the alloca ptr.
  llvm::Value *emit_union_wrap(llvm::Value *val, const TypePtr &val_type,
                                const TypePtr &union_type);

  /// Extract a concrete value from a union alloca given the expected alt type.
  llvm::Value *emit_union_extract(llvm::Value *union_ptr,
                                   const TypePtr &alt_type,
                                   const TypePtr &union_type);

  /// Get the tag index for a type within a union.
  int union_tag_for_type(const TypePtr &alt_type, const TypePtr &union_type);

  /// Check if a semantic type contains Error (is "impure").
  bool is_impure_union(const TypePtr &t) const;

  /// Strip Error alternatives from a union, returning the purified type.
  TypePtr strip_error_from_union(const TypePtr &t) const;

  // ── String helpers ───────────────────────────────────────────────────

  llvm::Value *make_string_constant(const std::string &text);

  /// Convert a value to an mc_string* based on its semantic type.
  llvm::Value *emit_to_string(llvm::Value *val, const TypePtr &sem);

  // ── Reference counting helpers ───────────────────────────────────────

  /// Register a local variable as managed (needs release at scope exit).
  void track_managed(const std::string &name, const TypePtr &sem);

  /// Emit retain call for a value based on its semantic type.
  void emit_retain(llvm::Value *val, const TypePtr &sem);

  /// Emit release call for a value based on its semantic type.
  void emit_release(llvm::Value *val, const TypePtr &sem);

  /// Emit release calls for all managed locals in the current function.
  void emit_release_locals();
};

} // namespace mc
