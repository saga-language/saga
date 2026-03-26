// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#pragma once

#include "span.hpp"
#include "token.hpp"

#include <iosfwd>
#include <memory>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

namespace mc {

// ---------------------------------------------------------------------------
// Forward declaration — all node types are owned via NodePtr.
// ---------------------------------------------------------------------------

struct Node;
using NodePtr = std::unique_ptr<Node>;

// ---------------------------------------------------------------------------
// Visitor helper — use with std::visit to dispatch over Node::Variant.
//
//   std::visit(overloaded{
//     [](const IdentifierNode& n) { ... },
//     [](const auto&)             { ... }, // catch-all
//   }, node.data);
// ---------------------------------------------------------------------------

template <typename... Ts> struct overloaded : Ts... {
  using Ts::operator()...;
};
template <typename... Ts> overloaded(Ts...) -> overloaded<Ts...>;

// ===========================================================================
// Section 1: Leaf / atom nodes
//
// These have no by-value struct dependencies other than Span and string_view.
// They must be defined first because later structs embed them by value.
// ===========================================================================

// Identifier = letter { letter | decimal_digit } [ "?" ]
struct IdentifierNode {
  Span span;
  std::string_view name;
};

// IdentifierList = Identifier { "," Identifier }
struct IdentifierListNode {
  Span span;
  std::vector<IdentifierNode> identifiers;
};

// ---------------------------------------------------------------------------
// Literal nodes — raw source text is preserved; semantic evaluation is a
// later pass. No exceptions, no from_chars here.
// ---------------------------------------------------------------------------

// BoolLiteral = "true" | "false"
struct BoolLiteralNode {
  Span span;
  std::string_view literal; // "true" or "false"
};

// IntegerLiteral = decimal | "0b" binary | "0x" hex | "0o" octal
struct IntegerLiteralNode {
  Span span;
  std::string_view literal; // e.g. "42", "0b1010", "0xdead_beef"
};

// FloatLiteral = digits "." digits [ Exponent ]
struct FloatLiteralNode {
  Span span;
  std::string_view literal; // e.g. "3.14", "1.5e+10"
};

// A raw text fragment inside a string literal (between interpolations, or the
// whole string if there is no interpolation). Escape sequences are stored
// verbatim; interpretation is deferred to a semantic pass.
struct StringFragmentNode {
  Span span;
  std::string_view text;
};

// StringLiteral = fragments of StringFragmentNode and interpolated expressions.
// A plain string with no interpolation has a single StringFragmentNode child.
struct StringLiteralNode {
  Span span;
  std::vector<NodePtr> fragments; // StringFragmentNode | any expression node
};

// ArrayLiteral = "[" [ Expression { "," Expression } ] "]"
struct ArrayLiteralNode {
  Span span;
  std::vector<NodePtr> elements;
};

// KeyValuePair = Expression ":" Expression  (one entry of a map literal)
struct KeyValueNode {
  Span span;
  NodePtr key;
  NodePtr value;
};

// MapLiteral = "{" { KeyValuePair } "}"
struct MapLiteralNode {
  Span span;
  std::vector<KeyValueNode> entries;
};

// FieldAssignment = Identifier ":" Expression  (inside a struct literal)
struct FieldAssignmentNode {
  Span span;
  IdentifierNode name;
  NodePtr value;
};

// StructLiteral = ( Identifier | Selector | StructType ) StructInitializer
struct StructLiteralNode {
  Span span;
  NodePtr type_expr; // IdentifierNode, SelectorNode, or StructTypeNode
  std::vector<FieldAssignmentNode> fields;
};

// ===========================================================================
// Section 2: Type nodes
//
// FieldSpecNode depends on IdentifierListNode (defined above).
// StructTypeNode depends on FieldSpecNode.
// ===========================================================================

// UnionType = SingleType { "|" SingleType }   (2 or more alternatives)
struct UnionTypeNode {
  Span span;
  std::vector<NodePtr> types;
};

// ArrayType = "[" Type "]"
struct ArrayTypeNode {
  Span span;
  NodePtr element_type;
};

// MapType = "{" Type ":" Type "}"
struct MapTypeNode {
  Span span;
  NodePtr key_type;
  NodePtr value_type;
};

// FuncType = "fn" Signature  (a function type expression)
struct FuncTypeNode {
  Span span;
  std::vector<NodePtr> params; // type nodes only (no names at type level)
  std::vector<NodePtr> returns;
};

// RangeType = "(" Type ")"
struct RangeTypeNode {
  Span span;
  NodePtr element_type;
};

// FieldSpec = IdentifierList Type  (one field declaration in a struct)
struct FieldSpecNode {
  Span span;
  IdentifierListNode names;
  NodePtr type;
};

// StructType = "struct" "{" [ FieldSpec { "," FieldSpec } ] "}"
struct StructTypeNode {
  Span span;
  std::vector<FieldSpecNode> fields;
};

// Generic = "|" TypeList "|"   e.g. |T|, |T, U|
struct GenericNode {
  Span span;
  std::vector<NodePtr> type_params; // IdentifierNode for each type variable
};

// ===========================================================================
// Section 3: Shared sub-nodes stored by value inside expression / declaration
// nodes.  Must be fully defined before any struct that embeds them directly.
// ===========================================================================

// CaseArm = "case" Expression ":" ( Expression | Block )
// Stored by value in std::vector<CaseArmNode> inside SwitchExprNode.
struct CaseArmNode {
  Span span;
  NodePtr pattern; // expression (value match) or type node (type match)
  NodePtr body;    // expression or BlockNode
};

// ParameterType = Type | VariadicType ("..." Type)
// Stored by value in std::vector<ParameterNode> inside SignatureNode.
struct ParameterNode {
  Span span;
  IdentifierListNode names;
  NodePtr type;
  bool is_variadic; // true for "...Type"
};

// Signature = "(" [ ParameterList ] ")" TypeList
// Stored by value inside FuncExprNode, FuncDeclNode, InterfaceFieldNode.
struct SignatureNode {
  Span span;
  std::vector<ParameterNode> params;
  std::vector<NodePtr> returns; // type nodes; empty means Void
};

// ===========================================================================
// Section 4: Expression nodes
// ===========================================================================

// BinaryExpr = Expression BinaryOperator PrimaryExpr
struct BinaryExprNode {
  Span span;
  NodePtr lhs;
  Token::Kind op;
  NodePtr rhs;
};

// UnaryExpr = unary_operator PrimaryExpr   (! or -)
struct UnaryExprNode {
  Span span;
  Token::Kind op; // Token::Kind::Not or Token::Kind::Sub
  NodePtr operand;
};

// "(" Expression ")"
struct GroupExprNode {
  Span span;
  NodePtr inner;
};

// CallExpr = PrimaryExpr "(" [ ExpressionList ] ")"
struct CallExprNode {
  Span span;
  NodePtr callee;
  std::vector<NodePtr> args;
};

// Slice = [ Expression ] ".." [ Expression ]
struct SliceNode {
  Span span;
  std::optional<NodePtr> low;  // absent = from beginning
  std::optional<NodePtr> high; // absent = to end
};

// IndexExpr = Selector "[" ( Expression | Slice ) "]"
struct IndexExprNode {
  Span span;
  NodePtr object;
  NodePtr index; // expression node or SliceNode
};

// Selector = ( Identifier | PrimaryExpr ) "." Identifier
struct SelectorNode {
  Span span;
  NodePtr object;
  IdentifierNode field;
};

// IfExpr = "if" Expression Block [ "else" Block ]
struct IfExprNode {
  Span span;
  NodePtr condition;
  NodePtr then_block;                // BlockNode
  std::optional<NodePtr> else_block; // BlockNode only — "else if" is not valid
};

// SwitchExpr = "switch" Expression SwitchBlock
struct SwitchExprNode {
  Span span;
  NodePtr subject;
  std::vector<CaseArmNode> arms;
  std::optional<NodePtr> else_body; // expression or BlockNode
};

// ForMode: range iteration  —  Identifier { "," Identifier } ":" Expression
struct ForRangeClauseNode {
  Span span;
  std::vector<IdentifierNode>
      vars; // one (value) or two (key, value) identifiers
  NodePtr iterable;
};

// ForMode: C-style iterator  —  init ";" condition ";" update
struct ForIterClauseNode {
  Span span;
  NodePtr init;      // VarDeclNode or DeclAssignNode
  NodePtr condition; // boolean expression
  NodePtr update;    // AssignNode, IncrementNode, or DecrementNode
};

// ForExpr = "for" [ ForMode ] [ IdentifierPipe ] Block
struct ForExprNode {
  Span span;
  std::optional<NodePtr> mode; // ForRangeClauseNode, ForIterClauseNode,
                               // or bare condition expression;
                               // absent = infinite loop
  std::optional<IdentifierNode> accumulator; // |acc| pipe name, if present
  NodePtr body;                              // BlockNode
};

// RangeExpr = "(" Expression ".." Expression ")"
struct RangeExprNode {
  Span span;
  NodePtr low;  // inclusive start
  NodePtr high; // exclusive end
};

// SpawnExpr = [ Generic ] "spawn" [ IdentifierPipe ] ( Block | Identifier )
struct SpawnExprNode {
  Span span;
  std::optional<GenericNode> generic; // |String| channel type, if present
  std::optional<IdentifierNode> pipe; // named task argument: |task|
  NodePtr body;                       // BlockNode or IdentifierNode
};

// OrExpr = Expression "or" [ IdentifierPipe ] Block
struct OrExprNode {
  Span span;
  NodePtr expr;                       // the expression that may be an error
  std::optional<IdentifierNode> pipe; // |err| capture, if present
  NodePtr fallback;                   // BlockNode — the or-clause body
};

// FuncExpr = "fn" [ Generic ] Signature Block
struct FuncExprNode {
  Span span;
  std::optional<GenericNode> generic;
  SignatureNode signature;
  NodePtr body; // BlockNode
};

// ImportExpr = "import" StringLiteral  (used as an expression)
struct ImportExprNode {
  Span span;
  std::string_view path; // string literal contents, without surrounding quotes
};

// ===========================================================================
// Section 5: Statement nodes
// ===========================================================================

// VarDecl = Identifier Type [ "=" Expression ]
struct VarDeclNode {
  Span span;
  IdentifierNode name;
  std::optional<NodePtr> type; // explicit type annotation, if present
  std::optional<NodePtr> init; // initialiser expression, if present
};

// DeclAssign = IdentifierList ":=" ExpressionList
struct DeclAssignNode {
  Span span;
  IdentifierListNode targets;
  NodePtr value; // rhs expression (or TupleNode for multi-value)
};

// Assignment = AssignTargetList assignment_operator ExpressionList
struct AssignNode {
  Span span;
  std::vector<NodePtr> targets; // IdentifierNode, SelectorNode, IndexExprNode
  Token::Kind op; // Assignment, AddAssignment, SubAssignment, etc.
  std::vector<NodePtr> values;
};

// Identifier "++"
struct IncrementNode {
  Span span;
  NodePtr operand;
};

// Identifier "--"
struct DecrementNode {
  Span span;
  NodePtr operand;
};

// "return" [ ExpressionList ]
struct ReturnNode {
  Span span;
  std::vector<NodePtr> values; // empty = bare `return`
};

// "break" [ ExpressionList ]
struct BreakNode {
  Span span;
  std::vector<NodePtr> values; // value passed to break expression, if any
};

// "next"
struct NextNode {
  Span span;
};

// ===========================================================================
// Section 6: Top-level declaration nodes
//
// Ordering within this section respects by-value dependencies:
//   EnumFieldNode  → EnumDeclNode
//   ReceiverNode   (standalone)
//   InterfaceFieldNode (needs SignatureNode) → InterfaceDeclNode
//   StructMemberNode   (standalone)         → StructDeclNode
//   FuncDeclNode (needs GenericNode, ReceiverNode, SignatureNode)
//   ConstDeclNode (standalone)
// ===========================================================================

// ConstDecl = [ "pub" ] "const" Identifier [ Type ] "=" Expression
struct ConstDeclNode {
  Span span;
  bool is_public;
  IdentifierNode name;
  std::optional<NodePtr> type; // explicit type, if provided
  NodePtr value;               // initialiser (required)
};

// EnumField = Identifier [ "{" Identifier ":" Expression { "," ... } "}" ]
struct EnumFieldNode {
  Span span;
  IdentifierNode name;
  std::vector<FieldAssignmentNode>
      initializer; // {name: expr, ...}; empty if none
};

// EnumDecl = [ "pub" ] "enum" Identifier "{" EnumField { terminal EnumField }
// "}"
struct EnumDeclNode {
  Span span;
  bool is_public;
  IdentifierNode name;
  std::vector<EnumFieldNode> fields;
};

// Receiver = "(" Identifier Type ")"   e.g. (u User)
struct ReceiverNode {
  Span span;
  IdentifierNode name; // the receiver variable name (e.g. `u`)
  NodePtr type;        // the receiver type
};

// FuncDecl = [ "pub" ] "fn" [ Generic ] [ Receiver ] Identifier Signature Block
struct FuncDeclNode {
  Span span;
  bool is_public;
  std::optional<GenericNode> generic;
  std::optional<ReceiverNode> receiver;
  IdentifierNode name;
  SignatureNode signature;
  NodePtr body; // BlockNode
};

// ImportDecl = "import" StringLiteral  (used as a declaration)
struct ImportDeclNode {
  Span span;
  std::string_view path; // string literal contents, without surrounding quotes
};

// InterfaceField = [ "pub" ] Identifier Signature
struct InterfaceFieldNode {
  Span span;
  bool is_public;
  IdentifierNode name;
  SignatureNode signature;
};

// InterfaceDecl = [ "pub" ] "interface" [ Generic ] Identifier "{" [
// InterfaceField ... ] "}"
struct InterfaceDeclNode {
  Span span;
  bool is_public;
  std::optional<GenericNode> generic;
  IdentifierNode name;
  std::vector<InterfaceFieldNode> methods;
};

// StructMember = [ "pub" ] ( FieldSpec | FuncDecl )
// The member field holds either a FieldSpecNode or a FuncDeclNode.
struct StructMemberNode {
  Span span;
  bool is_public;
  NodePtr member; // FieldSpecNode or FuncDeclNode
};

// StructDecl = [ "pub" ] "struct" [ Generic ] Identifier [ "<" IdentifierList ]
// "{" ... "}"
struct StructDeclNode {
  Span span;
  bool is_public;
  std::optional<GenericNode> generic;
  IdentifierNode name;
  std::vector<IdentifierNode> embeds; // mixin names from `< A, B` clause
  std::vector<StructMemberNode> members;
};

// ===========================================================================
// Section 7: Structural nodes
// ===========================================================================

// Block = "{" { ( Expression | Statement ) [ terminal ] } "}"
struct BlockNode {
  Span span;
  std::vector<NodePtr> stmts;
};

// SourceNode is the root of one file's AST.
struct SourceNode {
  Span span;
  std::vector<NodePtr> declarations; // top-level declarations for this file
};

// PackageNode is the root of the AST for one package (one directory).
// Each child SourceNode corresponds to one source file in the package.
struct PackageNode {
  Span span;
  std::vector<NodePtr> sources; // one SourceNode per file
};

// ===========================================================================
// Node: the discriminated union.
//
// Defined last so that every alternative type is complete. NodePtr
// (unique_ptr<Node>) appears in structs above — unique_ptr only requires the
// pointee to be complete when its destructor is instantiated (in a .cpp file
// that sees the full header), not at the point of the field declaration.
// ===========================================================================

struct Node {
  // clang-format off
  using Variant = std::variant<
    // --- Leaves / atoms ---
    IdentifierNode,     IdentifierListNode,
    BoolLiteralNode,    IntegerLiteralNode,  FloatLiteralNode,
    StringLiteralNode,  StringFragmentNode,
    ArrayLiteralNode,   MapLiteralNode,      KeyValueNode,
    StructLiteralNode,  FieldAssignmentNode,

