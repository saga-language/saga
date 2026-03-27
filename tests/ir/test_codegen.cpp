// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "frontend/file.hpp"
#include "frontend/fileset.hpp"
#include "frontend/parser.hpp"
#include "ir/codegen.hpp"
#include "semantic/analyzer.hpp"

#include <gtest/gtest.h>
#include <llvm/IR/Function.h>
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
  // main should have external linkage (it's the entry point).
  EXPECT_EQ(main->getLinkage(), llvm::Function::ExternalLinkage);
}

TEST(CodeGen, MainReturnsI32Zero) {
  auto r = CG::from("pub fn Main() Void {}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have exactly one basic block.
  EXPECT_EQ(main->size(), 1u);
  // The return type should be i32 (C ABI).
  EXPECT_TRUE(main->getReturnType()->isIntegerTy(32));
  // The entry block should end with a ret instruction.
  auto &entry = main->getEntryBlock();
  auto *term = entry.getTerminator();
  ASSERT_NE(term, nullptr);
  EXPECT_TRUE(llvm::isa<llvm::ReturnInst>(term));
}

TEST(CodeGen, NonMainVoidFunc) {
  auto r = CG::from(
      "fn helper() Void {}\n"
      "pub fn Main() Void {}");
  // helper should exist as a void function.
  auto *helper = r.func("helper");
  ASSERT_NE(helper, nullptr);
  EXPECT_TRUE(helper->getReturnType()->isVoidTy());
  // main should still be i32.
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(main->getReturnType()->isIntegerTy(32));
}

TEST(CodeGen, MainLinkerName) {
  // The language function is "Main" but the linker symbol must be "main".
  auto r = CG::from("pub fn Main() Void {}");
  EXPECT_EQ(r.func("Main"), nullptr) << "Should not have symbol 'Main'";
  EXPECT_NE(r.func("main"), nullptr) << "Should have symbol 'main'";
}

TEST(CodeGen, ModuleHasCorrectName) {
  auto r = CG::from("pub fn Main() Void {}");
  EXPECT_EQ(r.mod().getName(), "test");
}

} // namespace mc
