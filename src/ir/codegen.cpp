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
  // mc_string = { i8*, i64 }
  // This matches the C runtime struct:
  //   typedef struct { const char *data; int64_t len; } mc_string;
  string_type = llvm::StructType::create(
      context,
      {llvm::PointerType::getUnqual(context),
       llvm::Type::getInt64Ty(context)},
      "mc_string");
}

void CodeGen::declare_runtime() {
  // void mc_intrinsic_print(mc_string* s)
  auto *print_type = llvm::FunctionType::get(
      llvm::Type::getVoidTy(context),
      {llvm::PointerType::getUnqual(context)},
      /*isVarArg=*/false);
  llvm::Function::Create(print_type, llvm::Function::ExternalLinkage,
                         "mc_intrinsic_print", module.get());
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
  for (auto &src : pkg.sources) {
    emit_source(std::get<SourceNode>(src->data));
  }
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

  // Return type: Main → i32 (C ABI), others → void (for now).
  llvm::Type *ret_type = is_main ? llvm::Type::getInt32Ty(context)
                                 : llvm::Type::getVoidTy(context);

  auto *fn_type = llvm::FunctionType::get(ret_type, /*isVarArg=*/false);
  std::string link_name = is_main ? "main" : name;

  auto *func = llvm::Function::Create(
      fn_type, llvm::Function::ExternalLinkage, link_name, module.get());

  auto *entry = llvm::BasicBlock::Create(context, "entry", func);
  builder.SetInsertPoint(entry);

  // Emit the function body.
  auto &block = std::get<BlockNode>(fn.body->data);
  emit_block(block);

  // If the block didn't already terminate, add the implicit return.
  if (!builder.GetInsertBlock()->getTerminator()) {
    if (is_main) {
      builder.CreateRet(
          llvm::ConstantInt::get(llvm::Type::getInt32Ty(context), 0));
    } else {
      builder.CreateRetVoid();
    }
  }

  llvm::verifyFunction(*func);
}

// ===========================================================================
// Block / statement emission
// ===========================================================================

void CodeGen::emit_block(const BlockNode &block) {
  for (auto &stmt : block.stmts) {
    emit_stmt(*stmt);
  }
}

void CodeGen::emit_stmt(const Node &node) {
  // For now, every statement is just an expression evaluated for side effects.
  emit_expr(node);
}

// ===========================================================================
// Expression emission
// ===========================================================================

llvm::Value *CodeGen::emit_expr(const Node &node) {
  return std::visit(
      overloaded{
          [&](const CallExprNode &n) -> llvm::Value * {
            return emit_call_expr(n);
          },
          [&](const StringLiteralNode &n) -> llvm::Value * {
            return emit_string_literal(n);
          },
          [&](const IdentifierNode &n) -> llvm::Value * {
            return emit_identifier(n);
          },
          [&](const auto &) -> llvm::Value * {
            return nullptr; // Not yet implemented.
          },
      },
      node.data);
}

// ===========================================================================
// String literals
// ===========================================================================

/// Process a raw string fragment (as stored by the lexer) into its actual
/// content: strip surrounding quotes and handle escape sequences.
static std::string unescape_fragment(std::string_view raw) {
  // Strip surrounding double quotes if present.
  if (raw.size() >= 2 && raw.front() == '"' && raw.back() == '"') {
    raw = raw.substr(1, raw.size() - 2);
  } else if (raw.size() >= 1 && raw.front() == '"') {
    // StringStart: "text{  — strip leading quote only.
    raw = raw.substr(1);
  } else if (raw.size() >= 1 && raw.back() == '"') {
    // StringEnd: }text"  — strip trailing quote only.
    raw = raw.substr(0, raw.size() - 1);
  }
  // Strip leading } from StringEnd / StringMiddle fragments.
  if (raw.size() >= 1 && raw.front() == '}') {
    raw = raw.substr(1);
  }
  // Strip trailing { from StringStart / StringMiddle fragments.
  if (raw.size() >= 1 && raw.back() == '{') {
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

llvm::Value *CodeGen::emit_string_literal(const StringLiteralNode &node) {
  // For Slice 2 we only handle plain strings (no interpolation).
  // Concatenate all StringFragmentNode texts.
  std::string text;
  for (auto &frag : node.fragments) {
    if (auto *sf = std::get_if<StringFragmentNode>(&frag->data)) {
      text += unescape_fragment(sf->text);
    }
  }
  return make_string_constant(text);
}

llvm::Value *CodeGen::make_string_constant(const std::string &text) {
  // Deduplicate: return existing global if we've seen this string before.
  auto it = string_constants.find(text);
  if (it != string_constants.end())
    return it->second;

  // Create a global constant for the raw character data.
  auto *char_array = llvm::ConstantDataArray::getString(context, text,
                                                        /*AddNull=*/false);
  auto *raw_global = new llvm::GlobalVariable(
      *module, char_array->getType(), /*isConstant=*/true,
      llvm::GlobalValue::PrivateLinkage, char_array, ".str");
  raw_global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
  raw_global->setAlignment(llvm::Align(1));

  // Build the mc_string struct constant: { ptr to data, i64 length }.
  auto *data_ptr = llvm::ConstantExpr::getInBoundsGetElementPtr(
      char_array->getType(), raw_global,
      llvm::ArrayRef<llvm::Constant *>{
          llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0),
          llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), 0)});
  auto *length =
      llvm::ConstantInt::get(llvm::Type::getInt64Ty(context), text.size());

  auto *str_const = llvm::ConstantStruct::get(string_type, {data_ptr, length});

  // Create a global for the mc_string struct itself.
  auto *str_global = new llvm::GlobalVariable(
      *module, string_type, /*isConstant=*/true,
      llvm::GlobalValue::PrivateLinkage, str_const, ".mc_str");
  str_global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);

  string_constants[text] = str_global;
  return str_global;
}

// ===========================================================================
// Call expressions
// ===========================================================================

llvm::Value *CodeGen::emit_call_expr(const CallExprNode &node) {
  // Resolve the callee.  For now we only handle direct identifier calls.
  auto *ident = std::get_if<IdentifierNode>(&node.callee->data);
  if (!ident)
    return nullptr;

  std::string name(ident->name);

  // Map language-level intrinsic names to their runtime link symbols.
  std::string link_name;
  if (name == "intrinsic_print") {
    link_name = "mc_intrinsic_print";
  } else {
    link_name = name;
  }

  auto *callee = module->getFunction(link_name);
  if (!callee)
    return nullptr;

  // Emit arguments.
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

llvm::Value *CodeGen::emit_identifier(const IdentifierNode &) {
  // Identifiers as values (variable loads, etc.) are deferred to later
  // slices.  For Slice 2, identifiers only appear as call targets, which
  // are handled directly in emit_call_expr.
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