    // --- Types ---
    UnionTypeNode,  ArrayTypeNode,  MapTypeNode,   FuncTypeNode,
    RangeTypeNode,  StructTypeNode, GenericNode,

    // --- Shared sub-nodes ---
    CaseArmNode, ParameterNode,

    // --- Expressions ---
    BinaryExprNode,     UnaryExprNode,     GroupExprNode,
    CallExprNode,       IndexExprNode,     SliceNode,          SelectorNode,
    IfExprNode,         SwitchExprNode,
    ForExprNode,        ForRangeClauseNode, ForIterClauseNode,
    RangeExprNode,      SpawnExprNode,     OrExprNode,
    FuncExprNode,       ImportExprNode,

    // --- Statements ---
    VarDeclNode,    DeclAssignNode,  AssignNode,
    IncrementNode,  DecrementNode,
    ReturnNode,     BreakNode,       NextNode,

    // --- Declarations ---
    ConstDeclNode,
    EnumDeclNode,        EnumFieldNode,
    FuncDeclNode,        ReceiverNode,        SignatureNode,
    ImportDeclNode,
    InterfaceDeclNode,   InterfaceFieldNode,
    StructDeclNode,      StructMemberNode,    FieldSpecNode,

    // --- Structure ---
    BlockNode, PackageNode, SourceNode
  >;
  // clang-format on

  Span span;
  Variant data;

  template <typename T>
  explicit Node(Span s, T value) : span(s), data(std::move(value)) {}
};

// ---------------------------------------------------------------------------
// Factory function
//
// make_node<T>(span, field1, field2, ...)
//
// Constructs a Node holding a T using aggregate initialisation. The span is
// passed both to Node::span and as the first field of T (which every node
// struct declares as `Span span`). Remaining arguments map to T's fields in
// declaration order, excluding the leading span.
//
// Example:
//   auto n = make_node<BinaryExprNode>(span, std::move(lhs), op,
//   std::move(rhs));
// ---------------------------------------------------------------------------

template <typename T, typename... Args>
NodePtr make_node(Span span, Args &&...args) {
  return std::make_unique<Node>(span, T{span, std::forward<Args>(args)...});
}

// ---------------------------------------------------------------------------
// Debug printer — compact indented tree, useful during parser development.
// Each node type name is printed with inline scalar fields, followed by
// child nodes recursively indented by two spaces per level.
// ---------------------------------------------------------------------------
void dump_ast(const Node &node, std::ostream &os, int indent = 0);

} // namespace mc
