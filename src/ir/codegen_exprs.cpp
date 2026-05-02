// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>

#include <charconv>

namespace mc {

// ===========================================================================
// Expression emission
// ===========================================================================

llvm::Value *CodeGen::emit_expr(const Node &node) {
  return std::visit(
      overloaded{
          [&](const IntegerLiteralNode &n) -> llvm::Value * {
            return emit_int_literal(n);
          },
          [&](const FloatLiteralNode &n) -> llvm::Value * {
            return emit_float_literal(n);
          },
          [&](const BoolLiteralNode &n) -> llvm::Value * {
            return emit_bool_literal(n);
          },
          [&](const StringLiteralNode &n) -> llvm::Value * {
            return emit_string_literal(n);
          },
          [&](const BinaryExprNode &n) -> llvm::Value * {
            return emit_binary_expr(n, node);
          },
          [&](const UnaryExprNode &n) -> llvm::Value * {
            return emit_unary_expr(n);
          },
          [&](const GroupExprNode &n) -> llvm::Value * {
            return emit_group_expr(n);
          },
          [&](const IfExprNode &n) -> llvm::Value * {
            return emit_if_expr(n);
          },
          [&](const ForExprNode &n) -> llvm::Value * {
            return emit_for_expr(n);
          },
          [&](const SwitchExprNode &n) -> llvm::Value * {
            return emit_switch_expr(n);
          },
          [&](const StructLiteralNode &n) -> llvm::Value * {
            return emit_struct_literal(n);
          },
          [&](const SelectorNode &n) -> llvm::Value * {
            return emit_selector(n, node);
          },
          [&](const ArrayLiteralNode &n) -> llvm::Value * {
            return emit_array_literal(n);
          },
          [&](const MapLiteralNode &n) -> llvm::Value * {
            return emit_map_literal(n);
          },
          [&](const IndexExprNode &n) -> llvm::Value * {
            return emit_index_expr(n);
          },
          [&](const BreakNode &) -> llvm::Value * {
            if (!loop_stack.empty())
              builder.CreateBr(loop_stack.back().break_bb);
            return nullptr;
          },
          [&](const NextNode &) -> llvm::Value * {
            if (!loop_stack.empty())
              builder.CreateBr(loop_stack.back().next_bb);
            return nullptr;
          },
          [&](const OrExprNode &n) -> llvm::Value * {
            return emit_or_expr(n);
          },
          [&](const CallExprNode &n) -> llvm::Value * {
            return emit_call_expr(n, node);
          },
          [&](const FuncExprNode &n) -> llvm::Value * {
            return emit_func_expr(n, node);
          },
          [&](const SpawnExprNode &n) -> llvm::Value * {
            return emit_spawn_expr(n, node);
          },
          [&](const IdentifierNode &n) -> llvm::Value * {
            return emit_identifier(n);
          },
          [&](const VarDeclNode &n) -> llvm::Value * {
            emit_var_decl(n);
            return nullptr;
          },
          [&](const DeclAssignNode &n) -> llvm::Value * {
            emit_decl_assign(n);
            return nullptr;
          },
          [&](const AssignNode &n) -> llvm::Value * {
            emit_assign(n);
            return nullptr;
          },
          [&](const ReturnNode &n) -> llvm::Value * {
            emit_return(n);
            return nullptr;
          },
          [&](const IncrementNode &n) -> llvm::Value * {
            emit_increment(n);
            return nullptr;
          },
          [&](const DecrementNode &n) -> llvm::Value * {
            emit_decrement(n);
            return nullptr;
          },
          [&](const auto &) -> llvm::Value * {
            return nullptr;
          },
      },
      node.data);
}

// ===========================================================================
// Literal emission
// ===========================================================================

/// Parse an integer literal string, handling 0b, 0o, 0x prefixes and
/// underscore separators.
static int64_t parse_int_literal(std::string_view lit) {
  // Strip underscores.
  std::string clean;
  clean.reserve(lit.size());
  for (char c : lit) {
    if (c != '_')
      clean += c;
  }

  int base = 10;
  std::string_view digits = clean;
  if (digits.size() > 2 && digits[0] == '0') {
    switch (digits[1]) {
    case 'b': case 'B': base = 2;  digits = digits.substr(2); break;
    case 'o': case 'O': base = 8;  digits = digits.substr(2); break;
    case 'x': case 'X': base = 16; digits = digits.substr(2); break;
    default: break;
    }
  }

  int64_t val = 0;
  std::from_chars(digits.data(), digits.data() + digits.size(), val, base);
  return val;
}

static double parse_float_literal(std::string_view lit) {
  std::string clean;
  clean.reserve(lit.size());
  for (char c : lit) {
    if (c != '_')
      clean += c;
  }
  double val = 0.0;
  std::from_chars(clean.data(), clean.data() + clean.size(), val);
  return val;
}

llvm::Value *CodeGen::emit_int_literal(const IntegerLiteralNode &node) {
  int64_t val = parse_int_literal(node.literal);
  return llvm::ConstantInt::get(i64_type, static_cast<uint64_t>(val),
                                /*isSigned=*/true);
}

llvm::Value *CodeGen::emit_float_literal(const FloatLiteralNode &node) {
  double val = parse_float_literal(node.literal);
  return llvm::ConstantFP::get(f64_type, val);
}

llvm::Value *CodeGen::emit_bool_literal(const BoolLiteralNode &node) {
  bool val = (node.literal == "true");
  return llvm::ConstantInt::get(i1_type, val ? 1 : 0);
}

// ===========================================================================
// String literals
// ===========================================================================

std::string CodeGen::unescape_fragment(std::string_view raw) {
  if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"')
    raw = raw.substr(1, raw.size() - 2);
  else if (raw.size() >= 1 && raw.front() == '"')
    raw = raw.substr(1);
  else if (raw.size() >= 1 && raw.back() == '"')
    raw = raw.substr(0, raw.size() - 1);
  if (raw.size() >= 1 && raw.front() == '}')
    raw = raw.substr(1);
  if (raw.size() >= 1 && raw.back() == '{')
    raw = raw.substr(0, raw.size() - 1);

  std::string out;
  out.reserve(raw.size());
  for (size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '\\' && i + 1 < raw.size()) {
      ++i;
      switch (raw[i]) {
      case 'n':  out += '\n'; break;
      case 't':  out += '\t'; break;
      case '\\': out += '\\'; break;
      case '"':  out += '"';  break;
      case '{':  out += '{';  break;
      default:   out += '\\'; out += raw[i]; break;
      }
    } else {
      out += raw[i];
    }
  }
  return out;
}

