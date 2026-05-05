// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#pragma once

#include "frontend/ast.hpp"
#include "frontend/error_list.hpp"
#include "frontend/fileset.hpp"
#include "semantic/builtins.hpp"
#include "semantic/scope.hpp"
#include "semantic/types.hpp"

#include <list>
#include <unordered_map>
#include <unordered_set>

namespace saga {

// ---------------------------------------------------------------------------
// PackageResolver — resolves import paths to filesystem directories,
// caches analyzed packages, and detects import cycles.
// ---------------------------------------------------------------------------

struct PackageResolver {
  /// Directories to search for packages (e.g. std library, project root).
  std::vector<std::string> search_paths;

  /// Directories to search for pre-compiled .sgi interface files.
  /// Checked before source compilation for faster import resolution.
  std::vector<std::string> sgi_search_paths;

  /// Cache of already-resolved packages: import_path → module type.
  std::unordered_map<std::string, TypePtr> cache;

  /// Import paths currently being resolved (for cycle detection).
  std::unordered_set<std::string> in_progress;

  /// For testing: pre-registered mock packages that bypass filesystem.
  std::unordered_map<std::string, TypePtr> mock_packages;

  /// Maps import_path → directory where .sgi (and presumably .o) was found.
  /// Populated when a package is resolved via .sgi.
  std::unordered_map<std::string, std::string> sgi_resolved_dirs;

  /// Maps short package name → absolute directory containing the package's
  /// .sg sources. Populated by both the SGI loader (from the SGI's
  /// `source_dir` line) and the source compiler (from the resolved
  /// package directory). Used by codegen to lazily load generic method
  /// bodies that the origin package could not pre-instantiate.
  std::unordered_map<std::string, std::string> source_dirs;

  /// Find the filesystem directory for the given import path.
  /// Returns empty string if not found.
  std::string find_package_dir(const std::string &import_path) const;

  /// Find a .sgi file for the given import path in sgi_search_paths.
  /// Returns empty string if not found.
  std::string find_sgi_file(const std::string &import_path) const;

  /// List all .sg source files in the given directory.
  std::vector<std::string> list_source_files(const std::string &dir) const;
};

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

  /// Auxiliary type map keyed by byte-offset span, for identifiers that are
  /// not wrapped in a Node (e.g. struct-literal field names, accumulator
  /// pipes).  The LSP hover handler consults this when node_types has no
  /// tighter match.
  std::vector<std::pair<Span, TypePtr>> span_types;

  // ── Module/Import system ─────────────────────────────────────────────

  /// Package resolver (shared across sub-package analyzers).
  std::shared_ptr<PackageResolver> package_resolver;

  /// The directory of the package currently being analyzed (for relative imports).
  std::string current_package_dir;

  /// Authoritative name of the package currently being analyzed.  Set by
  /// the build driver from the manifest or import path.  When empty,
  /// current_package_name() falls back to the basename of
  /// current_package_dir; new call sites should prefer setting this
  /// explicitly so package identity is not bound to filesystem layout.
  std::string current_package_name_override;

  /// True when compiling a stdlib package (permits intrinsic_* calls and
  /// receiver methods on intrinsic types).
  bool is_stdlib = false;

  /// The package-level scope (saved after analysis for import extraction).
  Scope::Ptr package_scope_;

  /// Import paths seen in the current package (duplicate detection).
  std::unordered_set<std::string> imported_paths_;

  // ── Closure capture tracking ─────────────────────────────────────────

  /// Information about a single captured variable in a closure.
  struct CaptureInfo {
    std::string name;       // variable name
    TypePtr type;           // resolved type of the captured variable
  };

  /// Maps each FuncExprNode (by Node*) to its list of captured variables.
  std::unordered_map<const Node *, std::vector<CaptureInfo>> node_captures;

  // ── Spawn capture tracking ───────────────────────────────────────────

  /// How a captured variable should be transferred to a spawn block.
  enum class SpawnCaptureKind : uint8_t {
    Copy,   // value is trivially copyable (scalars, small structs)
    Share,  // refcounted type, read-only in both contexts → retain
    Move,   // variable not used after spawn → transfer ownership
  };

