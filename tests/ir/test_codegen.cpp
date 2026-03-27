// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "frontend/file.hpp"
#include "frontend/fileset.hpp"
#include "frontend/parser.hpp"
#include "ir/codegen.hpp"
#include "semantic/analyzer.hpp"

#include <gtest/gtest.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

namespace mc {

// ---------------------------------------------------------------------------
// Helper — parse + analyze + codegen in one step.
// ---------------------------------------------------------------------------

struct CG {
  FileSet fileset;
  NodePtr ast;
  std::unique_ptr<Analyzer> analyzer;
  std::unique_ptr<CodeGen> codegen;

  static CG from(const std::string &source) {
    CG r;
    r.fileset.add_file(File::from_source("test.sg", source));
    Parser parser(r.fileset);
    r.ast = parser.parse();
    EXPECT_NE(r.ast, nullptr);
    EXPECT_TRUE(parser.errors.errors.empty());

    r.analyzer = std::make_unique<Analyzer>(r.fileset);
    r.analyzer->analyze(*r.ast);
    EXPECT_TRUE(r.analyzer->errors.errors.empty());

    r.codegen = std::make_unique<CodeGen>("test", *r.analyzer);
    r.codegen->emit(*r.ast);
    return r;
  }

  llvm::Module &mod() { return *codegen->module; }

  llvm::Function *func(const std::string &name) {
    return codegen->module->getFunction(name);
  }
};

// ===========================================================================
// Slice 1 — empty Main function
// ===========================================================================

TEST(CodeGen, MainFunctionExists) {
  auto r = CG::from("pub fn Main() Void {}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_EQ(main->getLinkage(), llvm::Function::ExternalLinkage);
}

TEST(CodeGen, MainReturnsI32Zero) {
  auto r = CG::from("pub fn Main() Void {}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_EQ(main->size(), 1u);
  EXPECT_TRUE(main->getReturnType()->isIntegerTy(32));
  auto &entry = main->getEntryBlock();
  auto *term = entry.getTerminator();
  ASSERT_NE(term, nullptr);
  EXPECT_TRUE(llvm::isa<llvm::ReturnInst>(term));
}

TEST(CodeGen, NonMainVoidFunc) {
  auto r = CG::from(
      "fn helper() Void {}\n"
      "pub fn Main() Void {}");
  auto *helper = r.func("helper");
  ASSERT_NE(helper, nullptr);
  EXPECT_TRUE(helper->getReturnType()->isVoidTy());
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(main->getReturnType()->isIntegerTy(32));
}

TEST(CodeGen, MainLinkerName) {
  auto r = CG::from("pub fn Main() Void {}");
  EXPECT_EQ(r.func("Main"), nullptr) << "Should not have symbol 'Main'";
  EXPECT_NE(r.func("main"), nullptr) << "Should have symbol 'main'";
}

TEST(CodeGen, ModuleHasCorrectName) {
  auto r = CG::from("pub fn Main() Void {}");
  EXPECT_EQ(r.mod().getName(), "test");
}

// ===========================================================================
// Slice 2 — string constants and intrinsic_print
// ===========================================================================

TEST(CodeGen, StringTypeExists) {
  auto r = CG::from("pub fn Main() Void {}");
  auto *st = llvm::StructType::getTypeByName(r.mod().getContext(), "mc_string");
  ASSERT_NE(st, nullptr);
  EXPECT_EQ(st->getNumElements(), 2u);
}

TEST(CodeGen, RuntimePrintDeclared) {
  auto r = CG::from("pub fn Main() Void {}");
  auto *print_fn = r.func("mc_intrinsic_print");
  ASSERT_NE(print_fn, nullptr);
  EXPECT_TRUE(print_fn->isDeclaration()) << "Should be extern (no body)";
  EXPECT_TRUE(print_fn->getReturnType()->isVoidTy());
  EXPECT_EQ(print_fn->arg_size(), 1u);
}

TEST(CodeGen, StringConstantEmitted) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  intrinsic_print(\"hello\")\n"
      "}");
  // There should be a global named .mc_str.
  auto *g = r.mod().getNamedGlobal(".mc_str");
  ASSERT_NE(g, nullptr);
  EXPECT_TRUE(g->isConstant());
}

TEST(CodeGen, PrintCallEmitted) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  intrinsic_print(\"hello\")\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Walk instructions to find a call to mc_intrinsic_print.
  bool found_call = false;
  for (auto &bb : *main) {
    for (auto &inst : bb) {
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        auto *callee = call->getCalledFunction();
        if (callee && callee->getName() == "mc_intrinsic_print") {
          found_call = true;
          EXPECT_EQ(call->arg_size(), 1u);
        }
      }
    }
  }
  EXPECT_TRUE(found_call) << "Expected a call to mc_intrinsic_print";
}

TEST(CodeGen, EscapeSequencesInString) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  intrinsic_print(\"a\\nb\\tc\")\n"
      "}");
  // The raw data global should contain actual newline/tab bytes.
  auto *raw = r.mod().getNamedGlobal(".str");
  ASSERT_NE(raw, nullptr);
  auto *init = raw->getInitializer();
  ASSERT_NE(init, nullptr);
  auto *cda = llvm::dyn_cast<llvm::ConstantDataArray>(init);
  ASSERT_NE(cda, nullptr);
  std::string data = cda->getRawDataValues().str();
  EXPECT_NE(data.find('\n'), std::string::npos) << "Should contain newline";
  EXPECT_NE(data.find('\t'), std::string::npos) << "Should contain tab";
}

TEST(CodeGen, EmptyStringConstant) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  intrinsic_print(\"\")\n"
      "}");
  // Should still emit a valid call without crashing.
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(main->size() > 0);
}

TEST(CodeGen, MultiplePrintCalls) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  intrinsic_print(\"one\")\n"
      "  intrinsic_print(\"two\")\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  int call_count = 0;
  for (auto &bb : *main) {
    for (auto &inst : bb) {
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "mc_intrinsic_print")
          call_count++;
      }
    }
  }
  EXPECT_EQ(call_count, 2);
}

TEST(CodeGen, DuplicateStringsDeduplicated) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  intrinsic_print(\"same\")\n"
      "  intrinsic_print(\"same\")\n"
      "}");
  // Both calls should reference the same global.
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  std::vector<llvm::Value *> args;
  for (auto &bb : *main) {
    for (auto &inst : bb) {
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "mc_intrinsic_print") {
          args.push_back(call->getArgOperand(0));
        }
      }
    }
  }
  ASSERT_EQ(args.size(), 2u);
  EXPECT_EQ(args[0], args[1]) << "Identical strings should share a global";
}

} // namespace mc