/// Convert an LLVM value to an mc_string* based on its semantic type.
llvm::Value *CodeGen::emit_to_string(llvm::Value *val, const TypePtr &sem) {
  if (!val || !sem)
    return val;

  switch (sem->kind) {
  case TypeKind::String:
    return val; // Already a string pointer.
  case TypeKind::Int: {
    auto *fn = module->getFunction("saga_int_to_string");
    return builder.CreateCall(fn, {val}, "istr");
  }
  case TypeKind::Float: {
    auto *fn = module->getFunction("saga_float_to_string");
    return builder.CreateCall(fn, {val}, "fstr");
  }
  case TypeKind::Bool: {
    auto *ext = builder.CreateZExt(val, i64_type, "bext");
    auto *fn = module->getFunction("saga_bool_to_string");
    return builder.CreateCall(fn, {ext}, "bstr");
  }
  default:
    // For types we can't convert, return an empty string placeholder.
    return make_string_constant("");
  }
}

llvm::Value *CodeGen::emit_string_literal(const StringLiteralNode &node) {
  // Check if this is a plain string (no interpolation).
  bool has_interp = false;
  for (auto &frag : node.fragments) {
    if (!std::holds_alternative<StringFragmentNode>(frag->data)) {
      has_interp = true;
      break;
    }
  }

  if (!has_interp) {
    // Plain string — concatenate all text fragments into one constant.
    std::string text;
    for (auto &frag : node.fragments) {
      if (auto *sf = std::get_if<StringFragmentNode>(&frag->data))
        text += unescape_fragment(sf->text);
    }
    return make_string_constant(text);
  }

  // Interpolated string — emit each part and concatenate.
  auto *concat_fn = module->getFunction("saga_string_concat");
  llvm::Value *result = nullptr;

  for (auto &frag : node.fragments) {
    llvm::Value *part = nullptr;

    if (auto *sf = std::get_if<StringFragmentNode>(&frag->data)) {
      std::string text = unescape_fragment(sf->text);
      if (text.empty())
        continue;
      part = make_string_constant(text);
    } else {
      // Interpolated expression — emit it and convert to string.
      auto *val = emit_expr(*frag);
      auto frag_sem = semantic_type(*frag);
      part = emit_to_string(val, frag_sem);
    }

    if (!part)
      continue;

    if (!result) {
      result = part;
    } else {
      result = builder.CreateCall(concat_fn, {result, part}, "interp");
    }
  }

  return result ? result : make_string_constant("");
}

