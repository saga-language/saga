// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Intrinsics.h>

#include <charconv>

namespace saga {

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
          [&](const NullLiteralNode &n) -> llvm::Value * {
            return emit_null_literal(n);
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
          [&](const IsExpr &n) -> llvm::Value * { return emit_is_expr(n); },
          [&](const GroupExprNode &n) -> llvm::Value * {
            return emit_group_expr(n);
          },
          [&](const IfExprNode &n) -> llvm::Value * {
            return emit_if_expr(n, node);
          },
          [&](const ForExprNode &n) -> llvm::Value * {
            return emit_for_expr(n, node);
          },
          [&](const SwitchExprNode &n) -> llvm::Value * {
            return emit_switch_expr(n);
          },
          [&](const StructLiteralNode &n) -> llvm::Value * {
            return emit_struct_literal(n, node);
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
          [&](const BreakNode &n) -> llvm::Value * {
            if (loop_stack.empty()) return nullptr;
            auto &frame = loop_stack.back();
            // `break <value>` inside a for-expression typed `T | Error`:
            // wrap with ok tag, store to the pre-allocated union slot,
            // then branch to break_bb.  Without this the value is lost.
            if (frame.result_alloca && !n.values.empty() &&
                frame.result_value_type && frame.result_union_type) {
              auto *val = emit_expr(*n.values[0]);
              if (val) {
                auto *wrapped = emit_union_wrap(val, frame.result_value_type,
                                                frame.result_union_type);
                if (wrapped) {
                  auto *union_st =
                      get_union_llvm_type(frame.result_union_type);
                  auto sz =
                      module->getDataLayout().getTypeAllocSize(union_st);
                  auto al =
                      module->getDataLayout().getABITypeAlign(union_st);
                  builder.CreateMemCpy(frame.result_alloca, al, wrapped,
                                       al, sz);
                }
              }
            }
            builder.CreateBr(frame.break_bb);
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

// `void` (its single value, `null`) has no data. It carries a 1-byte placeholder
// so it is a valid SSA value; in a `T | void` union the payload store is skipped
// (emit_union_wrap keys on the semantic void type), and a standalone `void`
// variable stores this byte.
llvm::Value *CodeGen::emit_null_literal(const NullLiteralNode &) {
  return llvm::ConstantInt::get(llvm::Type::getInt8Ty(context), 0);
}

// ===========================================================================
// String literals
// ===========================================================================

std::string CodeGen::unescape_fragment(std::string_view raw) {
  // A fragment is either a fully-quoted StringLiteral ("..."), or one of
  // the interpolation pieces (StringStart "..{, StringMiddle }..{,
  // StringEnd }..").  Only the partial pieces carry the unquoted `{`/`}`
  // interpolation delimiters; stripping them from a fully-quoted literal
  // would chew off escaped braces like `"\{"`.
  bool is_literal =
      raw.size() >= 2 && raw.front() == '"' && raw.back() == '"';
  if (is_literal) {
    raw = raw.substr(1, raw.size() - 2);
  } else if (raw.size() >= 1 && raw.front() == '"') {
    raw = raw.substr(1);
    if (raw.size() >= 1 && raw.back() == '{')
      raw = raw.substr(0, raw.size() - 1);
  } else if (raw.size() >= 1 && raw.back() == '"') {
    raw = raw.substr(0, raw.size() - 1);
    if (raw.size() >= 1 && raw.front() == '}')
      raw = raw.substr(1);
  } else {
    if (raw.size() >= 1 && raw.front() == '}')
      raw = raw.substr(1);
    if (raw.size() >= 1 && raw.back() == '{')
      raw = raw.substr(0, raw.size() - 1);
  }

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

/// Convert an LLVM value to an saga_runtime_string* based on its semantic type.
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
      llvm::GlobalValue::PrivateLinkage, str_const, ".saga_runtime_str");
  str_global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

  string_constants[text] = str_global;
  return str_global;
}

// ===========================================================================
// Semantic type query
// ===========================================================================

TypePtr CodeGen::semantic_type(const Node &node) const {
  // Structural aliases are transparent (no methods, same ABI), so unwrap them
  // here: every codegen decision that branches on a type's kind (union, struct,
  // …) then sees through a `type U = A | B` name. Nominal aliases are kept.
  if (current_instantiation_) {
    auto it = current_instantiation_->node_types.find(&node);
    if (it != current_instantiation_->node_types.end())
      return unwrap_structural_alias(it->second);
  }
  auto it = analyzer.node_types.find(&node);
  if (it != analyzer.node_types.end())
    return unwrap_structural_alias(it->second);
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
          make_union_type({lhs_sem, analyzer.builtins.error_base});
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

llvm::Value *CodeGen::emit_float_pow(llvm::Value *base, llvm::Value *exp) {
  auto *intrin = llvm::Intrinsic::getDeclaration(
      module.get(), llvm::Intrinsic::pow, {f64_type});
  return builder.CreateCall(intrin, {base, exp}, "pow");
}

llvm::Value *CodeGen::emit_int_pow(llvm::Value *base, llvm::Value *exp) {
  auto *func = builder.GetInsertBlock()->getParent();
  auto *result = create_entry_alloca(func, "pow.result", i64_type);
  auto *idx = create_entry_alloca(func, "pow.i", i64_type);
  builder.CreateStore(llvm::ConstantInt::get(i64_type, 1), result);
  builder.CreateStore(llvm::ConstantInt::get(i64_type, 0), idx);

  auto *cond_bb = llvm::BasicBlock::Create(context, "pow.cond", func);
  auto *body_bb = llvm::BasicBlock::Create(context, "pow.body", func);
  auto *end_bb = llvm::BasicBlock::Create(context, "pow.end", func);

  builder.CreateBr(cond_bb);
  builder.SetInsertPoint(cond_bb);
  auto *i_val = builder.CreateLoad(i64_type, idx, "pow.i.val");
  auto *done = builder.CreateICmpSGE(i_val, exp, "pow.done");
  builder.CreateCondBr(done, end_bb, body_bb);

  builder.SetInsertPoint(body_bb);
  auto *r_val = builder.CreateLoad(i64_type, result, "pow.r.val");
  builder.CreateStore(builder.CreateMul(r_val, base, "pow.next.r"), result);
  builder.CreateStore(
      builder.CreateAdd(i_val, llvm::ConstantInt::get(i64_type, 1),
                        "pow.next.i"),
      idx);
  builder.CreateBr(cond_bb);

  builder.SetInsertPoint(end_bb);
  return builder.CreateLoad(i64_type, result, "pow.val");
}

llvm::Value *CodeGen::emit_binary_expr(const BinaryExprNode &node,
                                        const Node &parent) {
  // Check semantic types to detect string operations.
  auto lhs_sem = semantic_type(*node.lhs);
  bool is_string = lhs_sem && lhs_sem->kind == TypeKind::String;

  // Errors compare structurally (same type_id, equal fields) — see analyzer.
  if ((node.op == Token::Kind::Equal || node.op == Token::Kind::NotEqual) &&
      lhs_sem && is_error_valued(lhs_sem)) {
    auto rhs_sem = semantic_type(*node.rhs);
    if (rhs_sem && is_error_valued(rhs_sem))
      return emit_error_equality(node, lhs_sem, rhs_sem);
  }

  // ── Struct operator overloading ────────────────────────────────────────────
  if (lhs_sem && lhs_sem->kind == TypeKind::Struct) {
    if (auto *method = struct_operator_method_of(parent))
      return emit_struct_binary_op(node, parent, lhs_sem, *method);
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
  case K::Pow:
    return is_float ? emit_float_pow(lhs, rhs) : emit_int_pow(lhs, rhs);

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

// emit_is_expr — `value is Type`. For a union operand, load and compare the tag
// byte (the analyzer guarantees Type is an alternative). For any other operand
// the result is a compile-time constant: true iff the static type matches.
llvm::Value *CodeGen::emit_is_expr(const IsExpr &node) {
  auto value_sem = semantic_type(*node.value);
  auto test_sem = semantic_type(*node.type);
  auto *i1 = llvm::Type::getInt1Ty(context);

  if (value_sem && is_error_valued(value_sem) && test_sem &&
      is_error_valued(test_sem))
    return emit_error_is(node, value_sem, test_sem);

  if (value_sem && value_sem->kind == TypeKind::Union)
    return emit_union_is(node, value_sem, test_sem);

  emit_expr(*node.value); // evaluate for side effects, then fold
  bool same = value_sem && test_sem && types_equal(value_sem, test_sem);
  return llvm::ConstantInt::get(i1, same ? 1 : 0);
}

// `err is SomeError` on an error-typed value. Testing against base `error` is
// true for any error; a concrete-typed value folds statically; a base-`error`
// value compares the box's runtime type_id (offset 0) against the tested type.
llvm::Value *CodeGen::emit_error_is(const IsExpr &node, const TypePtr &value_sem,
                                    const TypePtr &test_sem) {
  auto *i1 = llvm::Type::getInt1Ty(context);
  auto *box = emit_expr(*node.value);
  if (!box)
    return nullptr;
  if (is_abstract_error(test_sem))
    return llvm::ConstantInt::get(i1, 1);
  if (!is_abstract_error(value_sem))
    return llvm::ConstantInt::get(i1,
                                  types_equal(value_sem, test_sem) ? 1 : 0);
  auto &tinfo = std::get<StructTypeInfo>(unwrap_alias(test_sem)->detail);
  return builder.CreateICmpEQ(
      builder.CreateLoad(i64_type, box, "is.tid"),
      llvm::ConstantInt::get(i64_type, error_type_id(tinfo)), "is.type");
}

// `x is T` on a union value: the tag identifies the alternative. When the
// matched slot is the base-`error` slot but T is a concrete error, also compare
// the boxed type_id so `x is NetworkError` distinguishes it from other errors.
llvm::Value *CodeGen::emit_union_is(const IsExpr &node, const TypePtr &value_sem,
                                    const TypePtr &test_sem) {
  auto *i1 = llvm::Type::getInt1Ty(context);
  auto *i8 = llvm::Type::getInt8Ty(context);
  int tag = union_tag_for_type(test_sem, value_sem);
  auto *union_val = emit_expr(*node.value);
  if (!union_val)
    return nullptr;
  if (tag < 0)
    return llvm::ConstantInt::get(i1, 0);

  auto *union_st = get_union_llvm_type(value_sem);
  auto *tag_gep = builder.CreateStructGEP(union_st, union_val, 0, "is.tag.ptr");
  auto *tag_ok = builder.CreateICmpEQ(
      builder.CreateLoad(i8, tag_gep, "is.tag"),
      llvm::ConstantInt::get(i8, tag), "is.eq");

  auto &info = std::get<UnionTypeInfo>(value_sem->detail);
  bool concrete_in_error_slot = is_abstract_error(info.alternatives[tag]) &&
                                is_error_valued(test_sem) &&
                                !is_abstract_error(test_sem);
  if (!concrete_in_error_slot)
    return tag_ok;

  auto *payload_gep =
      builder.CreateStructGEP(union_st, union_val, 1, "is.box.ptr");
  auto *box = builder.CreateLoad(llvm::PointerType::getUnqual(context),
                                 payload_gep, "is.box");
  auto &tinfo = std::get<StructTypeInfo>(unwrap_alias(test_sem)->detail);
  auto *tid_ok = builder.CreateICmpEQ(
      builder.CreateLoad(i64_type, box, "is.tid"),
      llvm::ConstantInt::get(i64_type, error_type_id(tinfo)), "is.type");
  return builder.CreateAnd(tag_ok, tid_ok, "is.slot.type");
}

// `a == b` / `a != b` on errors: equal iff same type_id and equal fields. Field
// reads use a concrete layout (from a non-abstract operand; else base `error`)
// and are guarded by the type_id check, so the layout always matches the box.
llvm::Value *CodeGen::emit_error_equality(const BinaryExprNode &node,
                                          const TypePtr &lhs_sem,
                                          const TypePtr &rhs_sem) {
  auto *a = emit_expr(*node.lhs);
  auto *b = emit_expr(*node.rhs);
  if (!a || !b)
    return nullptr;
  auto *i1 = llvm::Type::getInt1Ty(context);
  auto *func = builder.GetInsertBlock()->getParent();

  auto *tid_eq = builder.CreateICmpEQ(
      builder.CreateLoad(i64_type, a, "a.tid"),
      builder.CreateLoad(i64_type, b, "b.tid"), "err.tid.eq");

  const TypePtr &layout = is_abstract_error(lhs_sem) ? rhs_sem : lhs_sem;

  auto *entry = builder.GetInsertBlock();
  auto *cmp_bb = llvm::BasicBlock::Create(context, "err.eq.fields", func);
  auto *done_bb = llvm::BasicBlock::Create(context, "err.eq.done", func);
  builder.CreateCondBr(tid_eq, cmp_bb, done_bb);

  builder.SetInsertPoint(cmp_bb);
  auto *fields_eq = emit_error_fields_eq(a, b, layout);
  auto *cmp_end = builder.GetInsertBlock();
  builder.CreateBr(done_bb);

  builder.SetInsertPoint(done_bb);
  auto *phi = builder.CreatePHI(i1, 2, "err.eq");
  phi->addIncoming(llvm::ConstantInt::getFalse(i1), entry);
  phi->addIncoming(fields_eq, cmp_end);

  return node.op == Token::Kind::NotEqual ? builder.CreateNot(phi, "err.ne")
                                          : phi;
}

// Compare every field after type_id (message + extras) of two error boxes read
// through `layout`'s struct type. Both boxes are the same concrete type here.
llvm::Value *CodeGen::emit_error_fields_eq(llvm::Value *a, llvm::Value *b,
                                           const TypePtr &layout) {
  return emit_struct_fields_eq(a, b, layout, /*start=*/1);
}

// AND together per-field equality of two same-typed struct pointers, starting
// at field `start` (1 for an error box, skipping type_id; 0 for a plain struct).
llvm::Value *CodeGen::emit_struct_fields_eq(llvm::Value *a, llvm::Value *b,
                                            const TypePtr &struct_sem,
                                            size_t start) {
  llvm_type(struct_sem);
  auto &info = std::get<StructTypeInfo>(unwrap_alias(struct_sem)->detail);
  auto *st = struct_types.at(struct_cache_key(info));
  auto *i1 = llvm::Type::getInt1Ty(context);

  llvm::Value *acc = llvm::ConstantInt::getTrue(i1);
  for (size_t i = start; i < info.fields.size() && i < st->getNumElements();
       ++i) {
    auto *fa = builder.CreateStructGEP(st, a, i, "a.f");
    auto *fb = builder.CreateStructGEP(st, b, i, "b.f");
    acc = builder.CreateAnd(
        acc, emit_value_eq(info.fields[i].type, st->getElementType(i), fa, fb),
        "f.and");
  }
  return acc;
}

llvm::Value *CodeGen::emit_value_eq(const TypePtr &field_type,
                                    llvm::Type *field_ll, llvm::Value *a_gep,
                                    llvm::Value *b_gep) {
  auto k = field_type ? unwrap_alias(field_type)->kind : TypeKind::Int;
  if (k == TypeKind::String) {
    auto *cmp = builder.CreateCall(
        module->getFunction("saga_string_compare"),
        {builder.CreateLoad(field_ll, a_gep, "a.v"),
         builder.CreateLoad(field_ll, b_gep, "b.v")}, "f.strcmp");
    return builder.CreateICmpEQ(cmp, llvm::ConstantInt::get(i64_type, 0),
                                "f.streq");
  }
  // A nested struct-by-value field compares recursively so its own string /
  // managed fields are compared by value, not by raw bytes.
  if (k == TypeKind::Struct)
    return emit_struct_fields_eq(a_gep, b_gep, field_type, /*start=*/0);
  // Other aggregates (by-value arrays) fall back to a raw byte compare.
  if (field_ll->isAggregateType()) {
    uint64_t size = module->getDataLayout().getTypeAllocSize(field_ll);
    auto *cmp = builder.CreateCall(
        get_or_declare_memcmp(),
        {a_gep, b_gep, llvm::ConstantInt::get(i64_type, size)}, "f.memcmp");
    return builder.CreateICmpEQ(cmp, llvm::ConstantInt::get(cmp->getType(), 0),
                                "f.memeq");
  }
  auto *av = builder.CreateLoad(field_ll, a_gep, "a.v");
  auto *bv = builder.CreateLoad(field_ll, b_gep, "b.v");
  if (k == TypeKind::Float)
    return builder.CreateFCmpOEQ(av, bv, "f.feq");
  return builder.CreateICmpEQ(av, bv, "f.eq");
}

llvm::Function *CodeGen::get_or_declare_memcmp() {
  if (auto *f = module->getFunction("memcmp"))
    return f;
  auto *ft = llvm::FunctionType::get(
      llvm::Type::getInt32Ty(context),
      {llvm::PointerType::getUnqual(context),
       llvm::PointerType::getUnqual(context), i64_type},
      /*isVarArg=*/false);
  return llvm::Function::Create(ft, llvm::GlobalValue::ExternalLinkage,
                                "memcmp", module.get());
}

} // namespace saga