  /// Information about a single variable captured by a spawn expression.
  struct SpawnCaptureInfo {
    std::string name;
    TypePtr type;
    SpawnCaptureKind kind = SpawnCaptureKind::Copy;
  };

  /// Maps each SpawnExprNode (by Node*) to its list of captured variables.
  std::unordered_map<const Node *, std::vector<SpawnCaptureInfo>>
      spawn_captures;

  /// Resolved channel element type for each channel-carrying spawn
  /// (`|T| spawn ...`).  Populated during semantic analysis so codegen does
  /// not re-run type resolution against a scope that has already been popped.
  std::unordered_map<const Node *, TypePtr> spawn_channel_elem_types;

  // ── Operator overloading ─────────────────────────────────────────────

  // ── User-bound methods for non-struct types ────────────────────────

  /// Methods bound to types via receiver syntax that don't have their own
  /// methods vector (e.g. enums). Keyed by raw Type pointer.
  std::unordered_map<const Type *, std::vector<MethodInfo>> type_methods_;

  /// Stdlib-defined receiver methods on generic types (Array, Map).
  /// Keyed by TypeKind; signatures use sentinel type-param IDs (9990=T,
  /// 9991=K, 9992=V) matching the builtin_methods convention so that the
  /// same substitution logic in check_selector handles both.
  std::unordered_map<TypeKind, std::vector<MethodInfo>> kind_methods_;

  /// Per-kind/method side table that retains the original FuncDecl,
  /// pre-normalization signature, and original-ID TypeParams for bodies
  /// that codegen needs to specialise per concrete K (because they
  /// dispatch through a named protocol on the TypeParam — see
  /// kind_method_uses_typeparam_dispatch_).  Populated during the
  /// stdlib's signature pass and consumed by codegen.
  struct KindMethodDecl {
    const FuncDeclNode *decl = nullptr;
    TypePtr original_signature;
    std::vector<TypeParam> type_params;  // original IDs
  };
  std::unordered_map<TypeKind, std::unordered_map<std::string, KindMethodDecl>>
      kind_method_decls_;

  /// Maps a BinaryExprNode (by its containing Node*) to the method name that
  /// should be called to implement the operator (e.g. "Add", "Compare").
  /// Only populated for struct-typed operands; primitive operators are still
  /// lowered directly to IR.
  std::unordered_map<const Node *, std::string> struct_operator_methods;

  // ── Iterable struct tracking ───────────────────────────────────────────

  /// Maps the iterable Node* inside a ForRangeClauseNode to the inferred
  /// element type T when the iterable is a struct implementing
  /// `Next() T | Error`.  Codegen reads this to emit the protocol-based loop.
  std::unordered_map<const Node *, TypePtr> iterable_next_elem_type;

  // ── Generic free-function templates (lazy body-analysis) ─────────────

  /// A generic free function's body is not analysed at declaration time;
  /// it is re-analysed once per distinct binding tuple at instantiation
  /// time. The signature pass stores the AST and the lexical scope the
  /// body should resolve names against.
  struct GenericTemplate {
    const FuncDeclNode *decl;   // body reachable via decl->body
    Scope::Ptr decl_scope;      // scope visible to the body when declared
    std::vector<TypeParam> type_params; // ordered (id, name) pairs
  };

  /// Generic free-function templates collected during the signature pass.
  /// Keyed by FuncDeclNode*.  Receiver methods on intrinsic types are
  /// handled separately via type_methods_/kind_methods_ and are not
  /// stored here.
  std::unordered_map<const FuncDeclNode *, GenericTemplate> generic_templates_;

  /// Reverse map: signature TypePtr → FuncDeclNode*.  Populated during the
  /// signature pass so that check_call_expr can find the declaring AST for
  /// a callee it just type-checked.  Covers both generic and non-generic
  /// free functions (non-generic entries are harmless and keep the map
  /// simple for future use).
  std::unordered_map<const Type *, const FuncDeclNode *> func_decl_by_type_;

  /// One per-instantiation side-table view.  Lives inside instantiations_.
  /// Codegen will read these in Step 4 through accessors that fall through
  /// to the global tables for nodes that aren't in the generic body.
  struct BodyInstantiation {
    const FuncDeclNode *decl = nullptr;
    std::unordered_map<uint32_t, TypePtr> bindings;
    bool in_progress = false;