llvm::Value *CodeGen::make_string_constant(const std::string &text) {
  auto it = string_constants.find(text);
  if (it != string_constants.end())
    return it->second;

  auto *char_array =
      llvm::ConstantDataArray::getString(context, text, /*AddNull=*/false);
  auto *raw_global = new llvm::GlobalVariable(
      *module, char_array->getType(), true,
      llvm::GlobalValue::PrivateLinkage, char_array, ".str");
  raw_global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  raw_global->setAlignment(llvm::Align(1));

  auto *data_ptr = llvm::ConstantExpr::getInBoundsGetElementPtr(
      char_array->getType(), raw_global,
      llvm::ArrayRef<llvm::Constant *>{
          llvm::ConstantInt::get(i64_type, 0),
          llvm::ConstantInt::get(i64_type, 0)});
  auto *length = llvm::ConstantInt::get(i64_type, text.size());
  auto *refcount = llvm::ConstantInt::getSigned(i64_type, -1); // static
  auto *str_const =
      llvm::ConstantStruct::get(string_type, {data_ptr, length, refcount});

  auto *str_global = new llvm::GlobalVariable(
      *module, string_type, true,
      llvm::GlobalValue::PrivateLinkage, str_const, ".mc_str");
  str_global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

  string_constants[text] = str_global;
  return str_global;
}

// ===========================================================================
// Semantic type query
// ===========================================================================

TypePtr CodeGen::semantic_type(const Node &node) const {
  if (current_instantiation_) {
    auto it = current_instantiation_->node_types.find(&node);
    if (it != current_instantiation_->node_types.end())
      return it->second;
  }
  auto it = analyzer.node_types.find(&node);
  if (it != analyzer.node_types.end())
    return it->second;
  return nullptr;
}

// ---------------------------------------------------------------------------
// Per-instantiation side-table accessors (monomorphism_plan.md, Step 4)
//
// Each accessor checks current_instantiation_'s per-instantiation view
// first, then falls through to the corresponding analyzer.<table> global.
// A miss in the instantiation view is NOT an error — nodes outside the
// generic body live only in the global tables.
// ---------------------------------------------------------------------------

const Symbol *CodeGen::node_symbol(const Node &node) const {
  if (current_instantiation_) {
    auto it = current_instantiation_->node_symbols.find(&node);
    if (it != current_instantiation_->node_symbols.end())
      return &it->second;
  }
  auto it = analyzer.node_symbols.find(&node);
  if (it != analyzer.node_symbols.end())
    return &it->second;
  return nullptr;
}

