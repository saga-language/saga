// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#pragma once

#include "frontend/ast.hpp"
#include "frontend/error_list.hpp"
#include "frontend/fileset.hpp"
#include "semantic/builtins.hpp"
#include "semantic/scope.hpp"
#include "semantic/types.hpp"

#include <unordered_map>

namespace mc {

// ---------------------------------------------------------------------------
// Analyzer — semantic analysis over a parsed AST.
//
// Performs name resolution, type checking, and generic instantiation.
// Errors are accumulated into an ErrorList; analysis continues as far as
// possible (error-recovery via ErrorType propagation).
//
// Usage:
//   Analyzer analyzer(fileset);
//   analyzer.analyze(package_node);
//   analyzer.errors.print_errors();
// ---------------------------------------------------------------------------

struct Analyzer {
  FileSet &fileset;
  ErrorList errors;

  // ── Type system singletons ───────────────────────────────────────────

  BuiltinTypes builtins;

  // ── Scope stack ──────────────────────────────────────────────────────

  Scope::Ptr global_scope;
  Scope::Ptr current_scope;

  // ── Resolved information (output tables) ─────────────────────────────

  /// Maps each AST node (by Node*) to its resolved type.
  std::unordered_map<const Node *, TypePtr> node_types;

  /// Maps each identifier AST node to the Symbol it resolves to.
  std::unordered_map<const Node *, Symbol> node_symbols;

  /// Maps each generic instantiation site to its type-argument bindings.
  std::unordered_map<const Node *, std::unordered_map<uint32_t, TypePtr>>
      node_type_args;

  // ── Closure capture tracking ─────────────────────────────────────────

  /// Information about a single captured variable in a closure.
  struct CaptureInfo {
    std::string name;       // variable name
    TypePtr type;           // resolved type of the captured variable
  };

  /// Maps each FuncExprNode (by Node*) to its list of captured variables.
  std::unordered_map<const Node *, std::vector<CaptureInfo>> node_captures;

  // ── Next unique id for type parameters ───────────────────────────────
  uint32_t next_type_param_id = 0;

  // ── Closure resolution state ─────────────────────────────────────────
  /// Stack of closure nodes currently being resolved (for nested closures).
  std::vector<const Node *> closure_node_stack_;
  /// Pointer to the current closure node being resolved (top of stack).
  const Node *pending_closure_node_ = nullptr;

  // ── Construction ─────────────────────────────────────────────────────

  explicit Analyzer(FileSet &fs);

  // ── Entry point ──────────────────────────────────────────────────────

  /// Analyze an entire package (PackageNode at root).
  void analyze(const Node &root);

  // ── Scope helpers ────────────────────────────────────────────────────

  /// Push a new child scope of the given kind.
  void push_scope(ScopeKind kind);

  /// Pop the current scope, returning to its parent.
  void pop_scope();

  /// Declare a symbol in the current scope; reports an error on duplicate.
  bool declare(const Symbol &sym);

  /// Look up a name from the current scope.
  std::optional<Symbol> lookup(const std::string &name) const;

  // ── Type-parameter helpers ───────────────────────────────────────────

  /// Allocate a fresh type-parameter id.
  uint32_t fresh_type_param_id();

  /// Enter generic parameters into the current scope and return a mapping
  /// from their ids to the TypeParam types.  Call after push_scope.
  std::vector<TypeParam> enter_generics(const GenericNode &generic);

  /// Collect current type bindings for substitution.
  std::unordered_map<uint32_t, TypePtr> current_type_bindings() const;

  // ── Recording results ────────────────────────────────────────────────

  /// Associate a resolved type with an AST node.
  void record_type(const Node &node, TypePtr type);

  /// Associate a resolved symbol with an AST node.
  void record_symbol(const Node &node, const Symbol &sym);

  // ── Error reporting ──────────────────────────────────────────────────

  /// Report a semantic error at the given span.
  void error(Span span, const std::string &message);

  /// Report a type-mismatch error with expected/actual formatting.
  void type_error(Span span, const TypePtr &expected, const TypePtr &actual,
                  const std::string &context = "");

  /// Report an undefined-name error.
  void undefined_error(Span span, const std::string &name);

  /// Report a duplicate-declaration error.
  void redeclaration_error(Span span, const std::string &name);

