// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#pragma once

#include "ast.hpp"
#include "error_list.hpp"
#include "fileset.hpp"
#include "lexer.hpp"

namespace saga {
struct Parser {
  Lexer lexer;
  FileSet &fileset;
  Token current;
  Token previous;
  ErrorList errors;

  Parser(FileSet &fs) : fileset(fs) {}

  // ── Entry Points ─────────────────────────────────────────────────────
  NodePtr parse();

  /// Initialise the lexer from a single file and prime `current`.  Call
  /// this before parse_expression() when driving the parser outside of the
  /// full parse() entry point (e.g. in expression-level unit tests).
  void init_for_file(File *f);

  /// Top-level expression entry point.  Parses a single expression starting
  /// at `current`.  Exposed publicly so tests can parse expressions in
  /// isolation without going through parse_source() / parse_declaration().
  NodePtr parse_expression();

  /// Parse a block: "{" { (Statement | Expression) [terminal] } "}"
  /// Exposed publicly so tests can drive block parsing directly.
  NodePtr parse_block();

  // ── Precedence / Pratt helpers ───────────────────────────────────────
  //
  // Pure lookup functions — they depend only on their argument, not on any
  // parser state. Declared public static so they can be tested in isolation
  // and called without a Parser instance when useful.

  /// Return the left binding power for an infix or postfix operator token,
  /// or 0 if the token is not an infix operator.
  static int infix_binding_power(Token::Kind kind);

  /// Return the right binding power for a prefix operator token, or 0.
  static int prefix_binding_power(Token::Kind kind);

private:
  // ── Token Helpers ────────────────────────────────────────────────────
  //
  // These are the most important methods in the parser. Every parsing
  // function is built on top of these primitives.

  /// Advance to the next non-comment token, storing previous.
  Token advance();

  /// Return true (and advance) if current matches `kind`.
  bool match(Token::Kind kind);

  /// Return true if current token is `kind` (no advance).
  bool check(Token::Kind kind) const;

  /// If current matches `kind`, advance and return the consumed token.
  /// Otherwise return std::nullopt.
  std::optional<Token> accept(Token::Kind kind);

  /// Like accept but fatal: if current is not `kind`, report an error
  /// and return a synthetic token. Always advances.
  Token expect(Token::Kind kind);

  /// Look at the next token without consuming it.
  Token peek() const;

  /// Skip Terminator tokens (newlines). Returns how many were skipped.
  int skip_terminators();

  /// Skip terminators that appear before an expected closing token.
  /// Useful inside blocks and parameter lists.
  void skip_terminators_before(Token::Kind closing);

  /// True if current is Eof or we've hit max errors.
  bool is_at_end() const;

  /// Report an error at the current token's position.
  void error(const std::string &message);

  /// Report an error at an arbitrary span.
  void error_at(Span span, const std::string &message);

  /// After an error, skip tokens until we reach a synchronisation point
  /// (a newline, `}`, or a declaration keyword). This prevents cascading
  /// errors.
  void synchronize();

  // ── Span Helpers ─────────────────────────────────────────────────────

  /// Capture span start (call before parsing a production).
  size_t mark() const;

  /// Build a Span from a previously captured mark to the end of `previous`.
  Span span_from(size_t start) const;

  // ── Type Parsing ─────────────────────────────────────────────────────

  /// Type = UnionType
  NodePtr parse_type();

  /// UnionType = SingleType { "|" SingleType }
  NodePtr parse_union_type();

  /// SingleType = Identifier | ArrayType | MapType | FuncType | ...
  NodePtr parse_single_type();

  /// ArrayType = "[" Type "]"
  NodePtr parse_array_type();

  /// MapType = "{" Type ":" Type "}"
  NodePtr parse_map_type();

  /// FuncType = "fn" Signature
  NodePtr parse_func_type();

  /// RangeType = "(" Type ")"
  NodePtr parse_range_type();

  /// StructType = "struct" "{" FieldSpec { "," FieldSpec } "}"
  NodePtr parse_struct_type();

  /// Generic = "|" TypeList "|"
  std::optional<GenericNode> parse_generic();

  // ── Expression Parsing (Pratt) ───────────────────────────────────────

  /// Pratt expression parser core.  `min_bp` is the minimum binding
  /// power the caller will accept (0 for top-level).
  NodePtr parse_expr_bp(int min_bp);

  /// Parse a "null denotation" — prefix position: literals, identifiers,
  /// unary ops, grouped expressions, if/for/switch/fn/spawn/import.
  NodePtr parse_prefix();

  /// Parse a "left denotation" — infix/postfix position: binary ops,
  /// call, index, selector, `or`.
  NodePtr parse_infix(NodePtr lhs, int bp);

  // ── Primary / Atom Expressions ───────────────────────────────────────

  NodePtr parse_identifier();
  NodePtr parse_number(); // IntegerLiteral or FloatLiteral
  NodePtr parse_bool_literal();
  NodePtr parse_string_literal(); // handles interpolation fragments
  NodePtr parse_array_literal();  // "[" ... "]"
  NodePtr parse_map_or_block();   // disambiguate "{" — map literal vs block

  NodePtr parse_struct_literal(NodePtr type_expr); // after type identifier
  NodePtr parse_group_or_range(); // "(" ... ")" — group expr or range

  // ── Compound Expressions ─────────────────────────────────────────────

  NodePtr parse_if_expr();
  NodePtr parse_for_expr();
  NodePtr parse_switch_expr();
  NodePtr parse_spawn_expr();
  NodePtr parse_func_expr(); // anonymous fn
  NodePtr parse_import_expr();

  // ── Sub-expression helpers ───────────────────────────────────────────

  NodePtr parse_call_args(NodePtr callee);      // after "("
  NodePtr parse_index_or_slice(NodePtr object); // after "["
  NodePtr parse_selector(NodePtr object);       // after "."
  NodePtr parse_or_expr(NodePtr expr);          // after "or"
  std::optional<IdentifierNode> parse_pipe();   // "|" ident "|"

  // ── Statement Parsing ────────────────────────────────────────────────

  NodePtr parse_statement();
  NodePtr parse_var_decl();    // Identifier Type [ "=" Expression ]
  NodePtr parse_decl_assign(); // IdentifierList ":=" ExpressionList
  NodePtr parse_assignment(NodePtr target); // target = ... | += ... | ...
  NodePtr parse_return();
  NodePtr parse_break();
  NodePtr parse_next();

  // ── Block Parsing ────────────────────────────────────────────────────
  // parse_block() is declared public above.

  // ── Declaration Parsing (top-level) ──────────────────────────────────

  NodePtr parse_declaration();
  NodePtr parse_const_decl(bool is_public);
  NodePtr parse_enum_decl(bool is_public);
  NodePtr parse_func_decl(bool is_public);
  NodePtr parse_import_decl();
  NodePtr parse_interface_decl(bool is_public);
  NodePtr parse_struct_decl(bool is_public);

  // ── Declaration sub-helpers ──────────────────────────────────────────

  SignatureNode parse_signature();
  ParameterNode parse_parameter();
  FieldSpecNode parse_field_spec();
  EnumFieldNode parse_enum_field();
  CaseArmNode parse_case_arm();
  std::optional<ReceiverNode> parse_receiver();

  /// EmbedName = Identifier [ "." Identifier ]
  /// Returns IdentifierNode for "Foo" or SelectorNode for "lib.Foo".
  /// Generic-typed embeds are not yet permitted.
  NodePtr parse_embed_name();

  // ── Module / Program ─────────────────────────────────────────────────

  NodePtr parse_package();
  NodePtr parse_source();
};
} // namespace saga
