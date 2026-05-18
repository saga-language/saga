// Copywrite 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "frontend/ast.hpp"

#include <ostream>
#include <string>

namespace saga {
namespace {

// ---------------------------------------------------------------------------
// Utilities
// ---------------------------------------------------------------------------

std::string pad(int n) {
  return std::string(static_cast<std::size_t>(n) * 2, ' ');
}

// Human-readable symbol for operator token kinds.
std::string_view kind_name(Token::Kind k) noexcept {
  using K = Token::Kind;
  switch (k) {
  // Arithmetic
  case K::Add:
    return "+";
  case K::Sub:
    return "-";
  case K::Multiply:
    return "*";
  case K::Divide:
    return "/";
  case K::Modulo:
    return "%";
  case K::Pow:
    return "**";
  // Bitwise
  case K::BitwiseAnd:
    return "&";
  case K::BitwiseOr:
    return "|";
  case K::BitwiseXor:
    return "^";
  case K::BitwiseNot:
    return "~";
  case K::LeftShift:
    return "<<";
  case K::RightShift:
    return ">>";
  // Comparison / logical
  case K::Equal:
    return "==";
  case K::NotEqual:
    return "!=";
  case K::LessThan:
    return "<";
  case K::LessThanEqual:
    return "<=";
  case K::GreaterThan:
    return ">";
  case K::GreaterThanEqual:
    return ">=";
  case K::LogicalAnd:
    return "&&";
  case K::LogicalOr:
    return "||";
  // Unary
  case K::Not:
    return "!";
  // Assignment
  case K::Assignment:
    return "=";
  case K::DeclAssignment:
    return ":=";
  case K::AddAssignment:
    return "+=";
  case K::SubAssignment:
    return "-=";
  case K::MulAssignment:
    return "*=";
  case K::DivAssignment:
    return "/=";
  default:
    return "?";
  }
}

// ---------------------------------------------------------------------------
// Forward declaration — helpers and the visitor are mutually recursive.
// ---------------------------------------------------------------------------
void dump_impl(const Node &node, std::ostream &os, int indent);

// ---------------------------------------------------------------------------
// Helpers for NodePtr and optional<NodePtr>
// ---------------------------------------------------------------------------

void dump_ptr(const NodePtr &p, std::ostream &os, int indent) {
  if (p)
    dump_impl(*p, os, indent);
  else
    os << pad(indent) << "(null)\n";
}

void dump_opt_ptr(const std::optional<NodePtr> &opt, std::ostream &os,
                  int indent) {
  if (opt && *opt)
    dump_impl(**opt, os, indent);
}

// ---------------------------------------------------------------------------
// Helpers for by-value sub-node types.
// These mirror what dump_impl does for the same types when they appear as
// NodePtr children, but accept the struct directly (they are stored by value
// in vectors or optional<> fields of parent nodes).
// ---------------------------------------------------------------------------

void dump_identifier(const IdentifierNode &n, std::ostream &os, int indent) {
  os << pad(indent) << "IdentifierNode \"" << n.name << "\"\n";
}

void dump_identifier_list(const IdentifierListNode &n, std::ostream &os,
                          int indent) {
  os << pad(indent) << "IdentifierListNode\n";
  for (const auto &id : n.identifiers)
    dump_identifier(id, os, indent + 1);
}

void dump_field_spec(const FieldSpecNode &n, std::ostream &os, int indent) {
  os << pad(indent) << "FieldSpecNode\n";
  dump_identifier_list(n.names, os, indent + 1);
  dump_ptr(n.type, os, indent + 1);
}

void dump_field_assignment(const FieldAssignmentNode &n, std::ostream &os,
                           int indent) {
  os << pad(indent) << "FieldAssignmentNode \"" << n.name.name << "\"\n";
  dump_ptr(n.value, os, indent + 1);
}

void dump_key_value(const KeyValueNode &n, std::ostream &os, int indent) {
  os << pad(indent) << "KeyValueNode\n";
  dump_ptr(n.key, os, indent + 1);
  dump_ptr(n.value, os, indent + 1);
}

void dump_generic(const GenericNode &n, std::ostream &os, int indent) {
  os << pad(indent) << "GenericNode\n";
  for (const auto &tp : n.type_params)
    dump_ptr(tp, os, indent + 1);
}

void dump_opt_generic(const std::optional<GenericNode> &opt, std::ostream &os,
                      int indent) {
  if (opt)
    dump_generic(*opt, os, indent);
}

void dump_parameter(const ParameterNode &n, std::ostream &os, int indent) {
  os << pad(indent) << "ParameterNode"
     << (n.is_variadic ? " variadic=true" : "") << "\n";
  dump_identifier_list(n.names, os, indent + 1);
  dump_ptr(n.type, os, indent + 1);
}

void dump_signature(const SignatureNode &n, std::ostream &os, int indent) {
  os << pad(indent) << "SignatureNode\n";
  for (const auto &p : n.params)
    dump_parameter(p, os, indent + 1);
  for (const auto &r : n.returns)
    dump_ptr(r, os, indent + 1);
}

void dump_case_arm(const CaseArmNode &n, std::ostream &os, int indent) {
  os << pad(indent) << "CaseArmNode\n";
  for (const auto &p : n.patterns)
    dump_ptr(p, os, indent + 1);
  dump_ptr(n.body, os, indent + 1);
}

void dump_enum_field(const EnumFieldNode &n, std::ostream &os, int indent) {
  os << pad(indent) << "EnumFieldNode \"" << n.name.name << "\"\n";
  for (const auto &fa : n.initializer)
    dump_field_assignment(fa, os, indent + 1);
}

void dump_receiver(const ReceiverNode &n, std::ostream &os, int indent) {
  os << pad(indent) << "ReceiverNode \"" << n.name.name << "\"\n";
  dump_ptr(n.type, os, indent + 1);
}

void dump_interface_field(const InterfaceFieldNode &n, std::ostream &os,
                          int indent) {
  os << pad(indent) << "InterfaceFieldNode" << (n.is_public ? " pub" : "")
     << " \"" << n.name.name << "\"\n";
  dump_signature(n.signature, os, indent + 1);
}

void dump_struct_member(const StructMemberNode &n, std::ostream &os,
                        int indent) {
  os << pad(indent) << "StructMemberNode" << (n.is_public ? " pub" : "")
     << "\n";
  dump_ptr(n.member, os, indent + 1);
}

// ---------------------------------------------------------------------------
// Main visitor — one lambda arm per node type (60 total).
// ---------------------------------------------------------------------------

void dump_impl(const Node &node, std::ostream &os, int indent) {
  const int c = indent + 1; // child indent

  std::visit(
      overloaded{

          // -----------------------------------------------------------------------
          // Leaves / atoms
          // -----------------------------------------------------------------------

          [&](const IdentifierNode &n) {
            os << pad(indent) << "IdentifierNode \"" << n.name << "\"\n";
          },
          [&](const IdentifierListNode &n) {
            os << pad(indent) << "IdentifierListNode\n";
            for (const auto &id : n.identifiers)
              dump_identifier(id, os, c);
          },
          [&](const BoolLiteralNode &n) {
            os << pad(indent) << "BoolLiteralNode " << n.literal << "\n";
          },
          [&](const IntegerLiteralNode &n) {
            os << pad(indent) << "IntegerLiteralNode " << n.literal << "\n";
          },
          [&](const FloatLiteralNode &n) {
            os << pad(indent) << "FloatLiteralNode " << n.literal << "\n";
          },
          [&](const StringFragmentNode &n) {
            os << pad(indent) << "StringFragmentNode \"" << n.text << "\"\n";
          },
          [&](const StringLiteralNode &n) {
            os << pad(indent) << "StringLiteralNode\n";
            for (const auto &frag : n.fragments)
              dump_ptr(frag, os, c);
          },
          [&](const ArrayLiteralNode &n) {
            os << pad(indent) << "ArrayLiteralNode\n";
            for (const auto &el : n.elements)
              dump_ptr(el, os, c);
          },
          [&](const KeyValueNode &n) { dump_key_value(n, os, indent); },
          [&](const MapLiteralNode &n) {
            os << pad(indent) << "MapLiteralNode\n";
            for (const auto &kv : n.entries)
              dump_key_value(kv, os, c);
          },
          [&](const FieldAssignmentNode &n) {
            dump_field_assignment(n, os, indent);
          },
          [&](const StructLiteralNode &n) {
            os << pad(indent) << "StructLiteralNode\n";
            dump_ptr(n.type_expr, os, c);
            for (const auto &fa : n.fields)
              dump_field_assignment(fa, os, c);
          },

          // -----------------------------------------------------------------------
          // Types
          // -----------------------------------------------------------------------

          [&](const UnionTypeNode &n) {
            os << pad(indent) << "UnionTypeNode\n";
            for (const auto &t : n.types)
              dump_ptr(t, os, c);
          },
          [&](const ArrayTypeNode &n) {
            os << pad(indent) << "ArrayTypeNode\n";
            dump_ptr(n.element_type, os, c);
          },
          [&](const MapTypeNode &n) {
            os << pad(indent) << "MapTypeNode\n";
            dump_ptr(n.key_type, os, c);
            dump_ptr(n.value_type, os, c);
          },
          [&](const FuncTypeNode &n) {
            os << pad(indent) << "FuncTypeNode\n";
            for (const auto &p : n.params)
              dump_ptr(p, os, c);
            for (const auto &r : n.returns)
              dump_ptr(r, os, c);
          },
          [&](const RangeTypeNode &n) {
            os << pad(indent) << "RangeTypeNode\n";
            dump_ptr(n.element_type, os, c);
          },
          [&](const FieldSpecNode &n) { dump_field_spec(n, os, indent); },
          [&](const StructTypeNode &n) {
            os << pad(indent) << "StructTypeNode\n";
            for (const auto &f : n.fields)
              dump_field_spec(f, os, c);
          },
          [&](const GenericTypeAppNode &n) {
            os << pad(indent) << "GenericTypeAppNode\n";
            for (auto &ta : n.type_args)
              dump_impl(*ta, os, c);
            dump_impl(*n.base_type, os, c);
          },
          [&](const GenericNode &n) { dump_generic(n, os, indent); },
          [&](const TypeParamNode &n) {
            os << pad(indent) << "TypeParamNode \"" << n.name.name << "\"";
            if (n.constraint)
              os << " constraint=\"" << n.constraint->name << "\"";
            os << "\n";
          },

          // -----------------------------------------------------------------------
          // Shared sub-nodes
          // -----------------------------------------------------------------------

          [&](const CaseArmNode &n) { dump_case_arm(n, os, indent); },
          [&](const ParameterNode &n) { dump_parameter(n, os, indent); },
          [&](const SignatureNode &n) { dump_signature(n, os, indent); },

          // -----------------------------------------------------------------------
          // Expressions
          // -----------------------------------------------------------------------

          [&](const BinaryExprNode &n) {
            os << pad(indent) << "BinaryExprNode op=" << kind_name(n.op)
               << "\n";
            dump_ptr(n.lhs, os, c);
            dump_ptr(n.rhs, os, c);
          },
          [&](const UnaryExprNode &n) {
            os << pad(indent) << "UnaryExprNode op=" << kind_name(n.op) << "\n";
            dump_ptr(n.operand, os, c);
          },
          [&](const GroupExprNode &n) {
            os << pad(indent) << "GroupExprNode\n";
            dump_ptr(n.inner, os, c);
          },
          [&](const CallExprNode &n) {
            os << pad(indent) << "CallExprNode\n";
            dump_ptr(n.callee, os, c);
            for (const auto &a : n.args)
              dump_ptr(a, os, c);
          },
          [&](const SliceNode &n) {
            os << pad(indent) << "SliceNode\n";
            dump_opt_ptr(n.low, os, c);
            dump_opt_ptr(n.high, os, c);
          },
          [&](const IndexExprNode &n) {
            os << pad(indent) << "IndexExprNode\n";
            dump_ptr(n.object, os, c);
            dump_ptr(n.index, os, c);
          },
          [&](const SelectorNode &n) {
            os << pad(indent) << "SelectorNode\n";
            dump_ptr(n.object, os, c);
            dump_identifier(n.field, os, c);
          },
          [&](const IfExprNode &n) {
            os << pad(indent) << "IfExprNode\n";
            dump_ptr(n.condition, os, c);
            dump_ptr(n.then_block, os, c);
            dump_opt_ptr(n.else_block, os, c);
          },
          [&](const SwitchExprNode &n) {
            os << pad(indent) << "SwitchExprNode\n";
            dump_ptr(n.subject, os, c);
            for (const auto &arm : n.arms)
              dump_case_arm(arm, os, c);
            dump_opt_ptr(n.else_body, os, c);
          },
          [&](const ForRangeClauseNode &n) {
            os << pad(indent) << "ForRangeClauseNode\n";
            for (const auto &v : n.vars)
              dump_identifier(v, os, c);
            dump_ptr(n.iterable, os, c);
          },
          [&](const ForIterClauseNode &n) {
            os << pad(indent) << "ForIterClauseNode\n";
            dump_ptr(n.init, os, c);
            dump_ptr(n.condition, os, c);
            dump_ptr(n.update, os, c);
          },
          [&](const ForExprNode &n) {
            os << pad(indent) << "ForExprNode";
            if (n.accumulator)
              os << " acc=\"" << n.accumulator->name << "\"";
            os << "\n";
            dump_opt_ptr(n.mode, os, c);
            dump_ptr(n.body, os, c);
          },
          [&](const RangeExprNode &n) {
            os << pad(indent) << "RangeExprNode\n";
            dump_ptr(n.low, os, c);
            dump_ptr(n.high, os, c);
          },
          [&](const SpawnExprNode &n) {
            os << pad(indent) << "SpawnExprNode";
            if (n.pipe)
              os << " pipe=\"" << n.pipe->name << "\"";
            os << "\n";
            dump_opt_generic(n.generic, os, c);
            dump_ptr(n.body, os, c);
          },
          [&](const OrExprNode &n) {
            os << pad(indent) << "OrExprNode";
            if (n.pipe)
              os << " pipe=\"" << n.pipe->name << "\"";
            os << "\n";
            dump_ptr(n.expr, os, c);
            dump_ptr(n.fallback, os, c);
          },
          [&](const FuncExprNode &n) {
            os << pad(indent) << "FuncExprNode\n";
            dump_opt_generic(n.generic, os, c);
            dump_signature(n.signature, os, c);
            dump_ptr(n.body, os, c);
          },
          [&](const ImportExprNode &n) {
            os << pad(indent) << "ImportExprNode \"" << n.path << "\"\n";
          },

          // -----------------------------------------------------------------------
          // Statements
          // -----------------------------------------------------------------------

          [&](const VarDeclNode &n) {
            os << pad(indent) << "VarDeclNode \"" << n.name.name << "\"\n";
            dump_opt_ptr(n.type, os, c);
            dump_opt_ptr(n.init, os, c);
          },
          [&](const DeclAssignNode &n) {
            os << pad(indent) << "DeclAssignNode\n";
            dump_identifier_list(n.targets, os, c);
            dump_ptr(n.value, os, c);
          },
          [&](const AssignNode &n) {
            os << pad(indent) << "AssignNode op=" << kind_name(n.op) << "\n";
            for (const auto &t : n.targets)
              dump_ptr(t, os, c);
            for (const auto &v : n.values)
              dump_ptr(v, os, c);
          },
          [&](const IncrementNode &n) {
            os << pad(indent) << "IncrementNode\n";
            dump_ptr(n.operand, os, c);
          },
          [&](const DecrementNode &n) {
            os << pad(indent) << "DecrementNode\n";
            dump_ptr(n.operand, os, c);
          },
          [&](const ReturnNode &n) {
            os << pad(indent) << "ReturnNode\n";
            for (const auto &v : n.values)
              dump_ptr(v, os, c);
          },
          [&](const BreakNode &n) {
            os << pad(indent) << "BreakNode\n";
            for (const auto &v : n.values)
              dump_ptr(v, os, c);
          },
          [&](const NextNode &) { os << pad(indent) << "NextNode\n"; },

          // -----------------------------------------------------------------------
          // Declarations
          // -----------------------------------------------------------------------

          [&](const ConstDeclNode &n) {
            os << pad(indent) << "ConstDeclNode" << (n.is_public ? " pub" : "")
               << " \"" << n.name.name << "\"\n";
            dump_opt_ptr(n.type, os, c);
            dump_ptr(n.value, os, c);
          },
          [&](const EnumFieldNode &n) { dump_enum_field(n, os, indent); },
          [&](const EnumDeclNode &n) {
            os << pad(indent) << "EnumDeclNode" << (n.is_public ? " pub" : "")
               << " \"" << n.name.name << "\"\n";
            for (const auto &f : n.fields)
              dump_enum_field(f, os, c);
          },
          [&](const ReceiverNode &n) { dump_receiver(n, os, indent); },
          [&](const FuncDeclNode &n) {
            os << pad(indent) << "FuncDeclNode" << (n.is_public ? " pub" : "")
               << (n.is_extern ? " extern" : "")
               << " \"" << n.name.name << "\"\n";
            dump_opt_generic(n.generic, os, c);
            if (n.receiver)
              dump_receiver(*n.receiver, os, c);
            dump_signature(n.signature, os, c);
            if (n.body)
              dump_ptr(n.body, os, c);
          },
          [&](const ImportDeclNode &n) {
            os << pad(indent) << "ImportDeclNode \"" << n.path << "\"\n";
          },
          [&](const InterfaceFieldNode &n) {
            dump_interface_field(n, os, indent);
          },
          [&](const InterfaceDeclNode &n) {
            os << pad(indent) << "InterfaceDeclNode"
               << (n.is_public ? " pub" : "") << " \"" << n.name.name << "\"\n";
            dump_opt_generic(n.generic, os, c);
            for (const auto &m : n.methods)
              dump_interface_field(m, os, c);
          },
          [&](const StructMemberNode &n) { dump_struct_member(n, os, indent); },
          [&](const StructDeclNode &n) {
            os << pad(indent) << "StructDeclNode" << (n.is_public ? " pub" : "")
               << " \"" << n.name.name << "\"\n";
            dump_opt_generic(n.generic, os, c);
            for (const auto &e : n.embeds)
              dump_ptr(e, os, c);
            for (const auto &m : n.members)
              dump_struct_member(m, os, c);
          },

          // -----------------------------------------------------------------------
          // Structure
          // -----------------------------------------------------------------------

          [&](const BlockNode &n) {
            os << pad(indent) << "BlockNode\n";
            for (const auto &s : n.stmts)
              dump_ptr(s, os, c);
          },
          [&](const PackageNode &n) {
            os << pad(indent) << "PackageNode\n";
            for (const auto &s : n.sources)
              dump_ptr(s, os, c);
          },
          [&](const SourceNode &n) {
            os << pad(indent) << "SourceNode\n";
            for (const auto &d : n.declarations)
              dump_ptr(d, os, c);
          },

      },
      node.data);
}

} // namespace

// ---------------------------------------------------------------------------
// Public entry point
// ---------------------------------------------------------------------------

void dump_ast(const Node &node, std::ostream &os, int indent) {
  dump_impl(node, os, indent);
}

std::optional<std::string_view> type_param_name(const Node &node) {
  if (auto *tp = std::get_if<TypeParamNode>(&node.data))
    return tp->name.name;
  if (auto *id = std::get_if<IdentifierNode>(&node.data))
    return id->name;
  return std::nullopt;
}

} // namespace saga