  /// Report a shadowing error (inner scope reuses outer scope name).
  void shadowing_error(Span span, const std::string &name);

  /// Check whether any error message contains the given substring.
  bool has_error_containing(const std::string &substr) const;

  // ── Local declaration helper ─────────────────────────────────────────

  /// Declare a local symbol, checking for both same-scope redeclaration
  /// and outer-scope shadowing.  Returns false on error.
  bool declare_local(const Symbol &sym);

  // Phase 2: Type resolution — resolve type annotations to TypePtrs.
  // Public so codegen can query return types from signature nodes.
  TypePtr resolve_type(const Node &node);

private:
  // ── Node visitors (to be implemented in phases) ──────────────────────

  // Phase 1: Declarations — collect top-level names.
  void visit_package(const PackageNode &node);
  void visit_source(const SourceNode &node);
  void collect_declaration(const Node &node);
  TypePtr resolve_identifier_type(const IdentifierNode &node);
  TypePtr resolve_array_type(const ArrayTypeNode &node);
  TypePtr resolve_map_type(const MapTypeNode &node);
  TypePtr resolve_func_type(const FuncTypeNode &node);
  TypePtr resolve_range_type(const RangeTypeNode &node);
  TypePtr resolve_struct_type(const StructTypeNode &node);
  TypePtr resolve_union_type(const UnionTypeNode &node);

  // Phase 2b: Resolve top-level declaration bodies — fills in TypePtrs
  // that were left nullptr during collection.
  void resolve_declaration(const Node &node);
  void resolve_func_decl(const FuncDeclNode &node);
  void resolve_struct_decl(const StructDeclNode &node);
  void resolve_enum_decl(const EnumDeclNode &node);
  void resolve_interface_decl(const InterfaceDeclNode &node);
  void resolve_const_decl(const ConstDeclNode &node);

  // Signature / parameter helpers.
  TypePtr resolve_signature(const SignatureNode &sig);
  void declare_parameters(const SignatureNode &sig);

  // Phase 3: Resolve names inside function/method bodies.
  // If `enclosing_struct` is non-null, the struct's fields are injected
  // into the function scope (for in-bound methods).
  void resolve_func_decl_body(const FuncDeclNode &fn,
                               const TypePtr &enclosing_struct = nullptr);

  // Phase 4: Type-check function/method bodies.
  void check_func_decl_body(const FuncDeclNode &fn,
                             const TypePtr &enclosing_struct = nullptr);

  /// Inject a struct's fields into the current scope as local variables.
  void inject_struct_fields(const TypePtr &struct_type);

  /// Returns true if a node always terminates via `return` on every
  /// control-flow path (e.g. if/else where both branches return, or
  /// a switch where every arm returns).
  bool always_returns(const Node &node) const;

  // Phase 3: Name resolution in expressions — resolve identifiers,
  // record symbols, and walk all sub-expressions.
  void resolve_expr(const Node &node);
  void resolve_identifier(const IdentifierNode &node, const Node &parent);
  void resolve_block(const BlockNode &node);
  void resolve_call_expr(const CallExprNode &node);
  void resolve_index_expr(const IndexExprNode &node);
  void resolve_selector(const SelectorNode &node);
  void resolve_binary_expr(const BinaryExprNode &node);
  void resolve_unary_expr(const UnaryExprNode &node);
  void resolve_if_expr(const IfExprNode &node);
  void resolve_switch_expr(const SwitchExprNode &node);
  void resolve_for_expr(const ForExprNode &node);
  void resolve_range_expr(const RangeExprNode &node);
  void resolve_spawn_expr(const SpawnExprNode &node);
  void resolve_or_expr(const OrExprNode &node);
  void resolve_func_expr(const FuncExprNode &node, const Node &parent);
  void resolve_group_expr(const GroupExprNode &node);
  void resolve_string_literal(const StringLiteralNode &node);
  void resolve_array_literal(const ArrayLiteralNode &node);
  void resolve_map_literal(const MapLiteralNode &node);
  void resolve_struct_literal(const StructLiteralNode &node);

  // Phase 4: Name resolution in statements.
  void resolve_stmt(const Node &node);
  void resolve_var_decl(const VarDeclNode &node, const Node &parent);
  void resolve_decl_assign(const DeclAssignNode &node, const Node &parent);
  void resolve_assign(const AssignNode &node);
  void resolve_return(const ReturnNode &node);
  void resolve_break(const BreakNode &node);
  void resolve_increment(const IncrementNode &node);
  void resolve_decrement(const DecrementNode &node);

