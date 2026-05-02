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

  /// Maps intrinsic type name (e.g. "String") → list of {link_name, method_name}.
  std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>>
      intrinsic_method_links;

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

  /// Semantic return type of the current function (used by intrinsic_runtime_try).
  TypePtr current_func_return_sem = nullptr;

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

  /// True when the package contains at least one constant whose
  /// initialiser must run at program start (arrays, maps).  Drives
  /// emission of the per-package `<pkg>__init__` function.
  bool init_function_needed = false;

  /// Mangled link names of imported packages whose `<pkg>__init__` must be
  /// invoked before user `Main` runs.  Topological order: deepest dependency
  /// first.  Populated by the build driver before `emit()` runs; consumed by
  /// the Main wrapper.  Empty for library mode (no Main).
  std::vector<std::string> imported_init_symbols;

  /// Pending channel alloca from the most recent spawn with a generic type.
  /// Picked up by the next DeclAssign to create a companion ".channel" local.
  llvm::AllocaInst *pending_channel_alloca_ = nullptr;

  // ── Loop context (for break/next) ────────────────────────────────────

  struct LoopContext {
    llvm::BasicBlock *break_bb;   // target of `break`
    llvm::BasicBlock *next_bb;    // target of `next`
  };
  std::vector<LoopContext> loop_stack;

  // ── Per-instantiation side-table view ────────────────────────────────

  /// When emitting a monomorphised generic specialisation, points at the
  /// analyzer's BodyInstantiation for this (generic, bindings) pair.  All
  /// side-table accessors below check this map first and fall through to
  /// the global analyzer tables for nodes that don't belong to the
  /// generic body (see monomorphism_plan.md, Step 4).
  const Analyzer::BodyInstantiation *current_instantiation_ = nullptr;

  // ── FuncEmissionScope (RAII, Step 5a) ────────────────────────────────

  /// Guards re-entrant function emission.  On construction captures every
  /// piece of per-function CodeGen state and resets to fresh-function
  /// defaults; on destruction restores it.  Used by emit_specialisation
  /// so that emitting a generic specialisation in the middle of another
  /// function's emission does not leak allocas, loop labels, or the
  /// instantiation-view pointer.
  ///
  /// NOTE: when you add new per-function CodeGen state, update the
  /// save/restore list in src/ir/codegen_calls.cpp.
  class FuncEmissionScope {
  public:
    explicit FuncEmissionScope(CodeGen &cg);
    ~FuncEmissionScope();
    FuncEmissionScope(const FuncEmissionScope &) = delete;
    FuncEmissionScope &operator=(const FuncEmissionScope &) = delete;

  private:
    CodeGen &cg_;
    llvm::BasicBlock *saved_bb_;
    llvm::BasicBlock::iterator saved_ip_;
    std::unordered_map<std::string, llvm::AllocaInst *> saved_locals_;
    std::vector<ManagedLocal> saved_managed_locals_;
    std::vector<LoopContext> saved_loop_stack_;
    bool saved_current_func_is_main_;
    TypePtr saved_current_func_return_sem_;
    const Analyzer::BodyInstantiation *saved_current_instantiation_;
    llvm::Value *saved_current_actor_;
    llvm::AllocaInst *saved_pending_channel_alloca_;
  };

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

  /// Eagerly populate all codegen registries for every export of an
  /// imported module.  Must be called before any declare_structs/
  /// declare_functions/etc. pass walks the local source AST.
  void materialize_import(const TypePtr &module_type);

  /// Scan a SourceNode for import declarations and call materialize_import
  /// for each resolved module type.
  void materialize_imports_from_source(const SourceNode &src);

  // ── Entry point ──────────────────────────────────────────────────────

  void emit(const Node &root);

  // ── Output ───────────────────────────────────────────────────────────

  void dump() const;
  bool write_ir(const std::string &path) const;
  bool write_object(const std::string &path);

  /// Linker-safe type mangler.  Output is restricted to [A-Za-z0-9_].
  /// Recursive; nested compound types are terminated with `_End`.
  std::string mangle_type(const TypePtr &t) const;

  /// Compute the specialised link name for a generic free function.
  std::string mangle_specialisation(
      const FuncDeclNode &fn,
      const std::vector<TypePtr> &ordered_type_args) const;

