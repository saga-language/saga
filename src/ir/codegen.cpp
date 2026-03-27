// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "ir/codegen.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/TargetParser/Host.h>

#include <algorithm>
#include <charconv>
#include <format>

namespace mc {

// ===========================================================================
// Construction
// ===========================================================================

CodeGen::CodeGen(const std::string &module_name, Analyzer &analyzer)
    : module(std::make_unique<llvm::Module>(module_name, context)),
      builder(context),
      analyzer(analyzer) {
  init_types();
  declare_runtime();
}

// ===========================================================================
// Type helpers
// ===========================================================================

void CodeGen::init_types() {
  i64_type = llvm::Type::getInt64Ty(context);
  f64_type = llvm::Type::getDoubleTy(context);
  i1_type = llvm::Type::getInt1Ty(context);
  void_ll_type = llvm::Type::getVoidTy(context);

  // mc_string = { i8*, i64 }
  string_type = llvm::StructType::create(
      context,
      {llvm::PointerType::getUnqual(context), i64_type},
      "mc_string");
}

void CodeGen::declare_runtime() {
  auto *ptr_type = llvm::PointerType::getUnqual(context);

  // void mc_intrinsic_print(mc_string* s)
  llvm::Function::Create(
      llvm::FunctionType::get(void_ll_type, {ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_intrinsic_print", module.get());

  // mc_string* mc_string_concat(mc_string* a, mc_string* b)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_string_concat", module.get());

  // int64_t mc_string_compare(mc_string* a, mc_string* b)
  llvm::Function::Create(
      llvm::FunctionType::get(i64_type, {ptr_type, ptr_type}, false),
      llvm::Function::ExternalLinkage, "mc_string_compare", module.get());

  // mc_string* mc_int_to_string(int64_t val)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {i64_type}, false),
      llvm::Function::ExternalLinkage, "mc_int_to_string", module.get());

  // mc_string* mc_float_to_string(double val)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {f64_type}, false),
      llvm::Function::ExternalLinkage, "mc_float_to_string", module.get());

  // mc_string* mc_bool_to_string(int64_t val)
  llvm::Function::Create(
      llvm::FunctionType::get(ptr_type, {i64_type}, false),
      llvm::Function::ExternalLinkage, "mc_bool_to_string", module.get());
}

llvm::Type *CodeGen::llvm_type(const TypePtr &t) {
  if (!t)
    return void_ll_type;

  switch (t->kind) {
  case TypeKind::Int:
    return i64_type;
  case TypeKind::Float:
    return f64_type;
  case TypeKind::Bool:
    return i1_type;
  case TypeKind::String:
    return llvm::PointerType::getUnqual(context); // ptr to mc_string
  case TypeKind::Void:
    return void_ll_type;
  default:
    return void_ll_type;
  }
}

llvm::AllocaInst *CodeGen::create_entry_alloca(llvm::Function *fn,
                                                const std::string &name,
                                                llvm::Type *type) {
  llvm::IRBuilder<> tmp_builder(&fn->getEntryBlock(),
                                fn->getEntryBlock().begin());
  return tmp_builder.CreateAlloca(type, nullptr, name);
}

// ===========================================================================
// Entry point
// ===========================================================================

void CodeGen::emit(const Node &root) {
  std::visit(
      overloaded{
          [&](const PackageNode &pkg) { emit_package(pkg); },
          [&](const SourceNode &src) { emit_source(src); },
          [&](const auto &) {},
      },
      root.data);
}

// ===========================================================================
// Top-level visitors
// ===========================================================================

void CodeGen::emit_package(const PackageNode &pkg) {
  for (auto &src : pkg.sources)
    emit_source(std::get<SourceNode>(src->data));
}

void CodeGen::emit_source(const SourceNode &src) {
  for (auto &decl : src.declarations) {
    std::visit(
        overloaded{
            [&](const FuncDeclNode &fn) { emit_func_decl(fn); },
            [&](const auto &) {},
        },
        decl->data);
  }
}

// ===========================================================================
// Function emission
// ===========================================================================

void CodeGen::emit_func_decl(const FuncDeclNode &fn) {
  std::string name(fn.name.name);
  bool is_main = (name == "Main");

  // Return type.
  llvm::Type *ret_type = void_ll_type;
  if (is_main) {
    ret_type = llvm::Type::getInt32Ty(context);
  } else if (!fn.signature.returns.empty()) {
    auto sem_type = analyzer.resolve_type(*fn.signature.returns[0]);
    ret_type = llvm_type(sem_type);
  }

  auto *fn_type = llvm::FunctionType::get(ret_type, /*isVarArg=*/false);
  std::string link_name = is_main ? "main" : name;

  auto *func = llvm::Function::Create(
      fn_type, llvm::Function::ExternalLinkage, link_name, module.get());

  auto *entry = llvm::BasicBlock::Create(context, "entry", func);
  builder.SetInsertPoint(entry);

  // Reset per-function state.
  locals.clear();
  current_func_is_main = is_main;

  // Emit body.
  auto &block = std::get<BlockNode>(fn.body->data);
  emit_block(block);

  // If the block didn't terminate, add an implicit return.
  if (!builder.GetInsertBlock()->getTerminator()) {
    if (is_main) {
      builder.CreateRet(
          llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0));
    } else if (ret_type->isVoidTy()) {
      builder.CreateRetVoid();
    } else {
      // Return zero value for the type.
      builder.CreateRet(llvm::Constant::getNullValue(ret_type));
    }
  }

  llvm::verifyFunction(*func);
}

// ===========================================================================
// Block / statement emission
// ===========================================================================

void CodeGen::emit_block(const BlockNode &block) {
  for (auto &stmt : block.stmts)
    emit_stmt(*stmt);
}

void CodeGen::emit_stmt(const Node &node) {
  std::visit(
      overloaded{
          [&](const VarDeclNode &n) { emit_var_decl(n); },
          [&](const DeclAssignNode &n) { emit_decl_assign(n); },
          [&](const AssignNode &n) { emit_assign(n); },
          [&](const ReturnNode &n) { emit_return(n); },
          [&](const IncrementNode &n) { emit_increment(n); },
          [&](const DecrementNode &n) { emit_decrement(n); },
          [&](const auto &) {
            // Everything else is an expression evaluated for side effects.
            emit_expr(node);
          },
      },
      node.data);
}

// ===========================================================================
// Statement emitters
// ===========================================================================

void CodeGen::emit_var_decl(const VarDeclNode &node) {
  std::string name(node.name.name);
  auto *func = builder.GetInsertBlock()->getParent();

  // Determine the LLVM type from the semantic type annotation or
  // the initializer's type.
  llvm::Type *var_type = i64_type; // default to Int
  if (node.type) {
    auto sem_type = analyzer.resolve_type(**node.type);
    var_type = llvm_type(sem_type);
  } else if (node.init) {
    // Infer from the init expression's semantic type.
    auto it = analyzer.node_types.find(&**node.init);
    if (it != analyzer.node_types.end())
      var_type = llvm_type(it->second);
  }

  auto *alloca = create_entry_alloca(func, name, var_type);
  locals[name] = alloca;

  if (node.init) {
    auto *val = emit_expr(**node.init);
    if (val)
      builder.CreateStore(val, alloca);
  } else {
    // Zero-initialize.
    builder.CreateStore(llvm::Constant::getNullValue(var_type), alloca);
  }
}

void CodeGen::emit_decl_assign(const DeclAssignNode &node) {
  auto *val = emit_expr(*node.value);
  auto *func = builder.GetInsertBlock()->getParent();

  for (auto &ident : node.targets.identifiers) {
    std::string name(ident.name);
    llvm::Type *var_type = val ? val->getType() : i64_type;
    auto *alloca = create_entry_alloca(func, name, var_type);
    locals[name] = alloca;
    if (val)
      builder.CreateStore(val, alloca);
  }
}

void CodeGen::emit_assign(const AssignNode &node) {
  for (size_t i = 0; i < node.targets.size() && i < node.values.size(); ++i) {
    auto *rhs = emit_expr(*node.values[i]);
    if (!rhs)
      continue;

    // Target must be an identifier (for now).
    auto *ident = std::get_if<IdentifierNode>(&node.targets[i]->data);
    if (!ident)
      continue;

    auto it = locals.find(std::string(ident->name));
    if (it == locals.end())
      continue;

    auto *alloca = it->second;

    using K = Token::Kind;
    if (node.op == K::Assignment) {
      builder.CreateStore(rhs, alloca);
    } else {
      // Compound assignment: load current, apply op, store.
      auto *cur = builder.CreateLoad(alloca->getAllocatedType(), alloca);
      llvm::Value *result = nullptr;

      // Check if this is a string compound assignment.
      auto target_sem = semantic_type(*node.targets[i]);
      bool is_str = target_sem && target_sem->kind == TypeKind::String;

      if (is_str && node.op == K::AddAssignment) {
        auto *concat_fn = module->getFunction("mc_string_concat");
        result = builder.CreateCall(concat_fn, {cur, rhs}, "concat");
      } else {
        switch (node.op) {
        case K::AddAssignment: result = builder.CreateAdd(cur, rhs, "add"); break;
        case K::SubAssignment: result = builder.CreateSub(cur, rhs, "sub"); break;
        case K::MulAssignment: result = builder.CreateMul(cur, rhs, "mul"); break;
        case K::DivAssignment: result = builder.CreateSDiv(cur, rhs, "div"); break;
        default: result = rhs; break;
        }
      }
      builder.CreateStore(result, alloca);
    }
  }
}

void CodeGen::emit_return(const ReturnNode &node) {
  if (current_func_is_main) {
    if (node.values.empty()) {
      builder.CreateRet(
          llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0));
    } else {
      auto *val = emit_expr(*node.values[0]);
      // Truncate i64 to i32 for the C main return.
      auto *i32_val = builder.CreateTrunc(val, llvm::Type::getInt32Ty(context),
                                          "main_ret");
      builder.CreateRet(i32_val);
    }
    return;
  }