  // Phase 3–4 combined: resolve a block statement or expression.
  void resolve_block_stmt(const Node &node);

  // ── Type-checking stubs (later phases) ───────────────────────────────

  // Phase 5: Expression type-checking — infer/check expression types.
  TypePtr check_expr(const Node &node);
  TypePtr check_identifier(const IdentifierNode &node, const Node &parent);
  TypePtr check_bool_literal(const BoolLiteralNode &node);
  TypePtr check_int_literal(const IntegerLiteralNode &node);
  TypePtr check_float_literal(const FloatLiteralNode &node);
  TypePtr check_string_literal(const StringLiteralNode &node);
  TypePtr check_array_literal(const ArrayLiteralNode &node);
  TypePtr check_map_literal(const MapLiteralNode &node);
  TypePtr check_struct_literal(const StructLiteralNode &node);
  TypePtr check_binary_expr(const BinaryExprNode &node);
  TypePtr check_unary_expr(const UnaryExprNode &node);
  TypePtr check_call_expr(const CallExprNode &node);
  TypePtr check_index_expr(const IndexExprNode &node);
  TypePtr check_selector(const SelectorNode &node, const Node &parent);
  TypePtr check_if_expr(const IfExprNode &node);
  TypePtr check_switch_expr(const SwitchExprNode &node);
  TypePtr check_for_expr(const ForExprNode &node);
  TypePtr check_range_expr(const RangeExprNode &node);
  TypePtr check_spawn_expr(const SpawnExprNode &node);
  TypePtr check_or_expr(const OrExprNode &node);
  TypePtr check_func_expr(const FuncExprNode &node, const Node &parent);
  TypePtr check_group_expr(const GroupExprNode &node);
  TypePtr check_import_expr(const ImportExprNode &node);

  // Phase 6: Statement checking.
  void check_stmt(const Node &node);
  void check_var_decl(const VarDeclNode &node, const Node &parent);
  void check_decl_assign(const DeclAssignNode &node);
  void check_assign(const AssignNode &node);
  void check_increment(const IncrementNode &node);
  void check_decrement(const DecrementNode &node);
  void check_return(const ReturnNode &node);
  void check_break(const BreakNode &node);
  void check_next(const NextNode &node);

  // Phase 7: Top-level declaration checking.
  void check_const_decl(const ConstDeclNode &node);
  void check_enum_decl(const EnumDeclNode &node);
  void check_func_decl(const FuncDeclNode &node);
  void check_struct_decl(const StructDeclNode &node);
  void check_interface_decl(const InterfaceDeclNode &node);
  void check_import_decl(const ImportDeclNode &node);

  // Block checking.
  TypePtr check_block(const BlockNode &node);

  // ── Generic instantiation ────────────────────────────────────────────

  /// Instantiate a generic callable: infer type-parameter bindings from
  /// call-site argument types and return the fully substituted signature.
  TypePtr instantiate_generic_call(const TypePtr &callee_type,
                                   const std::vector<TypePtr> &arg_types,
                                   Span call_span);

  /// Instantiate a generic struct type: infer type-parameter bindings from
  /// field initializer values and return the fully substituted struct type.
  TypePtr instantiate_generic_struct(const TypePtr &struct_type,
                                     const std::vector<std::pair<std::string, TypePtr>> &field_types,
                                     Span span);

  // ── Interface conformance ────────────────────────────────────────────

  /// Check whether `concrete` satisfies every method in `iface`.
  bool satisfies_interface(const TypePtr &concrete, const TypePtr &iface);

  // ── Assignment compatibility helper ──────────────────────────────────

  /// Verify that `value_type` is assignable to `target_type`; report an
  /// error at `span` if not.
  void expect_assignable(Span span, const TypePtr &target_type,
                         const TypePtr &value_type,
                         const std::string &context = "");

  /// Verify that `type` is the expected kind; report an error if not.
  void expect_type(Span span, const TypePtr &type, TypeKind expected,
                   const std::string &context = "");

  /// Verify that the expression is a boolean; report an error if not.
  void expect_bool(Span span, const TypePtr &type,
                   const std::string &context = "condition");
};

} // namespace mc
