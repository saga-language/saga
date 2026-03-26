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

  // ── Next unique id for type parameters ───────────────────────────────
  uint32_t next_type_param_id = 0;

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

private:
  // ── Node visitors (to be implemented in phases) ──────────────────────

  // Phase 1: Declarations — collect top-level names.
  void visit_package(const PackageNode &node);
  void visit_source(const SourceNode &node);
  void collect_declaration(const Node &node);

  // Phase 2: Type resolution — resolve type annotations to TypePtrs.
  TypePtr resolve_type(const Node &node);
  TypePtr resolve_identifier_type(const IdentifierNode &node);
  TypePtr resolve_array_type(const ArrayTypeNode &node);
  TypePtr resolve_map_type(const MapTypeNode &node);
  TypePtr resolve_func_type(const FuncTypeNode &node);
  TypePtr resolve_range_type(const RangeTypeNode &node);
  TypePtr resolve_struct_type(const StructTypeNode &node);
  TypePtr resolve_union_type(const UnionTypeNode &node);

  // Phase 3: Expression type-checking — infer/check expression types.
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
  TypePtr check_func_expr(const FuncExprNode &node);
  TypePtr check_group_expr(const GroupExprNode &node);
  TypePtr check_import_expr(const ImportExprNode &node);

  // Phase 4: Statement checking.
  void check_stmt(const Node &node);
  void check_var_decl(const VarDeclNode &node, const Node &parent);
  void check_decl_assign(const DeclAssignNode &node);
  void check_assign(const AssignNode &node);
  void check_increment(const IncrementNode &node);
  void check_decrement(const DecrementNode &node);
  void check_return(const ReturnNode &node);
  void check_break(const BreakNode &node);
  void check_next(const NextNode &node);

  // Phase 5: Top-level declaration checking.
  void check_const_decl(const ConstDeclNode &node);
  void check_enum_decl(const EnumDeclNode &node);
  void check_func_decl(const FuncDeclNode &node);
  void check_struct_decl(const StructDeclNode &node);
  void check_interface_decl(const InterfaceDeclNode &node);
  void check_import_decl(const ImportDeclNode &node);

  // Block checking.
  TypePtr check_block(const BlockNode &node);

  // ── Generic instantiation ────────────────────────────────────────────

  /// Instantiate a generic callable: infer type args from call-site
  /// arguments and return the substituted signature.
  TypePtr instantiate_generic_call(const TypePtr &callee_type,
                                   const std::vector<TypeParam> &type_params,
                                   const std::vector<TypePtr> &arg_types,
                                   Span call_span);

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