  if (node.values.empty()) {
    builder.CreateRetVoid();
  } else if (node.values.size() == 1) {
    auto *val = emit_expr(*node.values[0]);
    if (val)
      builder.CreateRet(val);
    else
      builder.CreateRetVoid();
  } else {
    // Multiple return values — deferred.
    builder.CreateRetVoid();
  }
}

void CodeGen::emit_increment(const IncrementNode &node) {
  auto *ident = std::get_if<IdentifierNode>(&node.operand->data);
  if (!ident) return;
  auto it = locals.find(std::string(ident->name));
  if (it == locals.end()) return;

  auto *alloca = it->second;
  auto *cur = builder.CreateLoad(alloca->getAllocatedType(), alloca);
  auto *one = llvm::ConstantInt::get(i64_type, 1);
  auto *inc = builder.CreateAdd(cur, one, "inc");
  builder.CreateStore(inc, alloca);
}

void CodeGen::emit_decrement(const DecrementNode &node) {
  auto *ident = std::get_if<IdentifierNode>(&node.operand->data);
  if (!ident) return;
  auto it = locals.find(std::string(ident->name));
  if (it == locals.end()) return;

  auto *alloca = it->second;
  auto *cur = builder.CreateLoad(alloca->getAllocatedType(), alloca);
  auto *one = llvm::ConstantInt::get(i64_type, 1);
  auto *dec = builder.CreateSub(cur, one, "dec");
  builder.CreateStore(dec, alloca);
}

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
            return emit_binary_expr(n);
          },
          [&](const UnaryExprNode &n) -> llvm::Value * {
            return emit_unary_expr(n);
          },
          [&](const GroupExprNode &n) -> llvm::Value * {
            return emit_group_expr(n);
          },
          [&](const CallExprNode &n) -> llvm::Value * {
            return emit_call_expr(n);
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

static std::string unescape_fragment(std::string_view raw) {
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

llvm::Value *CodeGen::emit_string_literal(const StringLiteralNode &node) {
  std::string text;
  for (auto &frag : node.fragments) {
    if (auto *sf = std::get_if<StringFragmentNode>(&frag->data))
      text += unescape_fragment(sf->text);
  }
  return make_string_constant(text);
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
  auto *str_const =
      llvm::ConstantStruct::get(string_type, {data_ptr, length});

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
  auto it = analyzer.node_types.find(&node);
  if (it != analyzer.node_types.end())
    return it->second;
  return nullptr;
}

// ===========================================================================
// Binary expressions
// ===========================================================================

llvm::Value *CodeGen::emit_binary_expr(const BinaryExprNode &node) {
  // Check semantic types to detect string operations.
  auto lhs_sem = semantic_type(*node.lhs);
  bool is_string = lhs_sem && lhs_sem->kind == TypeKind::String;

  // ── String operations ────────────────────────────────────────────────
  if (is_string) {
    auto *lhs = emit_expr(*node.lhs);
    auto *rhs = emit_expr(*node.rhs);
    if (!lhs || !rhs)
      return nullptr;

    using K = Token::Kind;
    switch (node.op) {
    case K::Add: {
      auto *concat_fn = module->getFunction("mc_string_concat");
      return builder.CreateCall(concat_fn, {lhs, rhs}, "concat");
    }
    case K::Equal: {
      auto *cmp_fn = module->getFunction("mc_string_compare");
      auto *cmp = builder.CreateCall(cmp_fn, {lhs, rhs}, "strcmp");
      return builder.CreateICmpEQ(cmp, llvm::ConstantInt::get(i64_type, 0),
                                  "eq");
    }
    case K::NotEqual: {
      auto *cmp_fn = module->getFunction("mc_string_compare");
      auto *cmp = builder.CreateCall(cmp_fn, {lhs, rhs}, "strcmp");
      return builder.CreateICmpNE(cmp, llvm::ConstantInt::get(i64_type, 0),
                                  "ne");
    }
    case K::LessThan: {
      auto *cmp_fn = module->getFunction("mc_string_compare");
      auto *cmp = builder.CreateCall(cmp_fn, {lhs, rhs}, "strcmp");
      return builder.CreateICmpSLT(cmp, llvm::ConstantInt::get(i64_type, 0),
                                   "lt");
    }
    case K::GreaterThan: {
      auto *cmp_fn = module->getFunction("mc_string_compare");
      auto *cmp = builder.CreateCall(cmp_fn, {lhs, rhs}, "strcmp");
      return builder.CreateICmpSGT(cmp, llvm::ConstantInt::get(i64_type, 0),
                                   "gt");
    }
    case K::LessThanEqual: {
      auto *cmp_fn = module->getFunction("mc_string_compare");
      auto *cmp = builder.CreateCall(cmp_fn, {lhs, rhs}, "strcmp");
      return builder.CreateICmpSLE(cmp, llvm::ConstantInt::get(i64_type, 0),
                                   "le");
    }
    case K::GreaterThanEqual: {
      auto *cmp_fn = module->getFunction("mc_string_compare");
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
    case K::Divide:   return builder.CreateFDiv(lhs, rhs, "fdiv");
    case K::Modulo:   return builder.CreateFRem(lhs, rhs, "fmod");
    default: break;
    }
  }

  switch (node.op) {
  // ── Integer arithmetic ─────────────────────────────────────────────
  case K::Add:      return builder.CreateAdd(lhs, rhs, "add");
  case K::Sub:      return builder.CreateSub(lhs, rhs, "sub");
  case K::Multiply: return builder.CreateMul(lhs, rhs, "mul");
  case K::Divide:   return builder.CreateSDiv(lhs, rhs, "div");
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

// ===========================================================================
// Group expression
// ===========================================================================

llvm::Value *CodeGen::emit_group_expr(const GroupExprNode &node) {
  return emit_expr(*node.inner);
}

// ===========================================================================
// Call expressions
// ===========================================================================

llvm::Value *CodeGen::emit_call_expr(const CallExprNode &node) {
  auto *ident = std::get_if<IdentifierNode>(&node.callee->data);
  if (!ident)
    return nullptr;

  std::string name(ident->name);
  std::string link_name;
  if (name == "intrinsic_print")
    link_name = "mc_intrinsic_print";
  else
    link_name = name;

  auto *callee = module->getFunction(link_name);
  if (!callee)
    return nullptr;

  std::vector<llvm::Value *> args;
  for (auto &arg_node : node.args) {
    auto *val = emit_expr(*arg_node);
    if (val)
      args.push_back(val);
  }

  return builder.CreateCall(callee, args);
}

// ===========================================================================
// Identifier expressions
// ===========================================================================

llvm::Value *CodeGen::emit_identifier(const IdentifierNode &node) {
  std::string name(node.name);

  // Check local variables.
  auto it = locals.find(name);
  if (it != locals.end()) {
    return builder.CreateLoad(it->second->getAllocatedType(), it->second,
                              name);
  }

  // Builtin constants.
  if (name == "true")
    return llvm::ConstantInt::get(i1_type, 1);
  if (name == "false")
    return llvm::ConstantInt::get(i1_type, 0);

  return nullptr;
}

// ===========================================================================
// Output
// ===========================================================================

void CodeGen::dump() const { module->print(llvm::errs(), nullptr); }

bool CodeGen::write_ir(const std::string &path) const {
  std::error_code ec;
  llvm::raw_fd_ostream out(path, ec, llvm::sys::fs::OF_Text);
  if (ec)
    return false;
  module->print(out, nullptr);
  return true;
}

bool CodeGen::write_object(const std::string &path) {
  llvm::InitializeNativeTarget();
  llvm::InitializeNativeTargetAsmPrinter();
  llvm::InitializeNativeTargetAsmParser();

  auto target_triple = llvm::sys::getDefaultTargetTriple();
  module->setTargetTriple(target_triple);

  std::string error_str;
  auto *target =
      llvm::TargetRegistry::lookupTarget(target_triple, error_str);
  if (!target)
    return false;

  auto *target_machine = target->createTargetMachine(
      target_triple, "generic", "", llvm::TargetOptions{},
      std::nullopt);
  module->setDataLayout(target_machine->createDataLayout());

  std::error_code ec;
  llvm::raw_fd_ostream out(path, ec, llvm::sys::fs::OF_None);
  if (ec)
    return false;

  llvm::legacy::PassManager pass;
  if (target_machine->addPassesToEmitFile(
          pass, out, nullptr, llvm::CodeGenFileType::ObjectFile))
    return false;

  pass.run(*module);
  out.flush();
  return true;
}

} // namespace mc