    std::unordered_map<const Node *, TypePtr> node_types;
    std::unordered_map<const Node *, Symbol> node_symbols;
    std::unordered_map<const Node *, std::vector<CaptureInfo>> node_captures;
    std::unordered_map<const Node *, std::vector<SpawnCaptureInfo>>
        spawn_captures;
    std::unordered_map<const Node *, TypePtr> iterable_next_elem_type;
    std::unordered_map<const Node *, TypePtr> spawn_channel_elem_types;
    std::unordered_map<const Node *, std::string> struct_operator_methods;
    std::vector<std::pair<Span, TypePtr>> span_types;
    std::unordered_map<const Node *, std::unordered_map<uint32_t, TypePtr>>
        node_type_args;
  };

  /// Cached body instantiations.  List-per-decl keeps references stable
  /// across insertions so current_instantiation_ remains valid while the
  /// map is mutated by nested instantiations.
  std::unordered_map<const FuncDeclNode *, std::list<BodyInstantiation>>
      instantiations_;

  /// Origin packages whose source was lazily loaded so codegen can emit
  /// generic method bodies that the origin's compiled .o does not contain
  /// (D8). Each entry retains the FileSet, AST, and a sub-analyzer whose
  /// node-keyed tables have been merged into ours; this keeps every node
  /// pointer alive for as long as the parent analyzer survives.
  struct LoadedSourcePackage {
    std::unique_ptr<FileSet> fileset;
    NodePtr ast;
    std::unique_ptr<Analyzer> sub_analyzer;
  };
  std::unordered_map<std::string, LoadedSourcePackage> loaded_source_packages_;

  /// The BodyInstantiation whose side-tables should receive writes (and
  /// satisfy reads) during the current Phase 3/4 pass.  nullptr when
  /// analysing non-generic code.
  BodyInstantiation *current_instantiation_ = nullptr;

  /// Stdlib generic receiver methods on Array/Map (kind_methods_) whose
  /// bodies dispatch through a named protocol on a TypeParam value
  /// (e.g. `for v : a { v.String() }`).  These methods cannot be emitted
  /// once with T opaque — codegen specialises per concrete K at every
  /// user call site.  Populated by check_func_decl_body during the eager
  /// body pass and consumed by codegen.
  std::unordered_set<const FuncDeclNode *>
      kind_method_uses_typeparam_dispatch_;

  /// While checking a kind_methods_ body eagerly, points at the FuncDecl
  /// being checked.  resolve_method_signature consults this when its
  /// named-protocol fallback fires on a TypeParam receiver, so the decl
  /// can be added to kind_method_uses_typeparam_dispatch_.  nullptr
  /// outside the eager kind_methods_ pass.
  const FuncDeclNode *current_eager_kind_method_decl_ = nullptr;

  /// Call-site backtrace for instantiation errors.  check_call_expr pushes
  /// the CallExprNode* before driving a body instantiation and pops after.
  /// Analyzer::error() reads this to append "...instantiated from" frames.
  std::vector<const Node *> instantiation_stack_;

  // ── Next unique id for type parameters ───────────────────────────────
  uint32_t next_type_param_id = 0;

  // ── Closure resolution state ─────────────────────────────────────────
  /// Stack of closure nodes currently being resolved (for nested closures).
  std::vector<const Node *> closure_node_stack_;
  /// Pointer to the current closure node being resolved (top of stack).
  const Node *pending_closure_node_ = nullptr;

  // ── Spawn resolution state ──────────────────────────────────────────
  /// Stack of spawn nodes currently being resolved (for nested spawns).
  std::vector<const Node *> spawn_node_stack_;
  /// Pointer to the current spawn node being resolved (top of stack).
  const Node *pending_spawn_node_ = nullptr;

  // ── Construction ─────────────────────────────────────────────────────

  explicit Analyzer(FileSet &fs);
  explicit Analyzer(FileSet &fs, std::shared_ptr<PackageResolver> resolver);

  /// Return the short package name derived from current_package_dir.
  /// Returns "" when no package directory is set (top-level compilation).
  std::string current_package_name() const;

  // ── Entry point ──────────────────────────────────────────────────────