const std::vector<Analyzer::CaptureInfo> *
CodeGen::node_captures_of(const Node &node) const {
  if (current_instantiation_) {
    auto it = current_instantiation_->node_captures.find(&node);
    if (it != current_instantiation_->node_captures.end())
      return &it->second;
  }
  auto it = analyzer.node_captures.find(&node);
  if (it != analyzer.node_captures.end())
    return &it->second;
  return nullptr;
}

const std::vector<Analyzer::SpawnCaptureInfo> *
CodeGen::spawn_captures_of(const Node &node) const {
  if (current_instantiation_) {
    auto it = current_instantiation_->spawn_captures.find(&node);
    if (it != current_instantiation_->spawn_captures.end())
      return &it->second;
  }
  auto it = analyzer.spawn_captures.find(&node);
  if (it != analyzer.spawn_captures.end())
    return &it->second;
  return nullptr;
}

TypePtr CodeGen::iterable_next_elem_type_of(const Node &node) const {
  if (current_instantiation_) {
    auto it = current_instantiation_->iterable_next_elem_type.find(&node);
    if (it != current_instantiation_->iterable_next_elem_type.end())
      return it->second;
  }
  auto it = analyzer.iterable_next_elem_type.find(&node);
  if (it != analyzer.iterable_next_elem_type.end())
    return it->second;
  return nullptr;
}

TypePtr CodeGen::spawn_channel_elem_type_of(const Node &node) const {
  if (current_instantiation_) {
    auto it = current_instantiation_->spawn_channel_elem_types.find(&node);
    if (it != current_instantiation_->spawn_channel_elem_types.end())
      return it->second;
  }
  auto it = analyzer.spawn_channel_elem_types.find(&node);
  if (it != analyzer.spawn_channel_elem_types.end())
    return it->second;
  return nullptr;
}

const std::string *
CodeGen::struct_operator_method_of(const Node &node) const {
  if (current_instantiation_) {
    auto it = current_instantiation_->struct_operator_methods.find(&node);
    if (it != current_instantiation_->struct_operator_methods.end())
      return &it->second;
  }
  auto it = analyzer.struct_operator_methods.find(&node);
  if (it != analyzer.struct_operator_methods.end())
    return &it->second;
  return nullptr;
}

TypePtr CodeGen::span_type_at(Span span) const {
  auto scan = [&](const std::vector<std::pair<Span, TypePtr>> &v) -> TypePtr {
    for (auto &[s, t] : v) {
      if (s.start == span.start && s.end == span.end)
        return t;
    }
    return nullptr;
  };
  if (current_instantiation_) {
    if (auto t = scan(current_instantiation_->span_types))
      return t;
  }
  return scan(analyzer.span_types);
}

const std::unordered_map<uint32_t, TypePtr> *
CodeGen::node_type_args_of(const Node &node) const {
  if (current_instantiation_) {
    auto it = current_instantiation_->node_type_args.find(&node);
    if (it != current_instantiation_->node_type_args.end())
      return &it->second;
  }
  auto it = analyzer.node_type_args.find(&node);
  if (it != analyzer.node_type_args.end())
    return &it->second;
  return nullptr;
}

// ===========================================================================
// Binary expressions
// ===========================================================================

// ===========================================================================
// Struct operator overloading
// ===========================================================================