private:
  // ── Type helpers ─────────────────────────────────────────────────────

  void init_types();
  void declare_runtime();

  /// Form the origin-qualified registry key for a named type.
  /// Falls back to package_name when origin is empty (local or built-in
  /// types whose origin_package was not set, e.g. in unit-test contexts).
  std::string key_for(const std::string &origin,
                      const std::string &name) const {
    return mangle(origin.empty() ? package_name : origin, name);
  }

  /// True if a semantic type represents string keys (for mc_map).
  static bool is_string_key_type(const TypePtr &t);

  /// Unescape a raw string fragment (strips surrounding quotes, processes
  /// backslash sequences).  Shared by emit_string_literal and emit_const_decl.
  static std::string unescape_fragment(std::string_view raw);

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

  /// Emit `<pkg>__init__`: a single-entry void function whose body
  /// allocates and stores every collection constant whose value cannot
  /// be expressed as a compile-time LLVM constant.  Called once after
  /// all source nodes have been processed.
  void emit_init_function();

  /// Get or forward-declare a `void(void)` function with the given link
  /// name.  Used by the Main wrapper to call each imported package's
  /// `<pkg>__init__` symbol.
  llvm::Function *declare_void_init(const std::string &link_name);

  /// Prepend init calls to the current insertion point: every symbol in
  /// `imported_init_symbols` (topo order, deps first) followed by this
  /// package's own `<pkg>__init__` when `init_function_needed` is true.
  /// Called from the entry block of Main, before any executor setup.
  void emit_init_calls();

  /// Constants whose initialisers must run at program start, in source
  /// order.  Populated by emit_const_decl, flushed by emit_init_function.
  std::vector<const ConstDeclNode *> deferred_const_inits_;

  /// Build an LLVM constant for `val_node` whose Saga type is `expected`.
  /// Supports integer/float/bool/string literals and nested struct literals.
  /// Returns nullptr when the expression is not a compile-time constant.
  llvm::Constant *build_const_value(const Node &val_node,
                                    const TypePtr &expected);

  /// Register enum variant tags.
  void declare_enums(const SourceNode &src);
  void emit_enum_decl(const EnumDeclNode &node);

  /// Register interface vtable types.
  void declare_interfaces(const SourceNode &src);
  void emit_interface_decl(const InterfaceDeclNode &node);

  /// Forward-declare LLVM Function symbols for every non-generic struct
  /// method. Does not touch struct_method_links.
  void declare_struct_method_symbols(const SourceNode &src);

  /// Populate struct_method_links for every non-generic struct method.
  /// Must run after declare_struct_method_symbols so the symbols exist.
  void register_struct_method_links(const SourceNode &src);

  /// Emit struct method bodies.
  void emit_struct_methods(const SourceNode &src);

  /// Declare receiver methods on intrinsic types (Int, Float, Bool, String).
  void declare_intrinsic_methods(const SourceNode &src);

  /// Emit bodies of receiver methods on intrinsic types.
  void emit_intrinsic_methods(const SourceNode &src);

  /// Get or create a vtable for a concrete struct implementing an interface.
  /// struct_type and iface_type must be TypeKind::Struct and ::Interface.
  llvm::GlobalVariable *get_or_create_vtable(const TypePtr &struct_type,
                                              const TypePtr &iface_type);

  /// Box a concrete value into an interface fat pointer.
  llvm::Value *emit_interface_box(llvm::Value *concrete_val,
                                   const TypePtr &concrete_type,
                                   const TypePtr &iface_type);

  /// Build the LLVM FunctionType for a Saga function declaration.
  llvm::FunctionType *build_func_type(const FuncDeclNode &fn);

  /// Apply byval/sret/align param attributes to a freshly-created Function
  /// based on its AST signature.  Must be called once after Function::Create.
  void apply_func_abi_attrs(llvm::Function *func, const FuncDeclNode &fn);

  /// Build the LLVM FunctionType for a struct method (in-bound or out-bound).
  /// Also applies byval/sret attrs to the supplied function (after Create).
  /// Returns the lowered signature and stamps attributes into `out_attrs`.
  struct MethodSig {
    llvm::FunctionType *fn_type = nullptr;
    llvm::Type *sret_struct_ty = nullptr;
    std::vector<llvm::Type *> byval_struct_tys; // one per regular param
  };
  MethodSig build_method_signature(const FuncDeclNode &fn);
  void apply_method_abi_attrs(llvm::Function *func, const MethodSig &sig);
  void name_method_args(llvm::Function *func, const MethodSig &sig,
                        const FuncDeclNode &fn,
                        std::string_view receiver_name);

  /// Resolve a type annotation node to an LLVM type.
  llvm::Type *resolve_type_node(const Node &type_node);

  /// Emit a full function definition (entry block + body).
  void emit_func_decl(const FuncDeclNode &node);

  /// Shared body-emission helper used by both emit_func_decl and
  /// emit_specialisation.  Assumes `func` has been created and the caller
  /// has set up per-function state (locals/loop/etc).  Writes the entry
  /// block, param allocas (using `param_ll` LLVM types), the body, and
  /// the return-handling tail.
  void emit_function_body_inner(const FuncDeclNode &fn, llvm::Function *func,
                                 const std::vector<llvm::Type *> &param_ll,
                                 bool is_main);

  /// Emit one monomorphised specialisation of a generic free function.
  /// Creates (or returns) an LLVM Function with LinkOnceODR linkage whose
  /// signature is derived from `bindings`.  Runs the body under a fresh
  /// FuncEmissionScope with `current_instantiation_` pointed at `inst`.
  llvm::Function *
  emit_specialisation(const FuncDeclNode &fn, const TypePtr &generic_fn_type,
                      const std::unordered_map<uint32_t, TypePtr> &bindings,
                      const Analyzer::BodyInstantiation *inst);

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
  llvm::Value *emit_call_expr(const CallExprNode &node, const Node &parent);
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

  // ── for-loop dispatch helpers (codegen_loops.cpp) ───────────────────
  struct ForLoopBlocks {
    llvm::BasicBlock *cond_bb;
    llvm::BasicBlock *body_bb;
    llvm::BasicBlock *update_bb;
    llvm::BasicBlock *exit_bb;
  };
  void emit_for_infinite(const ForExprNode &node, const ForLoopBlocks &bbs);
  void emit_for_c_style(const ForExprNode &node,
                        const ForIterClauseNode &iter,
                        const ForLoopBlocks &bbs);
  void emit_for_condition(const ForExprNode &node, const Node &mode,
                          const ForLoopBlocks &bbs);
  void emit_for_range(const ForExprNode &node,
                      const ForRangeClauseNode &range,
                      const ForLoopBlocks &bbs);
  void emit_for_range_array(const ForExprNode &node,
                            const ForRangeClauseNode &range,
                            llvm::Value *iterable, const TypePtr &iter_sem,
                            const ForLoopBlocks &bbs);
  void emit_for_range_map(const ForExprNode &node,
                          const ForRangeClauseNode &range,
                          llvm::Value *iterable, const TypePtr &iter_sem,
                          const ForLoopBlocks &bbs);
  void emit_for_range_task(const ForExprNode &node,
                           const ForRangeClauseNode &range,
                           const StructTypeInfo &task_info,
                           const ForLoopBlocks &bbs);
  void emit_for_range_iterable_struct(const ForExprNode &node,
                                      const ForRangeClauseNode &range,
                                      llvm::Value *iterable,
                                      const TypePtr &iter_sem,
                                      const ForLoopBlocks &bbs);
  llvm::Value *emit_struct_literal(const StructLiteralNode &node);
  llvm::Value *emit_selector(const SelectorNode &node, const Node &parent);
  llvm::Value *emit_switch_expr(const SwitchExprNode &node);
  llvm::Value *emit_array_literal(const ArrayLiteralNode &node);
  llvm::Value *emit_map_literal(const MapLiteralNode &node);
  llvm::Value *emit_index_expr(const IndexExprNode &node);
  llvm::Value *emit_or_expr(const OrExprNode &node);
  llvm::Value *emit_func_expr(const FuncExprNode &node, const Node &parent);
  llvm::Value *emit_spawn_expr(const SpawnExprNode &node, const Node &parent);

  // Selector-callee dispatch in code generation: handles every shape of
  // `obj.method(args)` (module fn, struct method, struct-field call,
  // task message, context method). Implemented in
  // codegen_method_dispatch.cpp; call only when the callee is a selector.
  llvm::Value *emit_method_or_module_call(const CallExprNode &node,
                                          const Node &parent);

  /// Get a GEP to a struct field. Returns {ptr to field, field LLVM type}.
  std::pair<llvm::Value *, llvm::Type *>
  struct_field_gep(llvm::Value *struct_ptr, const TypePtr &struct_sem_type,
                   const std::string &field_name);

  // ── Type query helpers ───────────────────────────────────────────────

  /// Look up the semantic type of an AST node (recorded by the analyzer).
  TypePtr semantic_type(const Node &node) const;

  // ── Per-instantiation accessors (Step 4) ─────────────────────────────
  //
  // Every read of an analyzer side-table inside codegen must go through
  // these accessors.  Each consults current_instantiation_ first and
  // seamlessly falls through to the corresponding analyzer.<table> when
  // the node isn't part of the generic body being specialised.  Misses
  // in *both* maps are real bugs — callers assert or handle as they did
  // before Step 4.

  /// Look up the Symbol recorded for an identifier/selector/etc. node.
  const Symbol *node_symbol(const Node &node) const;

  /// Closure capture list for a FuncExprNode (or its parent wrapper).
  const std::vector<Analyzer::CaptureInfo> *
  node_captures_of(const Node &node) const;

  /// Spawn capture list for a SpawnExprNode's parent.
  const std::vector<Analyzer::SpawnCaptureInfo> *
  spawn_captures_of(const Node &node) const;

  /// Element type for an iterable in a for-range loop (struct protocol).
  TypePtr iterable_next_elem_type_of(const Node &node) const;

  /// Channel element type recorded on a spawn expression.
  TypePtr spawn_channel_elem_type_of(const Node &node) const;

  /// Overloaded binary-op method name (e.g. "Add") for a BinaryExprNode.
  const std::string *struct_operator_method_of(const Node &node) const;

  /// Type recorded for a span (LSP hover fallback).
  TypePtr span_type_at(Span span) const;

  /// Type-argument bindings at a generic call site.
  const std::unordered_map<uint32_t, TypePtr> *
  node_type_args_of(const Node &node) const;

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