  /// Analyze an entire package (PackageNode at root).
  void analyze(const Node &root);

  /// Load stdlib type packages (int, float, bool, string) from pre-compiled
  /// .sgi files and populate type_methods_.  Called automatically at the start
  /// of analysis when not in stdlib mode.
  void load_prelude();

  // ── Import resolution ────────────────────────────────────────────────

  /// Resolve an import path to a Module type.  Parses and analyzes the
  /// sub-package, caches the result, and returns the module type.
  TypePtr resolve_import(const std::string &import_path, Span span);

  /// Process all import declarations in a source after names are collected.
  void process_imports(const std::vector<NodePtr> &declarations);

  // ── Cross-package generic method body loading (D8) ────────────────────

  /// Result of `load_imported_method_decl`: enough information for codegen
  /// to emit a per-importer specialisation of a generic method whose body
  /// lives in another package. The instantiation pointer drives body
  /// codegen via current_instantiation_; the bindings are keyed by the
  /// origin package's TypeParam IDs (struct-aligned) so emit_specialisation
  /// can substitute and mangle correctly.
  struct ImportedMethodDecl {
    const FuncDeclNode *decl = nullptr;
    TypePtr template_signature;
    std::vector<TypeParam> struct_type_params;
    std::unordered_map<uint32_t, TypePtr> bindings;
    BodyInstantiation *instantiation = nullptr;
  };

  /// Lazily load and analyse the origin package's source so codegen can
  /// emit a generic method body that the origin's compiled .o does not
  /// contain. The caller supplies the concrete type arguments (positionally
  /// aligned with the struct's type parameters) so this method can drive
  /// per-binding body type-checking in the sub-analyzer. Returns
  /// `decl == nullptr` if the source cannot be located or the requested
  /// method cannot be found.
  ImportedMethodDecl
  load_imported_method_decl(const std::string &origin,
                             const std::string &struct_name,
                             const std::string &method_name,
                             const std::vector<TypePtr> &type_args);

private:
  /// Return a cached/mock TypePtr for `import_path` if present, or nullopt
  /// to signal the caller to continue with sgi/source resolution. Returns
  /// builtins.error_type wrapped in optional on cycle detection.
  std::optional<TypePtr> resolve_import_cached(const std::string &import_path,
                                               Span span);

  /// Try loading the import from a pre-compiled .sgi file. Returns the
  /// constructed module TypePtr on success, nullopt if no .sgi was found.
  /// On parse failure, emits a diagnostic at `span` and returns nullopt
  /// without falling through to source compilation.
  std::optional<TypePtr> load_import_from_sgi(const std::string &import_path,
                                               Span span);

  /// Parse and analyze the import's source files. Returns the constructed
  /// module TypePtr on success, builtins.error_type on failure.
  TypePtr compile_import_from_source(const std::string &import_path,
                                     Span span);

  /// Build the public-export list from a fully-analyzed sub-package scope.
  std::vector<ModuleExport>
  extract_module_exports(const Analyzer &sub_analyzer);

  /// Merge stdlib receiver methods from a sub-analyzer or sgi into our
  /// own type_methods_ / kind_methods_ tables.
  void merge_sgi_receiver_methods(const struct SgiFile &sgi);
  void merge_sub_analyzer_receiver_methods(const Analyzer &sub);

  /// Ensure the origin package's source has been parsed and analysed,
  /// retaining the sub-analyzer in `loaded_source_packages_` and merging
  /// its node-keyed tables (node_types/symbols/captures, generic
  /// templates, …) into ours so codegen can emit specialisations of
  /// methods that template through the imported struct's type
  /// parameters. Returns nullptr if the source cannot be located.
  Analyzer *ensure_source_loaded(const std::string &origin);

public:

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
  TypePtr resolve_selector_type(const SelectorNode &node);
  TypePtr resolve_array_type(const ArrayTypeNode &node);
  TypePtr resolve_map_type(const MapTypeNode &node);
  TypePtr resolve_func_type(const FuncTypeNode &node);
  TypePtr resolve_range_type(const RangeTypeNode &node);
  TypePtr resolve_struct_type(const StructTypeNode &node);
  TypePtr resolve_union_type(const UnionTypeNode &node);
  TypePtr resolve_generic_type_app(const GenericTypeAppNode &node);

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
  void resolve_spawn_expr(const SpawnExprNode &node, const Node &parent);
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
  TypePtr check_binary_expr(const BinaryExprNode &node, const Node &parent);
  TypePtr check_unary_expr(const UnaryExprNode &node);
  TypePtr check_call_expr(const CallExprNode &node, const Node &parent);
  TypePtr check_index_expr(const IndexExprNode &node);
  TypePtr check_selector(const SelectorNode &node, const Node &parent);