llvm::Value *CodeGen::emit_struct_binary_op(const BinaryExprNode &node,
                                             const Node &parent,
                                             const TypePtr &lhs_sem,
                                             const std::string &method) {
  auto &info = std::get<StructTypeInfo>(lhs_sem->detail);
  auto *ptr_type = llvm::PointerType::getUnqual(context);

  // ── Resolve the mangled link name for the method ──────────────────────────
  std::string link_name;
  {
    auto ml_it = struct_method_links.find(info.name);
    if (ml_it != struct_method_links.end()) {
      for (auto &[lname, mname] : ml_it->second) {
        if (mname == method) {
          link_name = lname;
          break;
        }
      }
    }
    // If not found in links (e.g. cross-package), fall back to current-package
    // mangling so the linker can resolve it.
    if (link_name.empty())
      link_name = mangle(info.name + "__" + method);
  }

  // ── Find or forward-declare the LLVM function ─────────────────────────────
  auto *callee = module->getFunction(link_name);
  if (!callee) {
    // Determine the return LLVM type from the method name.
    llvm::Type *ret_ll;
    if (method == "Compare") {
      ret_ll = i64_type; // Comparison enum
    } else if (method == "Equals" || method == "Equal") {
      ret_ll = i1_type; // Bool
    } else if (method == "Div") {
      // Div returns T | Error; we return the union struct ptr.
      auto union_sem =
          make_union_type({lhs_sem, analyzer.builtins.error_iface});
      auto *union_st = get_union_llvm_type(union_sem);
      ret_ll = union_st ? static_cast<llvm::Type *>(union_st) : ptr_type;
    } else {
      // Add, Sub, Mul: returns same struct type as self.
      auto st_it = struct_types.find(info.name);
      ret_ll = (st_it != struct_types.end())
                   ? static_cast<llvm::Type *>(st_it->second)
                   : ptr_type;
    }

    // Determine the RHS parameter type.
    auto rhs_sem = semantic_type(*node.rhs);
    llvm::Type *rhs_ll;
    if (rhs_sem && rhs_sem->kind == TypeKind::Struct) {
      auto rhs_st_it = struct_types.find(
          std::get<StructTypeInfo>(rhs_sem->detail).name);
      rhs_ll = (rhs_st_it != struct_types.end())
                   ? static_cast<llvm::Type *>(rhs_st_it->second)
                   : ptr_type;
    } else {
      rhs_ll = rhs_sem ? llvm_type(rhs_sem) : ptr_type;
    }

    auto *fn_type =
        llvm::FunctionType::get(ret_ll, {ptr_type, rhs_ll}, false);
    callee = llvm::Function::Create(
        fn_type, llvm::Function::ExternalLinkage, link_name, module.get());
  }
  if (!callee)
    return nullptr;

  // ── Build self_ptr for the LHS ─────────────────────────────────────────────
  // Prefer passing the alloca directly so the method gets a mutable ptr.
  llvm::Value *self_ptr = nullptr;
  if (auto *id = std::get_if<IdentifierNode>(&node.lhs->data)) {
    auto local_it = locals.find(std::string(id->name));
    if (local_it != locals.end()) {
      auto *alloca = local_it->second;
      auto st_it = struct_types.find(info.name);
      if (st_it != struct_types.end() &&
          alloca->getAllocatedType() == st_it->second) {
        self_ptr = alloca; // direct struct alloca — ideal
      }
    }
  }
  if (!self_ptr) {
    // Emit the expression and spill to a temp alloca.
    auto *lhs_val = emit_expr(*node.lhs);
    if (!lhs_val)
      return nullptr;
    auto st_it = struct_types.find(info.name);
    if (st_it != struct_types.end() &&
        lhs_val->getType() == st_it->second) {
      auto *func = builder.GetInsertBlock()->getParent();
      auto *tmp =
          create_entry_alloca(func, "op.self.tmp", st_it->second);
      builder.CreateStore(lhs_val, tmp);
      self_ptr = tmp;
    } else {
      self_ptr = lhs_val; // already a pointer
    }
  }

  // ── Emit the RHS argument ────────────────────────────────────────────────
  auto *rhs_val = emit_expr(*node.rhs);
  if (!rhs_val)
    return nullptr;

  // If the RHS is a struct value (not a pointer), spill it too.
  {
    auto rhs_sem = semantic_type(*node.rhs);
    if (rhs_sem && rhs_sem->kind == TypeKind::Struct) {
      auto &rinfo = std::get<StructTypeInfo>(rhs_sem->detail);
      auto st_it = struct_types.find(rinfo.name);
      if (st_it != struct_types.end() &&
          rhs_val->getType() == st_it->second) {
        auto *func = builder.GetInsertBlock()->getParent();
        auto *tmp =
            create_entry_alloca(func, "op.rhs.tmp", st_it->second);
        builder.CreateStore(rhs_val, tmp);
        rhs_val = tmp;
      }
    }
  }

  // ── Call the method ─────────────────────────────────────────────────────────
  auto *result = builder.CreateCall(callee, {self_ptr, rhs_val}, "op.res");

  // ── Post-process result based on the operator and method ────────────────
  using K = Token::Kind;

  if (method == "Compare") {
    // Compare returns Comparison enum: Less=0, Equal=1, Greater=2.
    auto *zero = llvm::ConstantInt::get(i64_type, 0); // Less
    auto *one  = llvm::ConstantInt::get(i64_type, 1); // Equal
    auto *two  = llvm::ConstantInt::get(i64_type, 2); // Greater
    switch (node.op) {
    case K::LessThan:
      return builder.CreateICmpEQ(result, zero, "lt");
    case K::LessThanEqual:
      // Less or Equal ⇔ result != Greater
      return builder.CreateICmpNE(result, two, "le");
    case K::GreaterThan:
      return builder.CreateICmpEQ(result, two, "gt");
    case K::GreaterThanEqual:
      // Greater or Equal ⇔ result != Less
      return builder.CreateICmpNE(result, zero, "ge");
    case K::Equal:
      return builder.CreateICmpEQ(result, one, "eq");
    case K::NotEqual:
      return builder.CreateICmpNE(result, one, "ne");
    default:
      return result;
    }
  }

  // Equals / Equal return Bool (i1). Negate for !=.
  if ((method == "Equals" || method == "Equal") && node.op == K::NotEqual)
    return builder.CreateNot(result, "ne");

  // Add, Sub, Mul, Div: result is already the correct type.
  return result;
}

