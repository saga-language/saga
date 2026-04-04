// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "frontend/parser.hpp"
#include "frontend/ast.hpp"
#include "frontend/token.hpp"

#include <string>
#include <vector>

namespace mc {

// ============================================================================
// File-local helpers
// ============================================================================

namespace {

// Human-readable name for each token kind, used when building error messages.
// Every Kind enumerator is listed explicitly so the compiler will warn if a
// new one is added without updating this table.
constexpr std::string_view token_kind_name(Token::Kind kind) {
  switch (kind) {
  // Special
  case Token::Kind::Eof:
    return "end of file";
  case Token::Kind::Invalid:
    return "invalid token";
  case Token::Kind::Terminator:
    return "newline";
  case Token::Kind::Comment:
    return "comment";

  // Literals
  case Token::Kind::Identifier:
    return "identifier";
  case Token::Kind::BoolLiteral:
    return "boolean literal";
  case Token::Kind::FloatLiteral:
    return "float literal";
  case Token::Kind::IntegerLiteral:
    return "integer literal";
  case Token::Kind::StringLiteral:
    return "string literal";
  case Token::Kind::StringStart:
    return "string";
  case Token::Kind::StringMiddle:
    return "string";
  case Token::Kind::StringEnd:
    return "string";

  // Keywords
  case Token::Kind::Break:
    return "'break'";
  case Token::Kind::Case:
    return "'case'";
  case Token::Kind::Const:
    return "'const'";
  case Token::Kind::Else:
    return "'else'";
  case Token::Kind::Enum:
    return "'enum'";
  case Token::Kind::Fn:
    return "'fn'";
  case Token::Kind::For:
    return "'for'";
  case Token::Kind::If:
    return "'if'";
  case Token::Kind::Import:
    return "'import'";
  case Token::Kind::Interface:
    return "'interface'";
  case Token::Kind::Next:
    return "'next'";
  case Token::Kind::Or:
    return "'or'";
  case Token::Kind::Pub:
    return "'pub'";
  case Token::Kind::Return:
    return "'return'";
  case Token::Kind::Spawn:
    return "'spawn'";
  case Token::Kind::Struct:
    return "'struct'";
  case Token::Kind::Switch:
    return "'switch'";

  // Punctuation
  case Token::Kind::Comma:
    return "','";
  case Token::Kind::Colon:
    return "':'";
  case Token::Kind::Dot:
    return "'.'";
  case Token::Kind::DotDot:
    return "'..'";
  case Token::Kind::Ellipsis:
    return "'...'";
  case Token::Kind::QuestionMark:
    return "'?'";
  case Token::Kind::Semicolon:
    return "';'";

  // Assignment operators
  case Token::Kind::Assignment:
    return "'='";
  case Token::Kind::AddAssignment:
    return "'+='";
  case Token::Kind::DeclAssignment:
    return "':='";
  case Token::Kind::SubAssignment:
    return "'-='";
  case Token::Kind::MulAssignment:
    return "'*='";
  case Token::Kind::DivAssignment:
    return "'/='";

  // Arithmetic operators
  case Token::Kind::Add:
    return "'+'";
  case Token::Kind::Decrement:
    return "'--'";
  case Token::Kind::Divide:
    return "'/'";
  case Token::Kind::Increment:
    return "'++'";
  case Token::Kind::Modulo:
    return "'%'";
  case Token::Kind::Multiply:
    return "'*'";
  case Token::Kind::Pow:
    return "'**'";
  case Token::Kind::Sub:
    return "'-'";

  // Encapsulation
  case Token::Kind::LeftBrace:
    return "'{'";
  case Token::Kind::RightBrace:
    return "'}'";
  case Token::Kind::LeftBracket:
    return "'['";
  case Token::Kind::RightBracket:
    return "']'";
  case Token::Kind::LeftParenthesis:
    return "'('";
  case Token::Kind::RightParenthesis:
    return "')'";

  // Logical operators
  case Token::Kind::LogicalOr:
    return "'||'";
  case Token::Kind::LogicalAnd:
    return "'&&'";
  case Token::Kind::Equal:
    return "'=='";
  case Token::Kind::Not:
    return "'!'";
  case Token::Kind::NotEqual:
    return "'!='";
  case Token::Kind::LessThan:
    return "'<'";
  case Token::Kind::LessThanEqual:
    return "'<='";
  case Token::Kind::GreaterThan:
    return "'>'";
  case Token::Kind::GreaterThanEqual:
    return "'>='";

  // Bitwise operators
  case Token::Kind::BitwiseAnd:
    return "'&'";
  case Token::Kind::BitwiseNot:
    return "'~'";
  case Token::Kind::BitwiseOr:
    return "'|'";
  case Token::Kind::BitwiseXor:
    return "'^'";
  case Token::Kind::LeftShift:
    return "'<<'";
  case Token::Kind::RightShift:
    return "'>>'";
  }

  return "unknown"; // unreachable; guards against future enum additions
}

// Returns true when `kind` can legally begin a SingleType production.
// Used to guard TypeList and union-type loops so they stop before consuming
// a token that belongs to the surrounding context (e.g. a closing delimiter).
constexpr bool is_type_start(Token::Kind kind) {
  switch (kind) {
  case Token::Kind::Identifier:      // named types: Int, Bool, MyType, …
  case Token::Kind::LeftBracket:     // ArrayType   "[" Type "]"
  case Token::Kind::LeftBrace:       // MapType     "{" Type ":" Type "}"
  case Token::Kind::Fn:              // FuncType    "fn" Signature
  case Token::Kind::LeftParenthesis: // RangeType   "(" Type ")"
  case Token::Kind::Struct:          // StructType  "struct" "{" … "}"
    return true;
  default:
    return false;
  }
}

// Returns true when `kind` can legally begin an expression — i.e. it is a
// valid token in the prefix / null-denotation position of the Pratt parser.
// Used by parse_return and parse_break to decide whether a value follows the
// keyword on the same logical line, and by parse_prefix's error branch to
// produce a precise message when a non-expression token is encountered.
constexpr bool is_expression_start(Token::Kind kind) {
  switch (kind) {
  case Token::Kind::Identifier:
  case Token::Kind::IntegerLiteral:
  case Token::Kind::FloatLiteral:
  case Token::Kind::BoolLiteral:
  case Token::Kind::StringLiteral:
  case Token::Kind::StringStart:
  case Token::Kind::Not:             // unary !
  case Token::Kind::Sub:             // unary -
  case Token::Kind::BitwiseNot:      // unary ~
  case Token::Kind::LeftParenthesis: // grouped expr or range literal
  case Token::Kind::LeftBracket:     // array literal
  case Token::Kind::LeftBrace:       // map literal / block (Groups 3–4)
  case Token::Kind::If:
  case Token::Kind::For:
  case Token::Kind::Switch:
  case Token::Kind::Spawn:
  case Token::Kind::BitwiseOr: // |T| generic prefix for spawn / fn
  case Token::Kind::Fn:
  case Token::Kind::Import:
  case Token::Kind::Struct:          // anonymous struct literal
    return true;
  default:
    return false;
  }
}

// Returns true when `kind` is a plain or compound assignment operator that
// produces an AssignNode.  Excludes ":=" (DeclAssignment) which is handled
// separately as a DeclAssignNode because it introduces new bindings and
// restricts its left-hand side to plain identifiers.
constexpr bool is_assign_op(Token::Kind kind) {
  switch (kind) {
  case Token::Kind::Assignment:    // =
  case Token::Kind::AddAssignment: // +=
  case Token::Kind::SubAssignment: // -=
  case Token::Kind::MulAssignment: // *=
  case Token::Kind::DivAssignment: // /=
    return true;
  default:
    return false;
  }
}

} // namespace

// ============================================================================
// Token Helpers
// ============================================================================

// Consume the current token, store it in `previous`, then scan forward past
// any comment tokens (comments carry no semantic meaning and are invisible to
// all parsing logic above this point). Returns the token that was consumed.
//
// Call advance() once during parse_source() to prime `current` after
// initializing the lexer with a file.
Token Parser::advance() {
  previous = current;
  current = lexer.scan();

  while (current.kind == Token::Kind::Comment) {
    current = lexer.scan();
  }

  return previous;
}

// Look at the next token without consuming it.
Token Parser::peek() const {
  // Save lexer state.
  auto saved_offset = lexer.offset;
  auto saved_reading_offset = lexer.reading_offset;
  auto saved_state = lexer.state;
  auto saved_current = current;

  // Scan the next token (const_cast needed because scan mutates the lexer).
  auto &mutable_lexer = const_cast<Lexer &>(lexer);
  Token next = mutable_lexer.scan();
  while (next.kind == Token::Kind::Comment)
    next = mutable_lexer.scan();

  // Restore lexer state.
  mutable_lexer.offset = saved_offset;
  mutable_lexer.reading_offset = saved_reading_offset;
  mutable_lexer.state = saved_state;

  return next;
}

// Non-consuming test — is the current token of the given kind?
bool Parser::check(Token::Kind kind) const { return current.kind == kind; }

// Consuming test — if current matches, advance and return true.
bool Parser::match(Token::Kind kind) {
  if (!check(kind))
    return false;
  advance();
  return true;
}

// Conditional consume — if current matches, return the consumed token.
// Returns std::nullopt without advancing when the token does not match.
std::optional<Token> Parser::accept(Token::Kind kind) {
  if (!check(kind))
    return std::nullopt;
  return advance();
}

// Mandatory consume. If the current token matches, advance and return it.
// If it does not match, report an error and return a synthetic token at the
// current position WITHOUT advancing.
//
// Why no advance on error: silently dropping the mismatched token would make
// subsequent tokens shift unpredictably, leading to a cascade of spurious
// errors that mask the real problem. By leaving `current` unchanged, the
// caller or a later synchronize() can make a more informed recovery decision.
Token Parser::expect(Token::Kind kind) {
  if (check(kind))
    return advance();

  error("expected " + std::string(token_kind_name(kind)) + ", got " +
        std::string(token_kind_name(current.kind)));

  // Synthetic token: correct kind, empty literal, current position.
  return Token{kind, {}, current.offset};
}

// Skip all consecutive Terminator (newline) tokens and return the count.
// Used at positions where newlines are syntactically insignificant, such as
// inside bracket pairs or before a `{`.
int Parser::skip_terminators() {
  int count = 0;
  while (check(Token::Kind::Terminator)) {
    advance();
    ++count;
  }
  return count;
}

// Variant of skip_terminators used when the caller knows a specific closing
// token is expected next. The `closing` parameter is unused at runtime but
// serves as explicit documentation of intent at the call site.
void Parser::skip_terminators_before(Token::Kind /*closing*/) {
  while (check(Token::Kind::Terminator)) {
    advance();
  }
}

// True when parsing should stop: either the token stream is exhausted or we
// have hit the error budget. Checking max_reached() prevents infinite loops
// in error-recovery paths when every token triggers a fresh error.
bool Parser::is_at_end() const {
  return current.kind == Token::Kind::Eof || errors.max_reached();
}

// Report a syntax error at the position of the current token.
void Parser::error(const std::string &message) {
  errors.report_error(lexer.file->position_at(current.offset), message);
}

// Report a syntax error at the start of an explicit span. Useful when the
// parser has already advanced past the offending token and needs to point
// back to it (e.g. mismatched delimiters detected at the closing token).
void Parser::error_at(Span span, const std::string &message) {
  errors.report_error(lexer.file->position_at(span.start), message);
}

// Panic-mode error recovery. After a syntax error, skip tokens until we
// reach a point where parsing can safely resume, then return. The caller's
// outer loop will re-examine `current` and continue normally.
//
// Sync points (in priority order):
//   1. Terminator — consume it; the next token starts a fresh line.
//   2. RightBrace — stop WITHOUT consuming; the enclosing block parser
//      must see this token to close properly.
//   3. Declaration keywords — stop without consuming; the top-level loop
//      can begin a new declaration from here.
void Parser::synchronize() {
  while (!is_at_end()) {
    if (check(Token::Kind::Terminator)) {
      advance(); // consume the newline
      return;
    }

    if (check(Token::Kind::RightBrace)) {
      return; // let the enclosing block parser handle the closing brace
    }

    switch (current.kind) {
    case Token::Kind::Const:
    case Token::Kind::Enum:
    case Token::Kind::Fn:
    case Token::Kind::Import:
    case Token::Kind::Interface:
    case Token::Kind::Pub:
    case Token::Kind::Struct:
      return; // beginning of a new declaration; stop here
    default:
      advance(); // discard this token and keep looking
    }
  }
}

// ============================================================================
// Span Helpers
// ============================================================================

// Return the byte offset of the current token as a span start-marker.
// Call this at the very beginning of a parse_* function, before any advance.
//
//   auto start = mark();
//   ... parse children ...
//   auto node = make_node<Foo>(span_from(start), ...);
size_t Parser::mark() const { return current.offset; }

// Build a closed Span from a previously captured mark to the end of the most
// recently consumed token (`previous`). If no token has been consumed yet
// (parser has not been primed), the span degenerates to [start, start).
Span Parser::span_from(size_t start) const {
  const size_t end = previous.literal.empty()
                         ? start
                         : previous.offset + previous.literal.size();
  return Span{start, end};
}

// ============================================================================
// Precedence / Pratt Helpers
// ============================================================================

// Map an infix (or postfix) operator token to its left binding power.
// A return value of 0 means the token is not an infix operator and the
// Pratt loop should stop.
//
// Precedence levels (highest to lowest, matching AGENTS.md and grammar.md):
//
//   100  Access:      .  [  (
//    90  Power:       **               (right-associative: see parse_infix)
//    80  Multiply:    *  /  %
//    70  Add:         +  -
//    60  Bitwise:     &  |  ^  <<  >>
//    50  Comparison:  ==  !=  >  <  >=  <=
//    40  Logical AND: &&
//    30  Logical OR:  ||
//    25  Range:       ..
//    20  Or clause:   or
int Parser::infix_binding_power(Token::Kind kind) {
  switch (kind) {
  // 1. Access (postfix)
  case Token::Kind::Dot:
  case Token::Kind::LeftBracket:
  case Token::Kind::LeftParenthesis:
    return 100;

  // 2. Power (right-associative — handled in parse_infix via bp - 1)
  case Token::Kind::Pow:
    return 90;

  // 3. Multiplicative
  case Token::Kind::Multiply:
  case Token::Kind::Divide:
  case Token::Kind::Modulo:
    return 80;

  // 4. Additive
  case Token::Kind::Add:
  case Token::Kind::Sub:
    return 70;

  // 5. Bitwise
  case Token::Kind::BitwiseAnd:
  case Token::Kind::BitwiseOr:
  case Token::Kind::BitwiseXor:
  case Token::Kind::LeftShift:
  case Token::Kind::RightShift:
    return 60;

  // 6. Comparison
  case Token::Kind::Equal:
  case Token::Kind::NotEqual:
  case Token::Kind::LessThan:
  case Token::Kind::LessThanEqual:
  case Token::Kind::GreaterThan:
  case Token::Kind::GreaterThanEqual:
    return 50;

  // 7. Logical
  case Token::Kind::LogicalAnd:
    return 40;
  case Token::Kind::LogicalOr:
    return 30;

  // 8. Range / slice
  case Token::Kind::DotDot:
    return 25;

  // 9. Or clause (error resolution)
  case Token::Kind::Or:
    return 20;

  // 10. Struct initialiser  — "{" after a type expression begins a struct
  // literal.  bp = 1 is the lowest non-zero value, ensuring that any
  // expression context that must stop before a "{ body }" block can do so
  // by calling parse_expr_bp(1) instead of parse_expression().
  case Token::Kind::LeftBrace:
    return 1;

  default:
    return 0;
  }
}

// Map a prefix operator token to its right binding power — i.e. how tightly
// it binds to the expression on its right.
//
// All prefix operators sit between Access (100) and Power (90) in the
// precedence hierarchy, so `- 2 ** 3` parses as `(-2) ** 3`, matching the
// grammar's stated ordering (Unary > Power).
//
// Operators: ! (logical not), - (arithmetic negation), ~ (bitwise not)
int Parser::prefix_binding_power(Token::Kind kind) {
  switch (kind) {
  case Token::Kind::Not:
    return 95; // !
  case Token::Kind::Sub:
    return 95; // -
  case Token::Kind::BitwiseNot:
    return 95; // ~
  default:
    return 0;
  }
}

// ============================================================================
// Type Parsing
// ============================================================================

// parse_type — Type = UnionType
//
// Every type in the language passes through this entry point, which delegates
// directly to parse_union_type. The indirection exists so that call-sites can
// write `parse_type()` regardless of whether the grammar later adds a wrapper
// production above UnionType.
NodePtr Parser::parse_type() { return parse_union_type(); }

// parse_union_type — UnionType = SingleType { "|" SingleType }
//
// A union type is two or more single types joined with "|":
//   Int | String
//   Bool | Int | Void
//
// When there is only one alternative (the common case), the SingleType node is
// returned directly — no UnionTypeNode wrapper is created. UnionTypeNode is
// only produced when at least one "|" is actually consumed.
//
// The "|" loop is safe in all type-annotation contexts because the grammar
// guarantees that the token following a complete type in every position where
// parse_type is called (parameter lists, return lists, field declarations,
// etc.) is never "|":  it is always a delimiter such as ")", "}", "]", ",", or
// a line terminator.  parse_generic is the one other user of "|", but it parses
// type parameters as bare identifiers (matching the AST's IdentifierNode
// annotation) and never calls parse_type, so there is no ambiguity.
NodePtr Parser::parse_union_type() {
  auto start = mark();

  NodePtr first = parse_single_type();

  // Fast path — no "|" follows, so this is a plain single type.
  if (!check(Token::Kind::BitwiseOr))
    return first;

  // Slow path — at least one "|" follows; collect all alternatives.
  std::vector<NodePtr> types;
  types.push_back(std::move(first));

  while (check(Token::Kind::BitwiseOr)) {
    advance(); // consume "|"
    types.push_back(parse_single_type());
  }

  return make_node<UnionTypeNode>(span_from(start), std::move(types));
}

// parse_single_type — SingleType = Identifier | Selector | IntrinsicType |
// StructType
//
// Grammar recap:
//   IntrinsicType = ArrayType | FuncType | MapType | RangeType | StructType
//                 | basic_type | float_type | integer_type | void_type
//   basic_type    = "Bool" | "Byte" | "Int" | "Float" | "String"
//   float_type    = "Float32" | "Float64"
//   integer_type  = "Int8" | "Int16" | "Int32" | "Int64"
//                 | "Uint8" | "Uint16" | "Uint32" | "Uint64"
//   void_type     = "Void"
//   Selector      = Identifier "." Identifier   (qualified type, e.g.
//   pkg.MyType)
//
// All named types — built-in (Int, Bool, Void …) and user-defined — tokenize
// as plain Identifier tokens because the lexer does not treat them as keywords.
// Only the structural type introducers ("fn", "struct") arrive as keywords,
// and the compound types are recognised by their opening delimiter.
//
// Dispatch summary:
//   "["   → ArrayType   (delegated to parse_array_type)
//   "{"   → MapType     (delegated to parse_map_type)
//   "fn"  → FuncType    (delegated to parse_func_type)
//   "("   → RangeType   (delegated to parse_range_type)
//   "struct" → StructType (delegated to parse_struct_type)
//   Identifier → plain IdentifierNode, or SelectorNode if "." follows
NodePtr Parser::parse_single_type() {
  auto start = mark();

  switch (current.kind) {
  // ── Compound / intrinsic types ────────────────────────────────────────
  case Token::Kind::LeftBracket: // "[" Type "]"
    return parse_array_type();
  case Token::Kind::LeftBrace: // "{" Type ":" Type "}"
    return parse_map_type();
  case Token::Kind::Fn: // "fn" Signature
    return parse_func_type();
  case Token::Kind::LeftParenthesis: // "(" Type ")"
    return parse_range_type();
  case Token::Kind::Struct: // "struct" "{" [ FieldSpec { "," FieldSpec } ] "}"
    return parse_struct_type();

  // ── Named types: plain or qualified (Selector) ───────────────────────
  case Token::Kind::Identifier: {
    Token tok = advance(); // consume the type-name identifier
    NodePtr ident = make_node<IdentifierNode>(span_from(start), tok.literal);

    // Selector check: Identifier "." Identifier  →  e.g. io.Reader
    if (!check(Token::Kind::Dot))
      return ident; // plain named type — Int, MyStruct, T, …

    advance(); // consume "."
    auto field_start = mark();
    Token field_tok = expect(Token::Kind::Identifier);
    IdentifierNode field{span_from(field_start), field_tok.literal};
    return make_node<SelectorNode>(span_from(start), std::move(ident), field);
  }

  // ── Error ─────────────────────────────────────────────────────────────
  default:
    error("expected type, got " + std::string(token_kind_name(current.kind)));
    return nullptr;
  }
}

// ============================================================================
// Declaration / Signature Sub-helpers
// ============================================================================
//
// These helpers are declared under "Declaration sub-helpers" in the header but
// are implemented here because parse_func_type and parse_struct_type (both type
// parsers) call them directly.

// parse_field_spec — FieldSpec = IdentifierList Type
//
// Used for inline StructType fields.  The IdentifierList collects all
// comma-separated names; the type follows WITHOUT a preceding comma.
//
//   x, y Int      →  names=[x, y]  type=Int
//   z [String]    →  names=[z]     type=[String]
//
// Disambiguation: within the name loop we consume a "," only while the
// following token is an Identifier.  A compound type opener ([, {, fn, (,
// struct) or any non-Identifier after "," terminates the name list immediately.
FieldSpecNode Parser::parse_field_spec() {
  auto start = mark();

  // ── Names (IdentifierList) ────────────────────────────────────────────
  auto names_start = mark();
  std::vector<IdentifierNode> name_nodes;

  {
    auto id_start = mark();
    Token id = expect(Token::Kind::Identifier);
    name_nodes.push_back(IdentifierNode{span_from(id_start), id.literal});
  }

  while (check(Token::Kind::Comma)) {
    advance(); // consume ","
    if (!check(Token::Kind::Identifier))
      break; // compound type follows; the comma was actually a field separator
             // consumed one token early — type parsing below will handle it
    auto id_start = mark();
    Token id = advance();
    name_nodes.push_back(IdentifierNode{span_from(id_start), id.literal});
  }

  IdentifierListNode names{span_from(names_start), std::move(name_nodes)};

  // ── Type ─────────────────────────────────────────────────────────────
  NodePtr type = parse_type();

  return FieldSpecNode{span_from(start), std::move(names), std::move(type)};
}

// parse_parameter — IdentifierList ParameterType
//
// ParameterType = Type | VariadicType ("..." Type)
//
// The same name-collection strategy as parse_field_spec:
//   x, y Int      →  names=[x, y]  type=Int  is_variadic=false
//   rest ...String →  names=[rest]  type=String  is_variadic=true
ParameterNode Parser::parse_parameter() {
  auto start = mark();

  // ── Names (IdentifierList) ────────────────────────────────────────────
  auto names_start = mark();
  std::vector<IdentifierNode> name_nodes;

  {
    auto id_start = mark();
    Token id = expect(Token::Kind::Identifier);
    name_nodes.push_back(IdentifierNode{span_from(id_start), id.literal});
  }

  while (check(Token::Kind::Comma)) {
    advance(); // consume ","
    if (!check(Token::Kind::Identifier))
      break; // compound type or Ellipsis follows
    auto id_start = mark();
    Token id = advance();
    name_nodes.push_back(IdentifierNode{span_from(id_start), id.literal});
  }

  IdentifierListNode names{span_from(names_start), std::move(name_nodes)};

  // ── Variadic? ─────────────────────────────────────────────────────────
  bool is_variadic = false;
  if (check(Token::Kind::Ellipsis)) {
    advance(); // consume "..."
    is_variadic = true;
  }

  // ── Type ─────────────────────────────────────────────────────────────
  NodePtr type = parse_type();

  return ParameterNode{span_from(start), std::move(names), std::move(type),
                       is_variadic};
}

// parse_signature — "(" [ ParameterList ] ")" TypeList
//
// ParameterList = IdentifierList ParameterType { "," IdentifierList
// ParameterType } TypeList      = Type { "," Type }   (return types; empty ⟹
// Void)
//
// Newlines inside the parameter list are insignificant and are skipped.
// TypeList is parsed greedily: a "," continues the list only when the
// following token is a valid type start, preventing accidental consumption
// of a comma that belongs to an enclosing construct.
SignatureNode Parser::parse_signature() {
  auto start = mark();

  expect(Token::Kind::LeftParenthesis); // "("
  skip_terminators();

  // ── Parameter list ────────────────────────────────────────────────────
  std::vector<ParameterNode> params;
  if (!check(Token::Kind::RightParenthesis)) {
    params.push_back(parse_parameter());
    while (check(Token::Kind::Comma)) {
      advance(); // consume ","
      skip_terminators();
      if (check(Token::Kind::RightParenthesis))
        break; // tolerate trailing comma
      params.push_back(parse_parameter());
    }
    skip_terminators_before(Token::Kind::RightParenthesis);
  }

  expect(Token::Kind::RightParenthesis); // ")"

  // ── Return types (TypeList) ───────────────────────────────────────────
  // An absent TypeList is valid and means Void (returns stays empty).
  //
  // LeftBrace is explicitly excluded from the type-start check: a "{" after
  // the closing ")" always begins the function body, never a map-type return
  // annotation.  Without this exclusion, "fn(x Int) { }" would attempt to
  // parse the body block as a return type and fail.
  //
  // The comma loop advances past "," before re-checking, which is the only
  // way to inspect the token that follows the comma without a peek function.
  // Consequence: if a comma is consumed and the next token turns out not to
  // be a type start, that comma is lost (no backtracking).  In practice this
  // only arises when a FuncType appears as a parameter type inside another
  // signature (e.g. fn(f fn(Int) Bool, x Int)); the inner TypeList would
  // greedily consume the outer comma.  That case is deferred to parse_infix
  // implementation where the full call-site context is available.
  std::vector<NodePtr> returns;

  auto is_return_type_start = [](Token::Kind k) {
    return k != Token::Kind::LeftBrace && is_type_start(k);
  };

  if (is_return_type_start(current.kind)) {
    returns.push_back(parse_type());
    while (check(Token::Kind::Comma)) {
      advance(); // consume ","
      if (!is_return_type_start(current.kind))
        break;
      returns.push_back(parse_type());
    }
  }

  return SignatureNode{span_from(start), std::move(params), std::move(returns)};
}

// ============================================================================
// Sub-type Parsers
// ============================================================================

// parse_array_type — ArrayType = "[" Type "]"
NodePtr Parser::parse_array_type() {
  auto start = mark();
  expect(Token::Kind::LeftBracket); // "["
  NodePtr element = parse_type();
  expect(Token::Kind::RightBracket); // "]"
  return make_node<ArrayTypeNode>(span_from(start), std::move(element));
}

// parse_map_type — MapType = "{" Type ":" Type "}"
NodePtr Parser::parse_map_type() {
  auto start = mark();
  expect(Token::Kind::LeftBrace); // "{"
  NodePtr key = parse_type();
  expect(Token::Kind::Colon); // ":"
  NodePtr value = parse_type();
  expect(Token::Kind::RightBrace); // "}"
  return make_node<MapTypeNode>(span_from(start), std::move(key),
                                std::move(value));
}

// parse_range_type — RangeType = "(" Type ")"
NodePtr Parser::parse_range_type() {
  auto start = mark();
  expect(Token::Kind::LeftParenthesis); // "("
  NodePtr element = parse_type();
  expect(Token::Kind::RightParenthesis); // ")"
  return make_node<RangeTypeNode>(span_from(start), std::move(element));
}

// parse_struct_type — StructType = "struct" "{" [ FieldSpec { "," FieldSpec } ]
// "}"
//
// Fields are comma-separated (not newline-separated; that convention is
// reserved for StructDecl).  Trailing commas are tolerated.
NodePtr Parser::parse_struct_type() {
  auto start = mark();
  expect(Token::Kind::Struct);    // "struct"
  expect(Token::Kind::LeftBrace); // "{"
  skip_terminators();

  std::vector<FieldSpecNode> fields;
  if (!check(Token::Kind::RightBrace)) {
    fields.push_back(parse_field_spec());
    while (check(Token::Kind::Comma)) {
      advance(); // consume ","
      skip_terminators();
      if (check(Token::Kind::RightBrace))
        break; // trailing comma
      fields.push_back(parse_field_spec());
    }
    skip_terminators_before(Token::Kind::RightBrace);
  }

  expect(Token::Kind::RightBrace); // "}"
  return make_node<StructTypeNode>(span_from(start), std::move(fields));
}

// parse_func_type — FuncType = "fn" "(" [ TypeList ] ")" Type
//
// FuncTypeNode stores type nodes only (no parameter names).
// Supports both bare types (e.g. `fn(Int, String) Bool`) and named
// parameters (e.g. `fn(x Int, y String) Bool`).  Named parameters are
// detected by looking ahead: if the current token is a lowercase identifier
// and the next starts a type (and isn't "," or ")"), the identifier is a
// parameter name that gets discarded.
NodePtr Parser::parse_func_type() {
  auto start = mark();
  expect(Token::Kind::Fn); // "fn"
  expect(Token::Kind::LeftParenthesis); // "("
  skip_terminators();

  // ── Parameter types ───────────────────────────────────────────────────
  std::vector<NodePtr> params;
  if (!check(Token::Kind::RightParenthesis)) {
    auto skip_param_names = [&]() {
      // Detect named parameters: if current is an identifier and the next
      // is a type-start (not "," or ")"), the identifier is a name.
      // Skip all names in a group: "x, y Int" → skip "x, y".
      while (check(Token::Kind::Identifier)) {
        auto next = peek();
        if (next.kind == Token::Kind::Comma || next.kind == Token::Kind::RightParenthesis) {
          // The identifier IS the type (e.g. `Int`), don't skip.
          break;
        }
        if (next.kind == Token::Kind::Ellipsis || is_type_start(next.kind)) {
          // Next token starts a type — current identifier is a name.
          advance(); // skip name
          // Skip more names in the same group: "x, y Int"
          while (check(Token::Kind::Comma)) {
            auto after_comma = peek();
            if (after_comma.kind != Token::Kind::Identifier)
              break;
            // Consume comma, then check if the following identifier is
            // also a name (has a type-start after it) or is the type itself.
            advance(); // consume ","
            auto after_ident = peek();
            if (after_ident.kind == Token::Kind::Comma ||
                after_ident.kind == Token::Kind::RightParenthesis) {
              // This identifier is the type. But we already consumed the comma.
              // That means the comma was a parameter separator.
              // The type is the current identifier — don't skip it.
              break;
            }
            // It's another name in the same group.
            advance(); // skip name
          }
          break;
        }
        break;
      }
      // Skip variadic marker.
      if (check(Token::Kind::Ellipsis))
        advance();
    };

    skip_param_names();
    params.push_back(parse_type());
    while (check(Token::Kind::Comma)) {
      advance(); // consume ","
      skip_terminators();
      if (check(Token::Kind::RightParenthesis))
        break;
      skip_param_names();
      params.push_back(parse_type());
    }
    skip_terminators_before(Token::Kind::RightParenthesis);
  }

  expect(Token::Kind::RightParenthesis); // ")"

  // ── Return type ───────────────────────────────────────────────────────
  // Parse a single return type only.  Multi-return in function types would
  // be ambiguous with commas separating outer parameters (e.g.
  // `fn(f fn(Int) Int, x Int)` — the `,` after `Int` belongs to the outer
  // parameter list, not to the inner return type list).
  auto is_return_type_start = [](Token::Kind k) {
    return k != Token::Kind::LeftBrace && is_type_start(k);
  };

  std::vector<NodePtr> returns;
  if (is_return_type_start(current.kind)) {
    returns.push_back(parse_type());
  }

  return make_node<FuncTypeNode>(span_from(start), std::move(params),
                                 std::move(returns));
}

// ============================================================================
// Declaration Parsing
// ============================================================================

// ============================================================================
// Expression Parsing (Pratt / top-down operator precedence)
// ============================================================================

// parse_expression — Expression = LogicalExpression [ OrExpr ]
//
// Top-level entry point: parse a full expression with no minimum binding
// power constraint (min_bp = 0 accepts every operator).  The `or` clause
// at bp = 20 is handled transparently by the Pratt loop inside parse_expr_bp,
// so this function needs no special-casing.
NodePtr Parser::parse_expression() { return parse_expr_bp(0); }

// parse_expr_bp — Pratt core loop.
//
// min_bp  the minimum *left* binding power the caller will accept.  Any
//         infix operator whose left-bp is ≤ min_bp belongs to an outer
//         expression; the loop stops and returns the current LHS to the
//         caller, which will consume that operator.
//
// Algorithm:
//   1. Call parse_prefix() to obtain the initial LHS (the "null denotation"
//      of the leading token).
//   2. Peek at the current token.  Look up its left binding power.
//   3. If that bp is ≤ min_bp the loop exits and LHS is returned.
//   4. Otherwise, call parse_infix(lhs, bp) which:
//        a. consumes the operator token,
//        b. recursively calls parse_expr_bp with the appropriate min_bp to
//           gather the right operand (bp for left-assoc, bp-1 for right-assoc),
//        c. returns a new composite node.
//      That node becomes the new LHS and the loop repeats.
//
// Right-associativity ("**", bp = 90):
//   parse_infix calls parse_expr_bp(bp - 1) = parse_expr_bp(89) so that a
//   second "**" at bp = 90 satisfies 90 > 89 and is folded rightward.
//
// Terminator / non-operator tokens:
//   infix_binding_power returns 0 for any token that is not an infix
//   operator.  Since min_bp is always ≥ 0, the condition 0 ≤ min_bp is
//   always true, so the loop stops naturally without consuming those tokens.
NodePtr Parser::parse_expr_bp(int min_bp) {
  NodePtr lhs = parse_prefix();
  if (!lhs)
    return nullptr;

  while (!is_at_end()) {
    int bp = infix_binding_power(current.kind);
    if (bp <= min_bp)
      break;

    lhs = parse_infix(std::move(lhs), bp);
    if (!lhs)
      return nullptr;
  }

  return lhs;
}

// parse_prefix — null denotation (nud): tokens that begin an expression.
//
// Dispatch table (Group 1 — no parse_block dependency):
//
//   Atoms           Identifier | number | bool | string
//   Unary ops       !  -  ~   →  UnaryExprNode (inline)
//   Grouped / range "(" … ")" →  parse_group_or_range()
//   Array literal   "[" … "]" →  parse_array_literal()
//   Import expr     "import"  →  parse_import_expr()
//
// Compound forms that require parse_block (if / for / switch / fn / spawn /
// map-or-block) are added in Groups 3 and 4.
NodePtr Parser::parse_prefix() {
  switch (current.kind) {

  // ── Atoms ──────────────────────────────────────────────────────────────
  case Token::Kind::Identifier:
    return parse_identifier();

  case Token::Kind::IntegerLiteral:
  case Token::Kind::FloatLiteral:
    return parse_number();

  case Token::Kind::BoolLiteral:
    return parse_bool_literal();

  case Token::Kind::StringLiteral:
  case Token::Kind::StringStart:
    return parse_string_literal();

  // ── Unary operators ────────────────────────────────────────────────────
  //
  // Grammar: UnaryExpr = unary_operator PrimaryExpr
  //          unary_operator = "!" | "-" | "~"
  //
  // All three share rbp = 95, placing them above Power (90) so that
  // `-2 ** 3` parses as `(-2) ** 3`, and below Access (100) so that
  // `-arr[0]` parses as `-(arr[0])`.
  case Token::Kind::Not:
  case Token::Kind::Sub:
  case Token::Kind::BitwiseNot: {
    auto start = mark();
    Token op = advance();
    int rbp = prefix_binding_power(op.kind);
    NodePtr operand = parse_expr_bp(rbp);
    if (!operand)
      return nullptr;
    return make_node<UnaryExprNode>(span_from(start), op.kind,
                                    std::move(operand));
  }

  // ── Grouped expression or range literal ────────────────────────────────
  //
  // Grammar: GroupExpr = "(" Expression ")"
  //          RangeExpr = "(" Expression ".." Expression ")"
  case Token::Kind::LeftParenthesis:
    return parse_group_or_range();

  // ── Array literal ───────────────────────────────────────────────────────
  //
  // Grammar: ArrayLiteral = "[" [ Expression { "," Expression } ] "]"
  case Token::Kind::LeftBracket:
    return parse_array_literal();

  // ── Import expression ───────────────────────────────────────────────────
  //
  // Grammar: ImportExpr = "import" StringLiteral
  case Token::Kind::Import:
    return parse_import_expr();

  // ── Compound expressions (Group 3) ─────────────────────────────────────
  case Token::Kind::If:
    return parse_if_expr();

  case Token::Kind::Switch:
    return parse_switch_expr();

  case Token::Kind::For:
    return parse_for_expr();

  case Token::Kind::Fn:
    return parse_func_expr();

  // spawn without a generic type: "spawn { }" or "spawn workerFn"
  case Token::Kind::Spawn:
    return parse_spawn_expr();

  // "|T| spawn …" — generic spawn; BitwiseOr is the only prefix use of "|"
  // in the grammar (FuncExpr's generic comes after "fn", not before it).
  case Token::Kind::BitwiseOr:
    return parse_spawn_expr();

  // ── Anonymous struct type (prefix for struct literal) ──────────────────────
  //
  // Grammar: StructLiteral = StructType StructInitializer
  //          StructType    = "struct" "{" [ FieldSpec { "," FieldSpec } ] "}"
  //
  // parse_struct_type() produces a StructTypeNode.  The Pratt loop then sees
  // the following "{" as an infix LeftBrace (bp = 1) and dispatches to
  // parse_struct_literal(), which consumes the StructInitializer.
  case Token::Kind::Struct:
    return parse_struct_type();

    // ── Compound expressions (Group 4 remainder) ─────────────────────────────
  case Token::Kind::LeftBrace:
    return parse_map_or_block();

  default:
    error("unexpected token in expression: " +
          std::string(token_kind_name(current.kind)));
    return nullptr;
  }
}

// parse_infix — left denotation (led): tokens that extend a left-hand side.
//
// Dispatch by token kind:
//   LeftBrace       → struct literal initialiser
//   Dot             → selector (member access)
//   LeftParenthesis → call expression
//   LeftBracket     → index or slice expression
//   Or              → or-clause (error resolution)
//   DotDot          → binary range operator (inside index/slice context)
//   Pow             → right-associative binary operator
//   all others      → left-associative binary operator
NodePtr Parser::parse_infix(NodePtr lhs, int bp) {
  auto start_offset = lhs->span.start;

  switch (current.kind) {
  case Token::Kind::LeftBrace:
    return parse_struct_literal(std::move(lhs));

  case Token::Kind::Dot:
    return parse_selector(std::move(lhs));

  case Token::Kind::LeftParenthesis:
    return parse_call_args(std::move(lhs));

  case Token::Kind::LeftBracket:
    return parse_index_or_slice(std::move(lhs));

  case Token::Kind::Or:
    return parse_or_expr(std::move(lhs));

  case Token::Kind::Pow: {
    Token op = advance();
    NodePtr rhs = parse_expr_bp(bp - 1);
    if (!rhs)
      return nullptr;
    return make_node<BinaryExprNode>(span_from(start_offset), std::move(lhs),
                                     op.kind, std::move(rhs));
  }

  default: {
    Token op = advance();
    NodePtr rhs = parse_expr_bp(bp);
    if (!rhs)
      return nullptr;
    return make_node<BinaryExprNode>(span_from(start_offset), std::move(lhs),
                                     op.kind, std::move(rhs));
  }
  }
}

// parse_selector — Selector = PrimaryExpr "." Identifier
NodePtr Parser::parse_selector(NodePtr object) {
  auto start_offset = object->span.start;
  advance(); // consume "."

  auto field_start = mark();
  Token field_tok = expect(Token::Kind::Identifier);
  IdentifierNode field{span_from(field_start), field_tok.literal};

  return make_node<SelectorNode>(span_from(start_offset), std::move(object),
                                 std::move(field));
}

// parse_call_args — CallExpr = PrimaryExpr "(" [ ExpressionList ] ")"
//
// ExpressionList = Expression { "," Expression }
//
// Newlines are insignificant inside the argument list.  Trailing commas
// are tolerated.
NodePtr Parser::parse_call_args(NodePtr callee) {
  auto start_offset = callee->span.start;
  advance(); // consume "("
  skip_terminators();

  std::vector<NodePtr> args;
  while (!check(Token::Kind::RightParenthesis) && !is_at_end()) {
    args.push_back(parse_expression());
    skip_terminators();
    if (!check(Token::Kind::Comma))
      break;
    advance(); // consume ","
    skip_terminators();
  }

  skip_terminators_before(Token::Kind::RightParenthesis);
  expect(Token::Kind::RightParenthesis);

  return make_node<CallExprNode>(span_from(start_offset), std::move(callee),
                                 std::move(args));
}

// parse_index_or_slice — IndexExpr = PrimaryExpr "[" ( Expression | Slice ) "]"
//                        Slice     = [ Expression ] ".." [ Expression ]
//
// Disambiguation: after "[", if we immediately see ".." it is a slice with
// an absent low bound.  Otherwise parse an expression; if ".." follows it
// is a slice (the expression is the low bound), otherwise it is an index.
NodePtr Parser::parse_index_or_slice(NodePtr object) {
  auto start_offset = object->span.start;
  advance(); // consume "["
  skip_terminators();

  if (check(Token::Kind::DotDot)) {
    auto slice_start = mark();
    advance(); // consume ".."
    skip_terminators();

    std::optional<NodePtr> high;
    if (!check(Token::Kind::RightBracket))
      high = parse_expression();

    skip_terminators_before(Token::Kind::RightBracket);
    expect(Token::Kind::RightBracket);

    NodePtr slice = make_node<SliceNode>(
        span_from(slice_start), std::optional<NodePtr>{}, std::move(high));
    return make_node<IndexExprNode>(span_from(start_offset), std::move(object),
                                    std::move(slice));
  }

  NodePtr first = parse_expr_bp(25);
  if (!first)
    return nullptr;
  skip_terminators();

  if (check(Token::Kind::DotDot)) {
    auto slice_start = first->span.start;
    advance(); // consume ".."
    skip_terminators();

    std::optional<NodePtr> high;
    if (!check(Token::Kind::RightBracket))
      high = parse_expression();

    skip_terminators_before(Token::Kind::RightBracket);
    expect(Token::Kind::RightBracket);

    NodePtr slice = make_node<SliceNode>(span_from(slice_start),
                                         std::make_optional(std::move(first)),
                                         std::move(high));
    return make_node<IndexExprNode>(span_from(start_offset), std::move(object),
                                    std::move(slice));
  }

  skip_terminators_before(Token::Kind::RightBracket);
  expect(Token::Kind::RightBracket);

  return make_node<IndexExprNode>(span_from(start_offset), std::move(object),
                                  std::move(first));
}

// parse_or_expr — OrExpr = Expression "or" [ IdentifierPipe ] Block
NodePtr Parser::parse_or_expr(NodePtr expr) {
  auto start_offset = expr->span.start;
  advance(); // consume "or"

  std::optional<IdentifierNode> pipe = parse_pipe();

  skip_terminators();
  NodePtr fallback = parse_block();

  return make_node<OrExprNode>(span_from(start_offset), std::move(expr),
                               std::move(pipe), std::move(fallback));
}

// parse_const_decl — ConstDecl = "const" Identifier [ Type ] "=" Expression
//
// The optional Type is present when the token after the identifier is not "=".
// Since "=" is never a valid type-start, the disambiguation is unambiguous.
NodePtr Parser::parse_const_decl(bool is_public) {
  auto start = mark();
  expect(Token::Kind::Const);

  auto name_start = mark();
  Token name_tok = expect(Token::Kind::Identifier);
  IdentifierNode name{span_from(name_start), name_tok.literal};

  std::optional<NodePtr> type;
  if (!check(Token::Kind::Assignment)) {
    type = parse_type();
  }

  expect(Token::Kind::Assignment);

  NodePtr value = parse_expression();

  return make_node<ConstDeclNode>(span_from(start), is_public, std::move(name),
                                  std::move(type), std::move(value));
}

// parse_enum_decl — EnumDecl = "enum" Identifier "{" EnumField
//                              { terminal EnumField } "}"
//
// EnumField      = Identifier [ EnumInitializer ]
// EnumInitializer = "{" Identifier ":" Expression
//                   [ "," Identifier ":" Expression ] "}"
//
// Fields are separated by terminators (newlines).
NodePtr Parser::parse_enum_decl(bool is_public) {
  auto start = mark();
  expect(Token::Kind::Enum);

  auto name_start = mark();
  Token name_tok = expect(Token::Kind::Identifier);
  IdentifierNode name{span_from(name_start), name_tok.literal};

  skip_terminators();
  expect(Token::Kind::LeftBrace);
  skip_terminators();

  std::vector<EnumFieldNode> fields;
  while (!check(Token::Kind::RightBrace) && !is_at_end()) {
    fields.push_back(parse_enum_field());
    skip_terminators();
  }

  expect(Token::Kind::RightBrace);

  return make_node<EnumDeclNode>(span_from(start), is_public, std::move(name),
                                 std::move(fields));
}

// parse_enum_field — EnumField = Identifier [ EnumInitializer ]
// EnumInitializer = "{" Identifier ":" Expression
//                   [ "," Identifier ":" Expression ] "}"
EnumFieldNode Parser::parse_enum_field() {
  auto start = mark();

  auto name_start = mark();
  Token name_tok = expect(Token::Kind::Identifier);
  IdentifierNode name{span_from(name_start), name_tok.literal};

  std::vector<FieldAssignmentNode> initializer;

  if (check(Token::Kind::LeftBrace)) {
    advance();
    skip_terminators();

    while (!check(Token::Kind::RightBrace) && !is_at_end()) {
      auto fa_start = mark();

      auto fa_name_start = mark();
      Token fa_name_tok = expect(Token::Kind::Identifier);
      IdentifierNode fa_name{span_from(fa_name_start), fa_name_tok.literal};

      expect(Token::Kind::Colon);

      NodePtr value = parse_expression();

      initializer.push_back(FieldAssignmentNode{
          span_from(fa_start), std::move(fa_name), std::move(value)});

      skip_terminators();
      if (check(Token::Kind::Comma)) {
        advance();
        skip_terminators();
      }
    }

    expect(Token::Kind::RightBrace);
  }

  return EnumFieldNode{span_from(start), std::move(name),
                       std::move(initializer)};
}

// parse_func_decl — FuncDecl = "fn" [ Generic ] [ Receiver ] Identifier
//                              Signature Block
//
// Grammar:
//   Generic   = "|" TypeList "|"
//   Receiver  = "(" Identifier Type ")"
//   Signature = "(" [ ParameterList ] ")" TypeList
//   Block     = "{" { Statement | Expression } "}"
//
// Disambiguation of Receiver vs Identifier:
//   After "fn" and the optional Generic, if the current token is "(" the
//   next production is a Receiver (e.g. `fn (u User) Name(…) …`).  Otherwise,
//   the current token must be the function name Identifier (e.g. `fn Name(…)
//   …`). This is unambiguous because the function name is always a plain
//   identifier and Signature's opening "(" only appears after the name.
NodePtr Parser::parse_func_decl(bool is_public) {
  auto start = mark();
  expect(Token::Kind::Fn); // consume "fn"

  std::optional<GenericNode> generic = parse_generic();

  std::optional<ReceiverNode> receiver = parse_receiver();

  auto name_start = mark();
  Token name_tok = expect(Token::Kind::Identifier);
  IdentifierNode name{span_from(name_start), name_tok.literal};

  SignatureNode sig = parse_signature();

  skip_terminators();
  NodePtr body = parse_block();

  return make_node<FuncDeclNode>(
      span_from(start), is_public, std::move(generic), std::move(receiver),
      std::move(name), std::move(sig), std::move(body));
}

// parse_receiver — Receiver = "(" Identifier Type ")"
//
// Returns std::nullopt when the current token is not "(" (leaving the
// token stream unchanged so the caller can treat the receiver as absent).
//
// Inside the parentheses exactly two things appear: a binding name (an
// Identifier token) and a Type.  Example: `(u User)`, `(s *MyStruct)`.
std::optional<ReceiverNode> Parser::parse_receiver() {
  if (!check(Token::Kind::LeftParenthesis))
    return std::nullopt;

  auto start = mark();
  advance(); // consume "("

  auto name_start = mark();
  Token name_tok = expect(Token::Kind::Identifier);
  IdentifierNode name{span_from(name_start), name_tok.literal};

  NodePtr type = parse_type();

  expect(Token::Kind::RightParenthesis); // ")"

  return ReceiverNode{span_from(start), std::move(name), std::move(type)};
}

// parse_interface_decl — InterfaceDecl = "interface" [ Generic ] Identifier
//                        "{" [ InterfaceField { terminal InterfaceField } ] "}"
//
// InterfaceField = [ "pub" ] Identifier Signature
//
// Fields are separated by terminators (newlines).  The closing "}" may appear
// on the same line as the last field or on a new line.
NodePtr Parser::parse_interface_decl(bool is_public) {
  auto start = mark();
  expect(Token::Kind::Interface);

  std::optional<GenericNode> generic = parse_generic();

  auto name_start = mark();
  Token name_tok = expect(Token::Kind::Identifier);
  IdentifierNode name{span_from(name_start), name_tok.literal};

  skip_terminators();
  expect(Token::Kind::LeftBrace);
  skip_terminators();

  std::vector<InterfaceFieldNode> methods;

  while (!check(Token::Kind::RightBrace) && !is_at_end()) {
    auto field_start = mark();
    bool field_public = match(Token::Kind::Pub);

    auto field_name_start = mark();
    Token field_name_tok = expect(Token::Kind::Identifier);
    IdentifierNode field_name{span_from(field_name_start),
                              field_name_tok.literal};

    SignatureNode sig = parse_signature();

    methods.push_back(InterfaceFieldNode{span_from(field_start), field_public,
                                         std::move(field_name),
                                         std::move(sig)});
    skip_terminators();
  }

  expect(Token::Kind::RightBrace);

  return make_node<InterfaceDeclNode>(span_from(start), is_public,
                                      std::move(generic), std::move(name),
                                      std::move(methods));
}

// parse_struct_decl — StructDecl = "struct" [ Generic ] Identifier
//                     [ "<" IdentifierList ] "{" [ StructMember
//                     { terminal StructMember } ] "}"
//
// StructMember = [ "pub" ] ( FieldSpec | FuncDecl )
//
// Disambiguation: after optional "pub", "fn" starts a FuncDecl; anything
// else starts a FieldSpec.  Members are separated by terminators (newlines).
NodePtr Parser::parse_struct_decl(bool is_public) {
  auto start = mark();
  expect(Token::Kind::Struct);

  std::optional<GenericNode> generic = parse_generic();

  auto name_start = mark();
  Token name_tok = expect(Token::Kind::Identifier);
  IdentifierNode name{span_from(name_start), name_tok.literal};

  std::vector<IdentifierNode> embeds;
  if (check(Token::Kind::LessThan)) {
    advance();
    auto id_start = mark();
    Token id_tok = expect(Token::Kind::Identifier);
    embeds.push_back(IdentifierNode{span_from(id_start), id_tok.literal});

    while (check(Token::Kind::Comma)) {
      advance();
      auto eid_start = mark();
      Token eid_tok = expect(Token::Kind::Identifier);
      embeds.push_back(IdentifierNode{span_from(eid_start), eid_tok.literal});
    }
  }

  skip_terminators();
  expect(Token::Kind::LeftBrace);
  skip_terminators();

  std::vector<StructMemberNode> members;
  while (!check(Token::Kind::RightBrace) && !is_at_end()) {
    auto member_start = mark();
    bool member_public = match(Token::Kind::Pub);

    if (check(Token::Kind::Fn)) {
      NodePtr func = parse_func_decl(member_public);
      members.push_back(StructMemberNode{span_from(member_start), member_public,
                                         std::move(func)});
    } else {
      FieldSpecNode field = parse_field_spec();
      NodePtr field_node = make_node<FieldSpecNode>(
          field.span, std::move(field.names), std::move(field.type));
      members.push_back(StructMemberNode{span_from(member_start), member_public,
                                         std::move(field_node)});
    }

    skip_terminators();
  }

  expect(Token::Kind::RightBrace);

  return make_node<StructDeclNode>(span_from(start), is_public,
                                   std::move(generic), std::move(name),
                                   std::move(embeds), std::move(members));
}

// ── parse_import_decl ────────────────────────────────────────────────────────
//
// ImportDecl = "import" StringLiteral
//
// The lexer stores string literals including their surrounding quotes:
//   "std/io"  →  token.literal == "\"std/io\""
// We strip the outer quotes so ImportDeclNode.path holds the bare path.

NodePtr Parser::parse_import_decl() {
  auto start = mark();
  expect(Token::Kind::Import);

  Token path_tok = expect(Token::Kind::StringLiteral);

  std::string_view path = path_tok.literal;
  if (path.size() >= 2)
    path = path.substr(1, path.size() - 2);

  return make_node<ImportDeclNode>(span_from(start), path);
}

// ── parse_declaration ────────────────────────────────────────────────────────
//
// Declaration = [ "pub" ] ( ConstDecl | EnumDecl | FuncDecl | ImportDecl
//                         | InterfaceDecl | StructDecl )
//
// "pub import" is syntactically accepted here so the import node can still be
// produced for subsequent passes; the semantic error is reported immediately.

NodePtr Parser::parse_declaration() {
  bool is_public = match(Token::Kind::Pub);

  switch (current.kind) {
  case Token::Kind::Const:
    return parse_const_decl(is_public);
  case Token::Kind::Enum:
    return parse_enum_decl(is_public);
  case Token::Kind::Fn:
    return parse_func_decl(is_public);
  case Token::Kind::Interface:
    return parse_interface_decl(is_public);
  case Token::Kind::Struct:
    return parse_struct_decl(is_public);

  case Token::Kind::Import:
    if (is_public)
      error("'pub' cannot be applied to an import declaration");
    return parse_import_decl();

  default:
    error("expected a declaration, got " +
          std::string(token_kind_name(current.kind)));
    synchronize();
    return nullptr;
  }
}

// ============================================================================
// Entry Points
// ============================================================================

NodePtr Parser::parse() { return parse_package(); }

// init_for_file — initialise the lexer from a single file and prime `current`.
//
// Equivalent to the per-file setup inside parse_package() but limited to one
// file.  Intended for call-sites (primarily tests) that drive
// parse_expression() directly rather than going through the full parse() →
// parse_source() pipeline.
void Parser::init_for_file(File *f) {
  lexer.init(f);
  advance();
}

NodePtr Parser::parse_package() {
  auto start = mark();
  std::vector<NodePtr> sources;

  for (auto &file : fileset.files) {
    lexer.init(file.get());
    advance();
    sources.push_back(parse_source());
  }

  return make_node<PackageNode>(span_from(start), std::move(sources));
}

// parse_source drives the top-level declaration loop for a single file.
//
// If parse_declaration returns nullptr (stub or error recovery), we check
// whether the token position advanced. If it did not, we force one step
// forward to guarantee the loop always terminates.
NodePtr Parser::parse_source() {
  auto start = mark();
  std::vector<NodePtr> declarations;

  while (!is_at_end()) {
    skip_terminators();
    if (is_at_end())
      break;

    size_t pos_before = current.offset;
    auto decl = parse_declaration();

    if (decl) {
      declarations.push_back(std::move(decl));
    } else if (current.offset == pos_before) {
      // No progress was made — force one token forward to avoid an
      // infinite loop. This is a last-resort guard; well-implemented
      // parsers should always advance at least one token on nullptr.
      advance();
    }
  }

  return make_node<SourceNode>(span_from(start), std::move(declarations));
}

// ============================================================================
// Primary / Atom Expression Parsing
// ============================================================================

// parse_identifier — Identifier = letter { letter | decimal_digit } [ "?" ]
//
// The lexer's scan_identifier() absorbs the optional trailing "?" into the
// token literal, so `value?` arrives as a single Identifier token whose
// literal is "value?". We simply consume that token and wrap its literal in
// an IdentifierNode.
//
// On mismatch, expect() reports a "expected identifier" error and returns a
// synthetic token without advancing, so subsequent parsing can still proceed.
NodePtr Parser::parse_identifier() {
  auto start = mark();
  Token tok = expect(Token::Kind::Identifier);
  return make_node<IdentifierNode>(span_from(start), tok.literal);
}

// parse_number — IntegerLiteral | FloatLiteral
//
// Grammar:
//   IntegerLiteral = decimal_digit { decimal_digit | "_" }
//                  | "0b" { binary_digit | "_" }
//                  | "0x" { hex_digit    | "_" }
//                  | "0o" { octal_digit  | "_" }
//   FloatLiteral   = decimal_digit { decimal_digit | "_" } "."
//                    decimal_digit { decimal_digit | "_" } [ Exponent ]
//   Exponent       = ("e" | "E") [ "+" | "-" ] decimal_digit { decimal_digit |
//   "_" }
//
// The lexer has already done the heavy lifting: the entire numeric literal
// (including any prefix like "0x" and internal underscores) arrives as a
// single IntegerLiteral or FloatLiteral token. We just consume it and wrap
// the raw literal text in the appropriate AST node.
//
// If the current token is neither, we report an error and return an
// IntegerLiteralNode with an empty literal so callers always receive a
// non-null node.
NodePtr Parser::parse_number() {
  auto start = mark();

  if (check(Token::Kind::IntegerLiteral)) {
    Token tok = advance();
    return make_node<IntegerLiteralNode>(span_from(start), tok.literal);
  }

  if (check(Token::Kind::FloatLiteral)) {
    Token tok = advance();
    return make_node<FloatLiteralNode>(span_from(start), tok.literal);
  }

  error("expected integer or float literal, got " +
        std::string(token_kind_name(current.kind)));
  return make_node<IntegerLiteralNode>(span_from(start), std::string_view{});
}

// parse_bool_literal — BoolLiteral = "true" | "false"
//
// The lexer emits both keywords as a single BoolLiteral token whose literal
// is the raw source text ("true" or "false"). We consume it and wrap it in
// a BoolLiteralNode, preserving the raw text for later semantic passes.
//
// On mismatch, we report an error and return a BoolLiteralNode with an empty
// literal so callers always receive a non-null node.
NodePtr Parser::parse_bool_literal() {
  auto start = mark();

  if (check(Token::Kind::BoolLiteral)) {
    Token tok = advance();
    return make_node<BoolLiteralNode>(span_from(start), tok.literal);
  }

  error("expected boolean literal ('true' or 'false'), got " +
        std::string(token_kind_name(current.kind)));
  return make_node<BoolLiteralNode>(span_from(start), std::string_view{});
}

// parse_string_literal — StringLiteral = SingleLineString | MultiLineString
//
// Grammar:
//   StringContent = unicode_char_except_special | EscapeSequence |
//   Interpolation Interpolation = "{" Expression "}"
//
// The lexer splits strings into up to four token kinds so that interpolated
// expressions can be parsed recursively:
//
//   StringLiteral  — the whole string when there is no interpolation
//                    e.g.  "hello"   literal == "\"hello\""
//   StringStart    — opening fragment up to (and including) the first '{'
//                    e.g.  "hello {  literal == "\"hello {"
//   StringMiddle   — fragment between two interpolations, '}'..'{'
//                    e.g.  } and {   literal == "} and {"
//   StringEnd      — closing fragment from '}' to (and including) the '"'
//                    e.g.  }"        literal == "}\""
//
// The '{' that opens an interpolation is absorbed into the preceding fragment
// token; the '}' that closes it is absorbed into the following fragment token.
// This means the parser never sees raw '{' / '}' during string parsing — it
// simply calls parse_expression() between consecutive fragment tokens.
//
// AST shape produced for  "hi {name}, you are {age} years old":
//
//   StringLiteralNode
//     StringFragmentNode   "hi {
//     IdentifierNode       name
//     StringFragmentNode   }, you are {
//     IdentifierNode       age
//     StringFragmentNode   } years old"
//
// On any unexpected token, an error is reported and a best-effort
// StringLiteralNode is returned so callers always receive a non-null node.
NodePtr Parser::parse_string_literal() {
  auto start = mark();
  std::vector<NodePtr> fragments;

  // ── Plain string — no interpolation ──────────────────────────────────
  if (check(Token::Kind::StringLiteral)) {
    auto frag_start = mark();
    Token tok = advance();
    fragments.push_back(
        make_node<StringFragmentNode>(span_from(frag_start), tok.literal));
    return make_node<StringLiteralNode>(span_from(start), std::move(fragments));
  }

  // ── Interpolated string ───────────────────────────────────────────────
  // StringStart { Expression ( StringMiddle | StringEnd ) }
  if (check(Token::Kind::StringStart)) {
    // Opening fragment  ("...{)
    {
      auto frag_start = mark();
      Token tok = advance();
      fragments.push_back(
          make_node<StringFragmentNode>(span_from(frag_start), tok.literal));
    }

    while (!is_at_end()) {
      // Interpolated expression between the braces.
      fragments.push_back(parse_expression());

      if (check(Token::Kind::StringMiddle)) {
        // Middle fragment (}...{) — more interpolations follow.
        auto frag_start = mark();
        Token tok = advance();
        fragments.push_back(
            make_node<StringFragmentNode>(span_from(frag_start), tok.literal));
        // Loop back to parse the next expression.

      } else if (check(Token::Kind::StringEnd)) {
        // Closing fragment (}...") — string is complete.
        auto frag_start = mark();
        Token tok = advance();
        fragments.push_back(
            make_node<StringFragmentNode>(span_from(frag_start), tok.literal));
        break;

      } else {
        error("expected string fragment after interpolated expression, got " +
              std::string(token_kind_name(current.kind)));
        break;
      }
    }

    return make_node<StringLiteralNode>(span_from(start), std::move(fragments));
  }

  // ── Fallthrough — not a string token at all ───────────────────────────
  error("expected string literal, got " +
        std::string(token_kind_name(current.kind)));
  return make_node<StringLiteralNode>(span_from(start), std::move(fragments));
}

// ============================================================================
// Sub-expression Helpers (Group 1)
// ============================================================================

// parse_pipe — IdentifierPipe = "|" Identifier "|"
//
// Tries to consume the optional pipe notation used in or-clauses, for-loops,
// and spawn expressions:
//
//   expr or |err| { ... }
//   for x : items |acc| { ... }
//   spawn |task| { ... }
//
// Returns the captured IdentifierNode on success, or std::nullopt when the
// current token is not "|" (leaving the token stream unchanged so the caller
// can treat the pipe as absent).
std::optional<IdentifierNode> Parser::parse_pipe() {
  if (!check(Token::Kind::BitwiseOr))
    return std::nullopt;

  advance(); // consume opening "|"

  auto id_start = mark();
  Token id = expect(Token::Kind::Identifier);
  expect(Token::Kind::BitwiseOr); // consume closing "|"

  return IdentifierNode{span_from(id_start), id.literal};
}

// parse_generic — Generic = "|" TypeList "|"
//
// TypeList = SingleType { "," SingleType }
//
// Parses the optional type-parameter list that can appear on anonymous
// function expressions and spawn expressions:
//
//   fn |T| (x T) T { ... }
//   |String| spawn { ... }
//
// Note: parse_single_type() is used instead of parse_type() /
// parse_union_type() to avoid the ambiguity where a "|" inside the list would
// be greedily consumed as a union-type separator rather than the closing
// delimiter. Generic type parameters are always single (non-union) types in
// practice.
//
// Returns std::nullopt when the current token is not "|".
std::optional<GenericNode> Parser::parse_generic() {
  if (!check(Token::Kind::BitwiseOr))
    return std::nullopt;

  auto start = mark();
  advance(); // consume opening "|"

  std::vector<NodePtr> type_params;
  type_params.push_back(parse_single_type());

  while (check(Token::Kind::Comma)) {
    advance(); // consume ","
    type_params.push_back(parse_single_type());
  }

  expect(Token::Kind::BitwiseOr); // consume closing "|"

  return GenericNode{span_from(start), std::move(type_params)};
}

// parse_import_expr — ImportExpr = "import" StringLiteral
//
// Identical grammar to ImportDecl but used in expression position, producing
// an ImportExprNode rather than an ImportDeclNode.  The surrounding quotes are
// stripped from the path literal, matching parse_import_decl's convention.
NodePtr Parser::parse_import_expr() {
  auto start = mark();
  expect(Token::Kind::Import);

  Token path_tok = expect(Token::Kind::StringLiteral);

  std::string_view path = path_tok.literal;
  if (path.size() >= 2)
    path = path.substr(1, path.size() - 2);

  return make_node<ImportExprNode>(span_from(start), path);
}

// parse_group_or_range — "(" Expression ")" or "(" Expression ".." Expression
// ")"
//
// Grammar:
//   GroupExpr = "(" Expression ")"
//   RangeExpr = "(" Expression ".." Expression ")"
//
// Disambiguation: the first sub-expression is parsed with parse_expr_bp(25),
// a minimum binding power equal to ".."'s own left-bp (25).  This stops the
// Pratt loop before it would consume ".." as an infix operator, keeping it
// visible for an explicit check below.
//
// Consequence: operators with left-bp ≤ 25 ("or" at 20, ".." at 25) are not
// parsed as the outermost operator of a grouped expression.  In practice this
// is not a restriction — "or" requires a block body and is uncommon in grouped
// position; ".." as a raw binary operator belongs in index/slice context.
NodePtr Parser::parse_group_or_range() {
  auto start = mark();
  expect(Token::Kind::LeftParenthesis);
  skip_terminators();

  // Parse up to (but not including) ".." so we can distinguish the two forms.
  NodePtr first = parse_expr_bp(25);
  if (!first)
    return nullptr;

  skip_terminators();

  if (check(Token::Kind::DotDot)) {
    // ── RangeExpr = "(" Expression ".." Expression ")" ──────────────────
    advance(); // consume ".."
    skip_terminators();

    NodePtr second =
        parse_expr_bp(25); // symmetric: stop before any nested ".."
    if (!second)
      return nullptr;

    skip_terminators_before(Token::Kind::RightParenthesis);
    expect(Token::Kind::RightParenthesis);
    return make_node<RangeExprNode>(span_from(start), std::move(first),
                                    std::move(second));
  }

  // ── GroupExpr = "(" Expression ")" ──────────────────────────────────────
  skip_terminators_before(Token::Kind::RightParenthesis);
  expect(Token::Kind::RightParenthesis);
  return make_node<GroupExprNode>(span_from(start), std::move(first));
}

// parse_array_literal — ArrayLiteral = "[" [ Expression { "," Expression } ]
// "]"
//
// Elements are comma-separated.  Trailing commas and embedded newlines are
// both tolerated: newlines are skipped after "[", after each ",", and before
// the closing "]".
NodePtr Parser::parse_array_literal() {
  auto start = mark();
  expect(Token::Kind::LeftBracket);
  skip_terminators();

  std::vector<NodePtr> elements;

  while (!check(Token::Kind::RightBracket) && !is_at_end()) {
    elements.push_back(parse_expression());
    skip_terminators();
    if (!check(Token::Kind::Comma))
      break;
    advance(); // consume ","
    skip_terminators();
  }

  skip_terminators_before(Token::Kind::RightBracket);
  expect(Token::Kind::RightBracket);
  return make_node<ArrayLiteralNode>(span_from(start), std::move(elements));
}

// parse_map_or_block — disambiguate "{" in expression prefix position.
//
// MapLiteral   = "{" { KeyValuePair } "}"
// KeyValuePair = Expression ":" Expression
//
// Disambiguation after consuming "{" and skipping terminators:
//   "}"                      → empty map literal
//   Expression then ":"      → map literal (key-value pair follows)
//   anything else            → bare block (delegate to parse_block logic)
//
// The first expression is parsed with parse_expr_bp(1) so that "{" is not
// consumed as a struct-literal infix operator.  If ":" follows, the
// expression was a key and we continue as a map.  Otherwise, the expression
// was the first statement of a block and we fall through to block parsing
// with that expression already in hand.
NodePtr Parser::parse_map_or_block() {
  auto start = mark();
  expect(Token::Kind::LeftBrace);
  skip_terminators();

  if (check(Token::Kind::RightBrace)) {
    advance();
    return make_node<MapLiteralNode>(span_from(start),
                                     std::vector<KeyValueNode>{});
  }

  size_t pos_before = current.offset;
  NodePtr first = parse_expr_bp(1);
  if (!first)
    return nullptr;

  if (check(Token::Kind::Colon)) {
    advance();
    NodePtr first_value = parse_expression();

    std::vector<KeyValueNode> entries;
    Span kv_span{first->span.start, first_value->span.end};
    entries.push_back(
        KeyValueNode{kv_span, std::move(first), std::move(first_value)});

    skip_terminators();
    if (check(Token::Kind::Comma)) {
      advance();
      skip_terminators();
    }

    while (!check(Token::Kind::RightBrace) && !is_at_end()) {
      auto kv_start = mark();
      NodePtr key = parse_expr_bp(1);
      expect(Token::Kind::Colon);
      NodePtr value = parse_expression();
      entries.push_back(
          KeyValueNode{span_from(kv_start), std::move(key), std::move(value)});

      skip_terminators();
      if (check(Token::Kind::Comma)) {
        advance();
        skip_terminators();
      }
    }

    expect(Token::Kind::RightBrace);
    return make_node<MapLiteralNode>(span_from(start), std::move(entries));
  }

  // Block fallback: the first expression was already consumed. To properly
  // handle statements that start with an expression (assignments, increments,
  // VarDecl, etc.) we re-delegate to parse_block, but we need to "put back"
  // the first expression. Instead, we complete the first statement inline
  // using the same post-expression dispatch that parse_statement uses, then
  // parse the remaining statements normally.
  std::vector<NodePtr> stmts;

  auto first_start = first->span.start;

  if (check(Token::Kind::Increment)) {
    advance();
    stmts.push_back(
        make_node<IncrementNode>(span_from(first_start), std::move(first)));
  } else if (check(Token::Kind::Decrement)) {
    advance();
    stmts.push_back(
        make_node<DecrementNode>(span_from(first_start), std::move(first)));
  } else if (check(Token::Kind::DeclAssignment)) {
    auto *id_ptr = std::get_if<IdentifierNode>(&first->data);
    if (!id_ptr)
      error("':=' requires an identifier on the left-hand side");
    IdentifierListNode id_list{first->span,
                               id_ptr ? std::vector<IdentifierNode>{*id_ptr}
                                      : std::vector<IdentifierNode>{}};
    advance();
    NodePtr value = parse_expression();
    stmts.push_back(make_node<DeclAssignNode>(span_from(first_start),
                                              std::move(id_list),
                                              std::move(value)));
  } else if (is_assign_op(current.kind)) {
    stmts.push_back(parse_assignment(std::move(first)));
  } else if (auto *id_ptr = std::get_if<IdentifierNode>(&first->data);
             id_ptr && (current.kind == Token::Kind::Identifier ||
                        current.kind == Token::Kind::Fn ||
                        current.kind == Token::Kind::Struct)) {
    IdentifierNode name = *id_ptr;
    NodePtr type = parse_type();
    std::optional<NodePtr> init;
    if (check(Token::Kind::Assignment)) {
      advance();
      init = parse_expression();
    }
    stmts.push_back(make_node<VarDeclNode>(span_from(first_start), name,
                                           std::make_optional(std::move(type)),
                                           std::move(init)));
  } else {
    stmts.push_back(std::move(first));
  }

  skip_terminators();

  while (!check(Token::Kind::RightBrace) && !is_at_end()) {
    size_t stmt_pos = current.offset;
    NodePtr stmt = parse_statement();
    if (stmt) {
      stmts.push_back(std::move(stmt));
    } else if (current.offset == stmt_pos) {
      advance();
    }
    skip_terminators();
  }

  expect(Token::Kind::RightBrace);
  return make_node<BlockNode>(span_from(start), std::move(stmts));
}

// ============================================================================
// Statement Helpers — partial (parse_statement / parse_block in Group 2)
// ============================================================================

// parse_return — ReturnStatement = "return" [ ExpressionList ]
//
// A bare "return" with no following expression is valid.  Whether a value is
// present is determined by is_expression_start(): if the current token cannot
// begin an expression it must be a terminator, "}", or EOF, all of which
// signal a value-less return.  Multiple comma-separated values are collected
// into the values vector (multi-return).
NodePtr Parser::parse_return() {
  auto start = mark();
  expect(Token::Kind::Return);

  std::vector<NodePtr> values;
  if (is_expression_start(current.kind)) {
    values.push_back(parse_expression());
    while (check(Token::Kind::Comma)) {
      advance(); // consume ","
      values.push_back(parse_expression());
    }
  }

  return make_node<ReturnNode>(span_from(start), std::move(values));
}

// parse_break — break_statement = "break" [ ExpressionList ]
//
// Mirrors parse_return exactly.  The optional value is passed out of the
// nearest enclosing `for` loop as its result.
NodePtr Parser::parse_break() {
  auto start = mark();
  expect(Token::Kind::Break);

  std::vector<NodePtr> values;
  if (is_expression_start(current.kind)) {
    values.push_back(parse_expression());
    while (check(Token::Kind::Comma)) {
      advance(); // consume ","
      values.push_back(parse_expression());
    }
  }

  return make_node<BreakNode>(span_from(start), std::move(values));
}

// parse_next — next_statement = "next"
//
// Unconditional continue.  No sub-expression; the node carries only its span.
NodePtr Parser::parse_next() {
  auto start = mark();
  expect(Token::Kind::Next);
  return make_node<NextNode>(span_from(start));
}

// ============================================================================
// Block and Statement Parsing (Group 2)
// ============================================================================

// parse_var_decl — VarDecl = Identifier Type [ "=" Expression ]
//
// Standalone entry point: called when `current` is the identifier that names
// the variable.  Consumes the identifier, parses the required type annotation,
// and optionally parses an initialiser after "=".
//
// Called from parse_statement() after the leading identifier has already been
// returned from parse_expression() as an IdentifierNode — in that path, the
// VarDecl is assembled inline (see parse_statement).  parse_var_decl() itself
// is the canonical implementation for any future call-site that arrives at a
// known VarDecl position with `current` == Identifier.
NodePtr Parser::parse_var_decl() {
  auto start = mark();

  auto name_start = mark();
  Token id = expect(Token::Kind::Identifier);
  IdentifierNode name{span_from(name_start), id.literal};

  NodePtr type = parse_type();

  std::optional<NodePtr> init;
  if (check(Token::Kind::Assignment)) {
    advance(); // consume "="
    init = parse_expression();
  }

  return make_node<VarDeclNode>(span_from(start), name,
                                std::make_optional(std::move(type)),
                                std::move(init));
}

// parse_decl_assign — DeclAssign = IdentifierList ":=" ExpressionList
//
// Standalone entry point: called when `current` is the first identifier of
// the target list.  Collects identifiers until ":=", then parses a single
// RHS expression.
//
// Note: the AST's DeclAssignNode stores a single `NodePtr value`.  The
// grammar's ExpressionList on the RHS is used for multi-return unpacking
// (e.g. `a, b := twoValueFn()`), where the single RHS expression is a call
// that returns two values.  Literal multi-value RHS (`a, b := 1, 2`) parses
// only the first value; full support requires a TupleNode that does not yet
// exist in the AST.
NodePtr Parser::parse_decl_assign() {
  auto start = mark();

  // ── IdentifierList ────────────────────────────────────────────────────
  auto id_list_start = mark();
  std::vector<IdentifierNode> names;

  {
    auto id_start = mark();
    Token id = expect(Token::Kind::Identifier);
    names.push_back({span_from(id_start), id.literal});
  }

  while (check(Token::Kind::Comma)) {
    advance(); // consume ","
    auto id_start = mark();
    Token id = expect(Token::Kind::Identifier);
    names.push_back({span_from(id_start), id.literal});
  }

  IdentifierListNode id_list{span_from(id_list_start), std::move(names)};

  expect(Token::Kind::DeclAssignment); // ":="

  NodePtr value = parse_expression();

  return make_node<DeclAssignNode>(span_from(start), std::move(id_list),
                                   std::move(value));
}

// parse_assignment — Assignment = target assignment_operator ExpressionList
//
// Called after the LHS target has already been parsed and passed in.
// Consumes the assignment operator (=  +=  -=  *=  /=) and one or more
// comma-separated RHS expressions.
//
// Single-target: the pre-parsed `target` is wrapped in the targets vector.
// For multi-target (a, b = ...) the caller in parse_statement assembles a
// targets vector itself and does not call this function.
NodePtr Parser::parse_assignment(NodePtr target) {
  size_t start = target->span.start;
  Token::Kind op = current.kind;
  advance(); // consume the assignment operator

  std::vector<NodePtr> targets;
  targets.push_back(std::move(target));

  std::vector<NodePtr> values;
  values.push_back(parse_expression());
  while (check(Token::Kind::Comma)) {
    advance(); // consume ","
    values.push_back(parse_expression());
  }

  return make_node<AssignNode>(span_from(start), std::move(targets), op,
                               std::move(values));
}

// parse_statement — Statement = Assignment | IncrementStatement
//                             | DecrementStatement | VarDecl
//                             | ReturnStatement | break_statement
//                             | next_statement
//
// Also handles DeclAssign (":=") even though the grammar lists it under
// Assignment, because it produces a distinct AST node.
//
// Strategy
// ────────
// 1. Keyword-led forms (return / break / next) are dispatched immediately.
//
// 2. Everything else starts by parsing a leading expression.  Because none
//    of the statement-specific operators appear in infix_binding_power
//    (assignment ops, "++", "--", "," all return 0), the Pratt loop always
//    stops before them, leaving them in `current` for the dispatch below.
//
// 3. Post-expression dispatch (in priority order):
//      ++  / --                → Increment / Decrement
//      ","  then ":=" or op   → multi-target DeclAssign / Assignment
//      ":="                   → single-target DeclAssign
//      assignment op          → single-target Assignment (via parse_assignment)
//      IdentifierNode + type  → VarDecl (assembled inline)
//      anything else          → expression statement (returned as-is)
//
// VarDecl detection: only Identifier / "fn" / "struct" as the following
// token are treated as unambiguous type-starts.  "[" and "(" are excluded
// because they also serve as high-precedence infix operators (index, call),
// making the disambiguation impossible without semantic information.
NodePtr Parser::parse_statement() {

  // ── 1. Keyword-led statements ───────────────────────────────────────────
  switch (current.kind) {
  case Token::Kind::Return:
    return parse_return();
  case Token::Kind::Break:
    return parse_break();
  case Token::Kind::Next:
    return parse_next();
  default:
    break;
  }

  // ── 2. Parse the leading expression ────────────────────────────────────
  auto start = mark();
  NodePtr expr = parse_expression();
  if (!expr)
    return nullptr;

  // ── 3a. Increment / Decrement ───────────────────────────────────────────
  if (check(Token::Kind::Increment)) {
    advance();
    return make_node<IncrementNode>(span_from(start), std::move(expr));
  }
  if (check(Token::Kind::Decrement)) {
    advance();
    return make_node<DecrementNode>(span_from(start), std::move(expr));
  }

  // ── 3b. Multi-target: "a , b := …" or "a , b = …" ─────────────────────
  if (check(Token::Kind::Comma)) {
    std::vector<NodePtr> targets;
    targets.push_back(std::move(expr));

    while (check(Token::Kind::Comma)) {
      advance(); // consume ","
      targets.push_back(parse_expression());
    }

    if (check(Token::Kind::DeclAssignment)) {
      // Build IdentifierListNode before consuming ":="
      std::vector<IdentifierNode> names;
      Span id_list_span{targets.front()->span.start, targets.back()->span.end};
      for (auto &t : targets) {
        if (auto *id = std::get_if<IdentifierNode>(&t->data))
          names.push_back(*id);
        else
          error_at(t->span, "expected identifier in ':=' target list");
      }
      IdentifierListNode id_list{id_list_span, std::move(names)};
      advance(); // consume ":="
      NodePtr value = parse_expression();
      return make_node<DeclAssignNode>(span_from(start), std::move(id_list),
                                       std::move(value));
    }

    if (is_assign_op(current.kind)) {
      Token::Kind op = current.kind;
      advance(); // consume assignment operator
      std::vector<NodePtr> values;
      values.push_back(parse_expression());
      while (check(Token::Kind::Comma)) {
        advance();
        values.push_back(parse_expression());
      }
      return make_node<AssignNode>(span_from(start), std::move(targets), op,
                                   std::move(values));
    }

    error("expected ':=' or assignment operator after target list");
    return nullptr;
  }

  // ── 3c. Single-target DeclAssign: "expr := value" ──────────────────────
  if (check(Token::Kind::DeclAssignment)) {
    auto *id_ptr = std::get_if<IdentifierNode>(&expr->data);
    if (!id_ptr) {
      error("':=' requires an identifier on the left-hand side");
      advance(); // consume ":=" for error recovery
      return nullptr;
    }
    IdentifierListNode id_list{expr->span, {*id_ptr}};
    advance(); // consume ":="
    NodePtr value = parse_expression();
    return make_node<DeclAssignNode>(span_from(start), std::move(id_list),
                                     std::move(value));
  }

  // ── 3d. Single-target Assignment: "expr = …", "expr += …", etc. ────────
  if (is_assign_op(current.kind))
    return parse_assignment(std::move(expr));

  // ── 3e. VarDecl: IdentifierNode followed by unambiguous type-start ──────
  if (auto *id_ptr = std::get_if<IdentifierNode>(&expr->data)) {
    if (current.kind == Token::Kind::Identifier ||
        current.kind == Token::Kind::Fn ||
        current.kind == Token::Kind::Struct) {
      IdentifierNode name = *id_ptr;
      NodePtr type = parse_type();
      std::optional<NodePtr> init;
      if (check(Token::Kind::Assignment)) {
        advance(); // consume "="
        init = parse_expression();
      }
      return make_node<VarDeclNode>(span_from(start), name,
                                    std::make_optional(std::move(type)),
                                    std::move(init));
    }
  }

  // ── 3f. Expression statement ────────────────────────────────────────────
  return expr;
}

// parse_block — Block = "{" { (Expression | Statement) [terminal] } "}"
//
// Statements and expressions appear interchangeably inside a block; both
// are handled by parse_statement(), which returns expression nodes directly
// when no statement-specific operator follows the expression.
//
// Each item is optionally followed by one or more Terminator tokens (newlines).
// The loop uses the same forward-progress guard as parse_source(): if
// parse_statement() returns nullptr without advancing the token position,
// a single token is force-consumed to prevent an infinite loop.
NodePtr Parser::parse_block() {
  auto start = mark();
  expect(Token::Kind::LeftBrace);
  skip_terminators();

  std::vector<NodePtr> stmts;

  while (!check(Token::Kind::RightBrace) && !is_at_end()) {
    size_t pos_before = current.offset;

    NodePtr stmt = parse_statement();

    if (stmt) {
      stmts.push_back(std::move(stmt));
    } else if (current.offset == pos_before) {
      advance(); // force progress on error to prevent an infinite loop
    }

    skip_terminators();
  }

  expect(Token::Kind::RightBrace);
  return make_node<BlockNode>(span_from(start), std::move(stmts));
}

// ============================================================================
// Compound Expression Parsing (Group 3)
// ============================================================================

// parse_if_expr — IfExpr = "if" Expression Block [ "else" Block ]
//
// The condition expression always stops before "{" because LeftBrace has
// infix_binding_power == 0.  skip_terminators() before each block call
// handles both same-line style ("if x {") and next-line style ("if x\n{").
//
// "else if" chains are NOT valid in this language — after "else" only a
// plain Block is allowed.  Seeing "if" after "else" is a parse error.
NodePtr Parser::parse_if_expr() {
  auto start = mark();
  expect(Token::Kind::If);

  // ── Condition ────────────────────────────────────────────────────────────
  // parse_expr_bp(1) stops before "{" (infix_bp == 1), preventing the
  // opening brace of the then-block from being consumed as a struct literal.
  NodePtr condition = parse_expr_bp(1);
  if (!condition)
    return nullptr;

  // ── Then block ───────────────────────────────────────────────────────────
  skip_terminators();
  NodePtr then_block = parse_block();

  // ── Optional else ────────────────────────────────────────────────────────
  std::optional<NodePtr> else_block;

  skip_terminators();
  if (check(Token::Kind::Else)) {
    advance(); // consume "else"
    skip_terminators();
    else_block = parse_block();
  }

  return make_node<IfExprNode>(span_from(start), std::move(condition),
                               std::move(then_block), std::move(else_block));
}

// parse_case_arm — CaseArm = "case" Expression ":" ( Expression | Block )
//
// Body disambiguation: if the token after ":" (with terminators skipped) is
// "{" the body is a Block; otherwise it is an Expression.  The expression
// body stops naturally before the next "case", "else", or "}" because none
// of those tokens have a non-zero infix_binding_power.
CaseArmNode Parser::parse_case_arm() {
  auto start = mark();

  expect(Token::Kind::Case);

  // parse_expr_bp(1): stop before "{" so the body block is not mistaken
  // for a struct literal opened by the pattern expression.
  NodePtr pattern = parse_expr_bp(1);

  expect(Token::Kind::Colon);
  skip_terminators();

  NodePtr body =
      check(Token::Kind::LeftBrace) ? parse_block() : parse_expression();

  return CaseArmNode{span_from(start), std::move(pattern), std::move(body)};
}

// parse_switch_expr — SwitchExpr = "switch" Expression SwitchBlock
//                     SwitchBlock = "{" CaseArm { CaseArm } [ ElseArm ] "}"
//                     ElseArm     = "else" ":" ( Expression | Block )
//
// The subject expression stops before "{" (infix_bp == 0); skip_terminators
// handles newlines between the subject and the opening brace.
//
// Inside SwitchBlock, case arms are separated by terminators.  The optional
// ElseArm uses "else" ":" (two tokens) to distinguish it from the "else"
// that begins an else-block in IfExpr.
//
// Body disambiguation for ElseArm: same rule as CaseArm — "{" means Block,
// anything else means Expression.
NodePtr Parser::parse_switch_expr() {
  auto start = mark();
  expect(Token::Kind::Switch);

  // ── Subject expression ────────────────────────────────────────────────────
  // parse_expr_bp(1): stop before "{" so the switch block is not mistaken
  // for a struct literal opened by the subject expression.
  NodePtr subject = parse_expr_bp(1);
  if (!subject)
    return nullptr;

  // ── Switch block ──────────────────────────────────────────────────────────
  skip_terminators();
  expect(Token::Kind::LeftBrace);
  skip_terminators();

  std::vector<CaseArmNode> arms;
  std::optional<NodePtr> else_body;

  // ── Case arms ─────────────────────────────────────────────────────────────
  while (check(Token::Kind::Case)) {
    arms.push_back(parse_case_arm());
    skip_terminators();
  }

  // ── Optional else arm ─────────────────────────────────────────────────────
  if (check(Token::Kind::Else)) {
    advance(); // consume "else"
    expect(Token::Kind::Colon);
    skip_terminators();

    else_body =
        check(Token::Kind::LeftBrace) ? parse_block() : parse_expression();

    skip_terminators();
  }

  expect(Token::Kind::RightBrace);

  return make_node<SwitchExprNode>(span_from(start), std::move(subject),
                                   std::move(arms), std::move(else_body));
}

// ============================================================================
// For Expression Parsing
// ============================================================================

// parse_for_expr — ForExpr = "for" [ ForMode ] [ IdentifierPipe ] Block
//
//   ForMode        = Expression | IteratorClause | RangeClause
//   IteratorClause = Identifier ":=" Expression ";" Expression ";" Assignment
//   RangeClause    = Identifier { "," Identifier } ":" Expression
//
// Disambiguation (no peek, no backtracking)
// ─────────────────────────────────────────
// After "for", if the next token is "{" or "|", there is no ForMode.
// Otherwise, parse_expression() is called for the leading expression —
// safe because all clause-specific tokens have infix_binding_power == 0
// and will not be consumed by the Pratt loop:
//
//   ":="  → Pratt loop stops → leading is the init identifier (IteratorClause)
//   ","   → Pratt loop stops → leading is the first range variable
//   (RangeClause)
//   ":"   → Pratt loop stops → leading is the sole range variable (RangeClause)
//   "{"   → Pratt loop stops → leading is a bare condition expression
//   "|"   → Pratt loop stops → leading is a bare condition expression
//
// IteratorClause init
// ───────────────────
// The init sub-clause is assembled as a DeclAssignNode (the grammar only
// allows ":=", not plain "=", for the init).  The update sub-clause is
// parsed via parse_statement() so that x++, x--, and x op= expr all work.
//
// Accumulator pipe
// ────────────────
// After the ForMode (if any), an optional "|identifier|" pipe is consumed
// by parse_pipe().  skip_terminators() before the call allows the pipe to
// appear on the same line or the next line.
NodePtr Parser::parse_for_expr() {
  auto start = mark();
  expect(Token::Kind::For);

  std::optional<NodePtr> mode;
  std::optional<IdentifierNode> accumulator;

  // ── ForMode ──────────────────────────────────────────────────────────────
  // Skip mode parsing if the next token already starts the pipe or block.
  if (!check(Token::Kind::LeftBrace) && !check(Token::Kind::BitwiseOr)) {
    auto mode_start = mark();
    // parse_expr_bp(1): stop before "{" — the for-body block must not be
    // consumed as a struct literal by the leading expression.
    NodePtr leading = parse_expr_bp(1);

    if (check(Token::Kind::DeclAssignment)) {
      // ── IteratorClause ─────────────────────────────────────────────────
      // Identifier ":=" Expression ";" Expression ";" Assignment
      auto *id_ptr = std::get_if<IdentifierNode>(&leading->data);
      if (!id_ptr)
        error_at(leading->span,
                 "for-iterator init requires an identifier before ':='");

      IdentifierListNode id_list{leading->span,
                                 id_ptr ? std::vector<IdentifierNode>{*id_ptr}
                                        : std::vector<IdentifierNode>{}};

      advance(); // consume ":="
      NodePtr init_val = parse_expression();
      NodePtr init = make_node<DeclAssignNode>(
          span_from(mode_start), std::move(id_list), std::move(init_val));
      expect(Token::Kind::Semicolon);
      NodePtr condition = parse_expression();
      expect(Token::Kind::Semicolon);

      // ── Iterator update ───────────────────────────────────────────────
      // Cannot call parse_statement() here: that calls parse_expression()
      // for assignment RHS values, which — with LeftBrace at infix_bp 1 —
      // would consume the for-body "{" as a struct literal (e.g. "i += 2 {").
      // Instead, parse the update inline using parse_expr_bp(1) throughout.
      NodePtr update;
      {
        auto upd_start = mark();
        NodePtr upd_lhs = parse_expr_bp(1);

        if (check(Token::Kind::Increment)) {
          advance();
          update = make_node<IncrementNode>(span_from(upd_start),
                                            std::move(upd_lhs));
        } else if (check(Token::Kind::Decrement)) {
          advance();
          update = make_node<DecrementNode>(span_from(upd_start),
                                            std::move(upd_lhs));
        } else if (is_assign_op(current.kind)) {
          Token::Kind upd_op = current.kind;
          advance(); // consume assignment operator
          std::vector<NodePtr> upd_targets, upd_values;
          upd_targets.push_back(std::move(upd_lhs));
          upd_values.push_back(parse_expr_bp(1)); // RHS also stops before "{"
          update = make_node<AssignNode>(span_from(upd_start),
                                         std::move(upd_targets), upd_op,
                                         std::move(upd_values));
        } else {
          error("expected '++', '--', or assignment operator in for-iterator "
                "update clause");
          update = std::move(upd_lhs);
        }
      }

      mode =
          make_node<ForIterClauseNode>(span_from(mode_start), std::move(init),
                                       std::move(condition), std::move(update));

    } else if (check(Token::Kind::Comma) || check(Token::Kind::Colon)) {
      // ── RangeClause ────────────────────────────────────────────────────
      // Identifier { "," Identifier } ":" Expression
      auto *id_ptr = std::get_if<IdentifierNode>(&leading->data);
      if (!id_ptr)
        error_at(leading->span,
                 "for-range clause requires an identifier before ':'");

      std::vector<IdentifierNode> vars;
      if (id_ptr)
        vars.push_back(*id_ptr);

      while (check(Token::Kind::Comma)) {
        advance(); // consume ","
        auto id_start = mark();
        Token id = expect(Token::Kind::Identifier);
        vars.push_back({span_from(id_start), id.literal});
      }

      expect(Token::Kind::Colon);
      // parse_expr_bp(60): stop before "{" (bp 1) AND before "|" (bp 60).
      // BitwiseOr would otherwise consume the accumulator pipe "|acc|" as
      // an infix operator.  Bitwise operations on iterables are not
      // meaningful; parenthesise if ever needed: `for i : (a | b) |acc|`.
      NodePtr iterable = parse_expr_bp(60);

      mode = make_node<ForRangeClauseNode>(
          span_from(mode_start), std::move(vars), std::move(iterable));

    } else {
      // ── Bare condition expression (while-style loop) ────────────────────
      mode = std::move(leading);
    }
  }

  // ── Optional accumulator pipe: "|" Identifier "|" ──────────────────────
  skip_terminators();
  accumulator = parse_pipe();

  // ── Body block ───────────────────────────────────────────────────────────
  skip_terminators();
  NodePtr body = parse_block();

  return make_node<ForExprNode>(span_from(start), std::move(mode),
                                std::move(accumulator), std::move(body));
}

// ============================================================================
// Function Expression Parsing
// ============================================================================

// parse_func_expr — FuncExpr = "fn" [ Generic ] Signature Block
//
// The three sub-parsers are already fully implemented:
//   parse_generic()    — optional "|" TypeList "|"
//   parse_signature()  — "(" [ ParameterList ] ")" TypeList
//   parse_block()      — "{" { Statement | Expression } "}"
//
// Generic: present only for anonymous generic functions like
//   fn |T| (x T) T { x }
// The generic type-parameter list uses the same "|…|" delimiter as the
// accumulator pipe, but the position (immediately after "fn", before "(")
// is unambiguous.
//
// skip_terminators() before parse_block() allows the opening brace to be
// on the next line after the signature:
//   fn(x Int) Int
//   { x }
NodePtr Parser::parse_func_expr() {
  auto start = mark();
  expect(Token::Kind::Fn);

  std::optional<GenericNode> generic = parse_generic();
  SignatureNode sig = parse_signature();

  skip_terminators();
  NodePtr body = parse_block();

  return make_node<FuncExprNode>(span_from(start), std::move(generic),
                                 std::move(sig), std::move(body));
}

// ============================================================================
// Spawn Expression Parsing
// ============================================================================

// parse_spawn_expr — SpawnExpr = [ Generic ] "spawn" [ IdentifierPipe ] ( Block
// | Identifier )
//
// The generic type parameter is optional and, uniquely among all compound
// expressions, comes BEFORE the keyword:
//
//   spawn { work() }          — bare spawn, block body
//   spawn workerFn            — bare spawn, identifier body (detached call)
//   |String| spawn { work() } — typed channel spawn, block body
//   spawn |task| { task() }   — spawn with named task pipe, block body
//   |String| spawn |task| { task() } — both generic and pipe
//
// Dispatch:
//   Token::Kind::Spawn     → called directly (no generic)
//   Token::Kind::BitwiseOr → called for "|T| spawn …" (generic present)
//   Both routes call parse_generic() first; it returns nullopt when
//   current is not "|", so the non-generic path is handled transparently.
//
// Body disambiguation: "{" → Block; anything else → Identifier.
// Only these two forms appear in the grammar; a general expression body
// is not supported (unlike if/for/switch which accept full expressions).
NodePtr Parser::parse_spawn_expr() {
  auto start = mark();

  // Optional generic: "|" TypeList "|"  (present only for typed channels)
  std::optional<GenericNode> generic = parse_generic();

  expect(Token::Kind::Spawn);

  // Optional named-task pipe: "|" Identifier "|"
  std::optional<IdentifierNode> pipe = parse_pipe();

  skip_terminators();

  // Body: Block or bare Identifier
  NodePtr body =
      check(Token::Kind::LeftBrace) ? parse_block() : parse_identifier();

  return make_node<SpawnExprNode>(span_from(start), std::move(generic),
                                  std::move(pipe), std::move(body));
}

// ============================================================================
// Struct Literal Parsing
// ============================================================================

// parse_struct_literal — StructLiteral = TypeExpr StructInitializer
//                        StructInitializer = "{" [ FieldAssignment
//                                                  { ("," | terminal)
//                                                    FieldAssignment } ] "}"
//                        FieldAssignment = Identifier ":" Expression
//
// Called from parse_infix when the current token is "{" and the LHS is the
// type expression (an IdentifierNode, SelectorNode, or StructTypeNode).
//
// Field separators
// ────────────────
// The grammar allows either a comma or a line terminator between fields,
// so both of these are legal:
//   Point { x: 0, y: 0 }       -- comma-separated
//   Point {                     -- newline-separated
//     x: 0
//     y: 0
//   }
//   Point { x: 0, y: 0, }      -- trailing comma OK
//
// Field value expressions
// ───────────────────────
// parse_expression() (min_bp = 0) is used for field values, allowing nested
// struct literals:
//   Line { start: Point { x: 0, y: 0 }, end: Point { x: 1, y: 1 } }
// The closing "}" of an inner struct literal has infix_bp == 0, so the
// Pratt loop stops before it, cleanly returning control to the outer loop.
NodePtr Parser::parse_struct_literal(NodePtr type_expr) {
  size_t start = type_expr->span.start;

  expect(Token::Kind::LeftBrace);
  skip_terminators();

  std::vector<FieldAssignmentNode> fields;

  while (!check(Token::Kind::RightBrace) && !is_at_end()) {
    auto field_start = mark();

    // ── Field name ──────────────────────────────────────────────────────
    auto id_start = mark();
    Token id = expect(Token::Kind::Identifier);
    IdentifierNode name{span_from(id_start), id.literal};

    expect(Token::Kind::Colon);

    // ── Field value ─────────────────────────────────────────────────────
    // Shorthand: `host:,` or `host:\n` means use the local variable
    // with the same name as the field, equivalent to `host: host`.
    NodePtr value;
    if (check(Token::Kind::Comma) || check(Token::Kind::RightBrace) ||
        check(Token::Kind::Terminator)) {
      value = make_node<IdentifierNode>(span_from(id_start), id.literal);
    } else {
      // parse_expression() allows nested struct literals as values.
      value = parse_expression();
    }

    fields.push_back(FieldAssignmentNode{span_from(field_start),
                                         std::move(name), std::move(value)});

    // ── Separator: newline(s) and/or optional comma ──────────────────────
    skip_terminators();
    if (check(Token::Kind::Comma)) {
      advance();
      skip_terminators();
    }
  }

  expect(Token::Kind::RightBrace);

  return make_node<StructLiteralNode>(span_from(start), std::move(type_expr),
                                      std::move(fields));
}

} // namespace mc