  // ── check_selector helpers ──────────────────────────────────────────
  TypePtr resolve_module_selector(const ModuleTypeInfo &mod,
                                  const std::string &field_name,
                                  Span field_span);
  TypePtr resolve_struct_member(const TypePtr &owner_type,
                                const std::string &field_name,
                                Span field_span);
  TypePtr resolve_method_signature(const TypePtr &obj_type,
                                   const std::string &field_name);
  TypePtr check_if_expr(const IfExprNode &node);
  TypePtr check_switch_expr(const SwitchExprNode &node);
  TypePtr check_for_expr(const ForExprNode &node,
                         TypePtr accumulator_hint = nullptr);
  TypePtr check_range_expr(const RangeExprNode &node);
  TypePtr check_spawn_expr(const SpawnExprNode &node, const Node &parent);
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

  // Struct operator overloading helper.
  TypePtr check_struct_binary_expr(const BinaryExprNode &node,
                                    const Node &parent, const TypePtr &lhs,
                                    const TypePtr &rhs);
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
  /// If out_bindings is non-null, the inferred bindings are written there
  /// so the caller can drive body instantiation (see instantiate_generic_body).
  TypePtr instantiate_generic_call(
      const TypePtr &callee_type, const std::vector<TypePtr> &arg_types,
      Span call_span,
      std::unordered_map<uint32_t, TypePtr> *out_bindings = nullptr);

  /// Drive Phase 3 and Phase 4 over a generic free function's body with
  /// the given bindings.  Reuses a cached BodyInstantiation on match (or
  /// when a re-entrant call re-arrives during in-progress analysis).
  /// `call_node` is pushed onto instantiation_stack_ for error traces.
  BodyInstantiation *instantiate_generic_body(
      const FuncDeclNode &fn,
      const std::unordered_map<uint32_t, TypePtr> &bindings,
      const Node &call_node);

  /// Instantiate a generic struct type: infer type-parameter bindings from
  /// field initializer values and return the fully substituted struct type.
  TypePtr instantiate_generic_struct(const TypePtr &struct_type,
                                     const std::vector<std::pair<std::string, TypePtr>> &field_types,
                                     Span span);

  // ── Interface conformance ────────────────────────────────────────────

  /// Check whether `concrete` satisfies every method in `iface`.
  bool satisfies_interface(const TypePtr &concrete, const TypePtr &iface);

  /// Named protocols the compiler dispatches through.  Used by
  /// `check_satisfies_protocol` to pick the relevant interface from
  /// `builtins.*_iface` and to render the protocol name in diagnostics.
  enum class ProtocolKind { Hashable, Stringable };

  /// Verify `concrete` satisfies the named protocol `p`; emit a named
  /// diagnostic at `at` if not.  Skips silently when `concrete` is a
  /// TypeParam (deferred to the monomorphisation site), an ErrorType,
  /// or when the protocol interface hasn't been loaded yet (e.g. during
  /// std/proto's own bootstrap or in tests without a package resolver).
  /// `context` describes where the requirement comes from
  /// ("map key", "interpolated expression", …) and is folded into the
  /// diagnostic.  Returns `false` and reports an error on failure.
  bool check_satisfies_protocol(const TypePtr &concrete, ProtocolKind p,
                                Span at, const std::string &context = "");

  /// Recursively verify `t` is Stringable, descending into Array elements
  /// and Map keys/values so that `[[Foo]]` requires `Foo` itself to be
  /// Stringable rather than terminating at the trivially-Stringable Array.
  /// Returns `false` and reports the named diagnostic on failure.
  bool check_stringable_recursive(const TypePtr &t, Span at,
                                  const std::string &context = "");

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

} // namespace saga