llvm::Value *CodeGen::emit_binary_expr(const BinaryExprNode &node,
                                        const Node &parent) {
  // Check semantic types to detect string operations.
  auto lhs_sem = semantic_type(*node.lhs);
  bool is_string = lhs_sem && lhs_sem->kind == TypeKind::String;

  // ── Struct operator overloading ────────────────────────────────────────────
  if (lhs_sem && lhs_sem->kind == TypeKind::Struct) {
    if (auto *method = struct_operator_method_of(parent))
      return emit_struct_binary_op(node, parent, lhs_sem, *method);
  }

  // ── Type matching on union types ─────────────────────────────────────
  // Pattern: `union_value == TypeName` → compare the tag byte.
  using K = Token::Kind;
  if (lhs_sem && lhs_sem->kind == TypeKind::Union &&
      (node.op == K::Equal || node.op == K::NotEqual)) {
    if (auto *rhs_ident = std::get_if<IdentifierNode>(&node.rhs->data)) {
      // Check if RHS is a type name by looking at the analyzer's symbol.
      const Symbol *rhs_sym = node_symbol(*node.rhs);
      bool is_type_sym = false;
      if (rhs_sym && rhs_sym->kind == SymbolKind::Type) {
        is_type_sym = true;
      }
      // Also check via name lookup for built-in types.
      if (!is_type_sym) {
        auto sym = analyzer.lookup(std::string(rhs_ident->name));
        if (sym && sym->kind == SymbolKind::Type)
          is_type_sym = true;
      }
      if (is_type_sym) {
        auto rhs_sem = semantic_type(*node.rhs);
        int tag = union_tag_for_type(rhs_sem, lhs_sem);
        if (tag >= 0) {
          auto *lhs_val = emit_expr(*node.lhs);
          if (!lhs_val) return nullptr;

          // lhs_val is a pointer to the union alloca.
          auto *union_st = get_union_llvm_type(lhs_sem);
          auto *tag_gep = builder.CreateStructGEP(union_st, lhs_val, 0,
                                                   "match.tag.ptr");
          auto *tag_val = builder.CreateLoad(
              llvm::Type::getInt8Ty(context), tag_gep, "match.tag");
          auto *tag_const = llvm::ConstantInt::get(
              llvm::Type::getInt8Ty(context), tag);

          if (node.op == K::Equal)
            return builder.CreateICmpEQ(tag_val, tag_const, "type.eq");
          else
            return builder.CreateICmpNE(tag_val, tag_const, "type.ne");
        }
      }
    }
  }

  // ── String operations ────────────────────────────────────────────────
  if (is_string) {
    auto *lhs = emit_expr(*node.lhs);
    auto *rhs = emit_expr(*node.rhs);
    if (!lhs || !rhs)
      return nullptr;

    using K = Token::Kind;
    switch (node.op) {
    case K::Add: {
      auto *concat_fn = module->getFunction("saga_string_concat");
      return builder.CreateCall(concat_fn, {lhs, rhs}, "concat");
    }
    case K::Equal: {
      auto *cmp_fn = module->getFunction("saga_string_compare");
      auto *cmp = builder.CreateCall(cmp_fn, {lhs, rhs}, "strcmp");
      return builder.CreateICmpEQ(cmp, llvm::ConstantInt::get(i64_type, 0),
                                  "eq");
    }
    case K::NotEqual: {
      auto *cmp_fn = module->getFunction("saga_string_compare");
      auto *cmp = builder.CreateCall(cmp_fn, {lhs, rhs}, "strcmp");
      return builder.CreateICmpNE(cmp, llvm::ConstantInt::get(i64_type, 0),
                                  "ne");
    }
    case K::LessThan: {
      auto *cmp_fn = module->getFunction("saga_string_compare");
      auto *cmp = builder.CreateCall(cmp_fn, {lhs, rhs}, "strcmp");
      return builder.CreateICmpSLT(cmp, llvm::ConstantInt::get(i64_type, 0),
                                   "lt");
    }
    case K::GreaterThan: {
      auto *cmp_fn = module->getFunction("saga_string_compare");
      auto *cmp = builder.CreateCall(cmp_fn, {lhs, rhs}, "strcmp");
      return builder.CreateICmpSGT(cmp, llvm::ConstantInt::get(i64_type, 0),
                                   "gt");
    }
    case K::LessThanEqual: {
      auto *cmp_fn = module->getFunction("saga_string_compare");
      auto *cmp = builder.CreateCall(cmp_fn, {lhs, rhs}, "strcmp");
      return builder.CreateICmpSLE(cmp, llvm::ConstantInt::get(i64_type, 0),
                                   "le");
    }
    case K::GreaterThanEqual: {
      auto *cmp_fn = module->getFunction("saga_string_compare");
      auto *cmp = builder.CreateCall(cmp_fn, {lhs, rhs}, "strcmp");
      return builder.CreateICmpSGE(cmp, llvm::ConstantInt::get(i64_type, 0),
                                   "ge");
    }
    default:
      return nullptr;
    }
  }

  // ── Numeric / bool operations ────────────────────────────────────────
  auto *lhs = emit_expr(*node.lhs);
  auto *rhs = emit_expr(*node.rhs);
  if (!lhs || !rhs)
    return nullptr;

  bool is_float = lhs->getType()->isDoubleTy() || rhs->getType()->isDoubleTy();

  // Int→Float promotion if mixed.
  if (is_float) {
    if (lhs->getType()->isIntegerTy(64))
      lhs = builder.CreateSIToFP(lhs, f64_type, "itof");
    if (rhs->getType()->isIntegerTy(64))
      rhs = builder.CreateSIToFP(rhs, f64_type, "itof");
  }

  using K = Token::Kind;

  // ── Float arithmetic ─────────────────────────────────────────────────
  if (is_float) {
    switch (node.op) {
    case K::Add:      return builder.CreateFAdd(lhs, rhs, "fadd");
    case K::Sub:      return builder.CreateFSub(lhs, rhs, "fsub");
    case K::Multiply: return builder.CreateFMul(lhs, rhs, "fmul");
    case K::Divide: {
      auto *result = builder.CreateFDiv(lhs, rhs, "fdiv");
      auto node_sem = semantic_type(parent);
      if (node_sem && node_sem->kind == TypeKind::Union) {
        auto val_t = analyzer.builtins.float_type;
        return emit_union_wrap(result, val_t, node_sem);
      }
      return result;
    }
    case K::Modulo:   return builder.CreateFRem(lhs, rhs, "fmod");
    default: break;
    }
  }

  switch (node.op) {
  // ── Integer arithmetic ─────────────────────────────────────────────
  case K::Add:      return builder.CreateAdd(lhs, rhs, "add");
  case K::Sub:      return builder.CreateSub(lhs, rhs, "sub");
  case K::Multiply: return builder.CreateMul(lhs, rhs, "mul");
  case K::Divide: {
    auto *result = builder.CreateSDiv(lhs, rhs, "div");
    auto node_sem = semantic_type(parent);
    if (node_sem && node_sem->kind == TypeKind::Union) {
      auto val_t = analyzer.builtins.int_type;
      return emit_union_wrap(result, val_t, node_sem);
    }
    return result;
  }
  case K::Modulo:   return builder.CreateSRem(lhs, rhs, "mod");
  case K::Pow: {
    // TODO: proper pow intrinsic.
    return llvm::ConstantInt::get(i64_type, 0);
  }

  // ── Comparison ─────────────────────────────────────────────────────
  case K::Equal:
    return is_float ? builder.CreateFCmpOEQ(lhs, rhs, "eq")
                    : builder.CreateICmpEQ(lhs, rhs, "eq");
  case K::NotEqual:
    return is_float ? builder.CreateFCmpONE(lhs, rhs, "ne")
                    : builder.CreateICmpNE(lhs, rhs, "ne");
  case K::LessThan:
    return is_float ? builder.CreateFCmpOLT(lhs, rhs, "lt")
                    : builder.CreateICmpSLT(lhs, rhs, "lt");
  case K::LessThanEqual:
    return is_float ? builder.CreateFCmpOLE(lhs, rhs, "le")
                    : builder.CreateICmpSLE(lhs, rhs, "le");
  case K::GreaterThan:
    return is_float ? builder.CreateFCmpOGT(lhs, rhs, "gt")
                    : builder.CreateICmpSGT(lhs, rhs, "gt");
  case K::GreaterThanEqual:
    return is_float ? builder.CreateFCmpOGE(lhs, rhs, "ge")
                    : builder.CreateICmpSGE(lhs, rhs, "ge");

  // ── Logical ────────────────────────────────────────────────────────
  case K::LogicalAnd: return builder.CreateAnd(lhs, rhs, "and");
  case K::LogicalOr:  return builder.CreateOr(lhs, rhs, "or");

  // ── Bitwise ────────────────────────────────────────────────────────
  case K::BitwiseAnd: return builder.CreateAnd(lhs, rhs, "band");
  case K::BitwiseOr:  return builder.CreateOr(lhs, rhs, "bor");
  case K::BitwiseXor: return builder.CreateXor(lhs, rhs, "bxor");
  case K::LeftShift:  return builder.CreateShl(lhs, rhs, "shl");
  case K::RightShift: return builder.CreateAShr(lhs, rhs, "shr");

  default:
    return nullptr;
  }
}

// ===========================================================================
// Unary expressions
// ===========================================================================

llvm::Value *CodeGen::emit_unary_expr(const UnaryExprNode &node) {
  auto *operand = emit_expr(*node.operand);
  if (!operand)
    return nullptr;

  using K = Token::Kind;
  switch (node.op) {
  case K::Sub:
    if (operand->getType()->isDoubleTy())
      return builder.CreateFNeg(operand, "fneg");
    return builder.CreateNeg(operand, "neg");
  case K::Not:
    return builder.CreateNot(operand, "not");
  case K::BitwiseNot:
    return builder.CreateNot(operand, "bnot");
  default:
    return nullptr;
  }
}


} // namespace mc
