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

// ===========================================================================
// Helper: count instructions of a given opcode in a function
// ===========================================================================

static int count_opcodes(llvm::Function *fn, unsigned opcode) {
  int count = 0;
  for (auto &bb : *fn)
    for (auto &inst : bb)
      if (inst.getOpcode() == opcode)
        count++;
  return count;
}

static bool has_alloca_named(llvm::Function *fn, const std::string &name) {
  for (auto &bb : *fn)
    for (auto &inst : bb)
      if (auto *a = llvm::dyn_cast<llvm::AllocaInst>(&inst))
        if (a->getName() == name)
          return true;
  return false;
}

// ===========================================================================
// Slice 3 — Integer arithmetic, variables, and control
// ===========================================================================

TEST(CodeGen, IntLiteralConstant) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 42\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(has_alloca_named(main, "x"));
}

TEST(CodeGen, IntAddition) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 10 + 32\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // LLVM will constant-fold 10+32 to 42, so we just check it compiles
  // and the variable exists.
  EXPECT_TRUE(has_alloca_named(main, "x"));
}

TEST(CodeGen, IntSubtraction) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 50 - 8\n"
      "}");
  EXPECT_TRUE(has_alloca_named(r.func("main"), "x"));
}

TEST(CodeGen, IntMultiplication) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 6 * 7\n"
      "}");
  EXPECT_TRUE(has_alloca_named(r.func("main"), "x"));
}

TEST(CodeGen, IntDivision) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 100 / 7\n"
      "}");
  EXPECT_TRUE(has_alloca_named(r.func("main"), "x"));
}

TEST(CodeGen, IntModulo) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 100 % 7\n"
      "}");
  EXPECT_TRUE(has_alloca_named(r.func("main"), "x"));
}

TEST(CodeGen, VariableLoadAndArithmetic) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  a := 10\n"
      "  b := 20\n"
      "  c := a + b\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(has_alloca_named(main, "a"));
  EXPECT_TRUE(has_alloca_named(main, "b"));
  EXPECT_TRUE(has_alloca_named(main, "c"));
  // Should have load instructions to read a and b.
  EXPECT_GE(count_opcodes(main, llvm::Instruction::Load), 2);
}

TEST(CodeGen, CompoundAddAssignment) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 10\n"
      "  x += 5\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have a load, add, and store for the compound assignment.
  EXPECT_GE(count_opcodes(main, llvm::Instruction::Load), 1);
  EXPECT_GE(count_opcodes(main, llvm::Instruction::Add), 1);
}

TEST(CodeGen, CompoundSubAssignment) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 10\n"
      "  x -= 3\n"
      "}");
  auto *main = r.func("main");
  EXPECT_GE(count_opcodes(main, llvm::Instruction::Sub), 1);
}

TEST(CodeGen, CompoundMulAssignment) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 10\n"
      "  x *= 2\n"
      "}");
  auto *main = r.func("main");
  EXPECT_GE(count_opcodes(main, llvm::Instruction::Mul), 1);
}

TEST(CodeGen, CompoundDivAssignment) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 10\n"
      "  x /= 2\n"
      "}");
  auto *main = r.func("main");
  EXPECT_GE(count_opcodes(main, llvm::Instruction::SDiv), 1);
}

TEST(CodeGen, Increment) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 0\n"
      "  x++\n"
      "}");
  auto *main = r.func("main");
  EXPECT_GE(count_opcodes(main, llvm::Instruction::Add), 1);
}

TEST(CodeGen, Decrement) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 10\n"
      "  x--\n"
      "}");
  auto *main = r.func("main");
  EXPECT_GE(count_opcodes(main, llvm::Instruction::Sub), 1);
}

TEST(CodeGen, UnaryNegation) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 5\n"
      "  y := -x\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(has_alloca_named(main, "y"));
}

TEST(CodeGen, UnaryLogicalNot) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := true\n"
      "  y := !x\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(has_alloca_named(main, "y"));
}

TEST(CodeGen, BoolLiterals) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  t := true\n"
      "  f := false\n"
      "}");
  auto *main = r.func("main");
  EXPECT_TRUE(has_alloca_named(main, "t"));
  EXPECT_TRUE(has_alloca_named(main, "f"));
}

TEST(CodeGen, ComparisonEq) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  a := 1\n"
      "  b := 2\n"
      "  x := a == b\n"
      "}");
  auto *main = r.func("main");
  EXPECT_TRUE(has_alloca_named(main, "x"));
  EXPECT_GE(count_opcodes(main, llvm::Instruction::ICmp), 1);
}

TEST(CodeGen, ComparisonLt) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  a := 1\n"
      "  b := 2\n"
      "  x := a < b\n"
      "}");
  auto *main = r.func("main");
  EXPECT_GE(count_opcodes(main, llvm::Instruction::ICmp), 1);
}

TEST(CodeGen, BitwiseOps) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  a := 0xFF & 0x0F\n"
      "  b := 0xFF | 0x0F\n"
      "  c := 0xFF ^ 0x0F\n"
      "  d := 1 << 4\n"
      "  e := 16 >> 2\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(has_alloca_named(main, "a"));
  EXPECT_TRUE(has_alloca_named(main, "e"));
}

TEST(CodeGen, GroupedExpression) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := (1 + 2) * 3\n"
      "}");
  EXPECT_TRUE(has_alloca_named(r.func("main"), "x"));
}

TEST(CodeGen, VarDeclExplicitType) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x Int = 42\n"
      "}");
  EXPECT_TRUE(has_alloca_named(r.func("main"), "x"));
}

TEST(CodeGen, VarDeclZeroInit) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x Int\n"
      "}");
  auto *main = r.func("main");
  EXPECT_TRUE(has_alloca_named(main, "x"));
  // Should store a zero value.
  EXPECT_GE(count_opcodes(main, llvm::Instruction::Store), 1);
}

TEST(CodeGen, SimpleAssignment) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 1\n"
      "  x = 2\n"
      "}");
  auto *main = r.func("main");
  // Two stores: initial := 1 and reassignment = 2.
  EXPECT_GE(count_opcodes(main, llvm::Instruction::Store), 2);
}

TEST(CodeGen, FloatLiteral) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 3.14\n"
      "}");
  auto *main = r.func("main");
  EXPECT_TRUE(has_alloca_named(main, "x"));
}

TEST(CodeGen, FloatArithmetic) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 1.5 + 2.5\n"
      "  y := 3.0 * 2.0\n"
      "}");
  auto *main = r.func("main");
  EXPECT_TRUE(has_alloca_named(main, "x"));
  EXPECT_TRUE(has_alloca_named(main, "y"));
}

TEST(CodeGen, NonMainFuncReturnType) {
  auto r = CG::from(
      "fn answer() Int { 42 }\n"
      "pub fn Main() Void {}");
  auto *fn = r.func("answer");
  ASSERT_NE(fn, nullptr);
  EXPECT_TRUE(fn->getReturnType()->isIntegerTy(64));
}

TEST(CodeGen, HexAndBinaryLiterals) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  a := 0xFF\n"
      "  b := 0b1010\n"
      "  c := 0o777\n"
      "}");
  auto *main = r.func("main");
  EXPECT_TRUE(has_alloca_named(main, "a"));
  EXPECT_TRUE(has_alloca_named(main, "b"));
  EXPECT_TRUE(has_alloca_named(main, "c"));
}

TEST(CodeGen, UnderscoresInLiterals) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 1_000_000\n"
      "}");
  EXPECT_TRUE(has_alloca_named(r.func("main"), "x"));
}

// ===========================================================================
// Slice 4 — String values, concatenation, comparisons, conversions
// ===========================================================================

TEST(CodeGen, StringVariable) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  s := \"hello\"\n"
      "  intrinsic_print(s)\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(has_alloca_named(main, "s"));
  // Should have a call to mc_intrinsic_print.
  bool found = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "mc_intrinsic_print")
          found = true;
  EXPECT_TRUE(found);
}

TEST(CodeGen, StringConcatEmitsCall) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  a := \"hello\"\n"
      "  b := \" world\"\n"
      "  c := a + b\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found_concat = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "mc_string_concat")
          found_concat = true;
  EXPECT_TRUE(found_concat) << "Expected call to mc_string_concat";
}

TEST(CodeGen, StringConcatAssignEmitsCall) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  s := \"hello\"\n"
      "  s += \" world\"\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found_concat = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "mc_string_concat")
          found_concat = true;
  EXPECT_TRUE(found_concat) << "Expected call to mc_string_concat for +=";
}

TEST(CodeGen, StringEqualityEmitsCompare) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  a := \"hello\"\n"
      "  b := \"world\"\n"
      "  x := a == b\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found_cmp = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "mc_string_compare")
          found_cmp = true;
  EXPECT_TRUE(found_cmp) << "Expected call to mc_string_compare";
}

TEST(CodeGen, StringLessThanEmitsCompare) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  a := \"abc\"\n"
      "  b := \"def\"\n"
      "  x := a < b\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found_cmp = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "mc_string_compare")
          found_cmp = true;
  EXPECT_TRUE(found_cmp);
}

TEST(CodeGen, RuntimeConversionsDeclared) {
  auto r = CG::from("pub fn Main() Void {}");
  EXPECT_NE(r.func("mc_string_concat"), nullptr);
  EXPECT_NE(r.func("mc_string_compare"), nullptr);
  EXPECT_NE(r.func("mc_int_to_string"), nullptr);
  EXPECT_NE(r.func("mc_float_to_string"), nullptr);
  EXPECT_NE(r.func("mc_bool_to_string"), nullptr);
}

TEST(CodeGen, StringNotAddedAsInteger) {
  // Verify that string + string does NOT emit an integer add instruction.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  a := \"x\"\n"
      "  b := \"y\"\n"
      "  c := a + b\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // There should be no integer add (only concat call).
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (inst.getOpcode() == llvm::Instruction::Add)
        FAIL() << "String concatenation should not use integer add";
}

TEST(CodeGen, MultipleConcatenations) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  a := \"a\" + \"b\" + \"c\"\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have two concat calls: ("a"+"b") then (result+"c").
  int concat_count = 0;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "mc_string_concat")
          concat_count++;
  EXPECT_EQ(concat_count, 2);
}

// ===========================================================================
// Slice 5 — Function parameters, calls, tail returns, forward refs
// ===========================================================================

TEST(CodeGen, FuncWithIntParams) {
  auto r = CG::from(
      "fn Add(a, b Int) Int { a + b }\n"
      "pub fn Main() Void {}");
  auto *add = r.func("Add");
  ASSERT_NE(add, nullptr);
  EXPECT_EQ(add->arg_size(), 2u);
  EXPECT_TRUE(add->getReturnType()->isIntegerTy(64));
  // Both params should be i64.
  for (auto &arg : add->args())
    EXPECT_TRUE(arg.getType()->isIntegerTy(64));
}

TEST(CodeGen, FuncParamNames) {
  auto r = CG::from(
      "fn Add(a, b Int) Int { a + b }\n"
      "pub fn Main() Void {}");
  auto *add = r.func("Add");
  ASSERT_NE(add, nullptr);
  EXPECT_EQ(add->getArg(0)->getName(), "a");
  EXPECT_EQ(add->getArg(1)->getName(), "b");
}

TEST(CodeGen, FuncWithStringParam) {
  auto r = CG::from(
      "fn Greet(name String) Void { intrinsic_print(name) }\n"
      "pub fn Main() Void {}");
  auto *greet = r.func("Greet");
  ASSERT_NE(greet, nullptr);
  EXPECT_EQ(greet->arg_size(), 1u);
  EXPECT_TRUE(greet->getReturnType()->isVoidTy());
  // String param is passed as ptr.
  EXPECT_TRUE(greet->getArg(0)->getType()->isPointerTy());
}

TEST(CodeGen, TailExpressionReturn) {
  auto r = CG::from(
      "fn Double(n Int) Int { n + n }\n"
      "pub fn Main() Void {}");
  auto *fn = r.func("Double");
  ASSERT_NE(fn, nullptr);
  // The function should end with a ret i64 (the tail expression).
  auto *term = fn->back().getTerminator();
  ASSERT_NE(term, nullptr);
  EXPECT_TRUE(llvm::isa<llvm::ReturnInst>(term));
  auto *ret = llvm::cast<llvm::ReturnInst>(term);
  EXPECT_NE(ret->getReturnValue(), nullptr);
  EXPECT_TRUE(ret->getReturnValue()->getType()->isIntegerTy(64));
}

TEST(CodeGen, CallUserFunction) {
  auto r = CG::from(
      "fn Add(a, b Int) Int { a + b }\n"
      "pub fn Main() Void {\n"
      "  x := Add(10, 32)\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "Add") {
          found = true;
          EXPECT_EQ(call->arg_size(), 2u);
        }
  EXPECT_TRUE(found) << "Expected call to Add";
}

TEST(CodeGen, NestedCalls) {
  auto r = CG::from(
      "fn Mul(x, y Int) Int { x * y }\n"
      "fn Square(n Int) Int { Mul(n, n) }\n"
      "pub fn Main() Void {\n"
      "  x := Square(5)\n"
      "}");
  auto *sq = r.func("Square");
  ASSERT_NE(sq, nullptr);
  bool found_mul = false;
  for (auto &bb : *sq)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "Mul")
          found_mul = true;
  EXPECT_TRUE(found_mul) << "Square should call Mul";
}

TEST(CodeGen, ForwardReference) {
  // Main is defined before Double.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := Double(21)\n"
      "}\n"
      "fn Double(n Int) Int { n + n }");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "Double")
          found = true;
  EXPECT_TRUE(found) << "Main should call Double via forward reference";
  // Double should also be fully defined.
  auto *dbl = r.func("Double");
  ASSERT_NE(dbl, nullptr);
  EXPECT_FALSE(dbl->isDeclaration()) << "Double should have a body";
}

TEST(CodeGen, MultipleParamTypes) {
  auto r = CG::from(
      "fn Mixed(n Int, x Float) Float { x }\n"
      "pub fn Main() Void {}");
  auto *fn = r.func("Mixed");
  ASSERT_NE(fn, nullptr);
  EXPECT_EQ(fn->arg_size(), 2u);
  EXPECT_TRUE(fn->getArg(0)->getType()->isIntegerTy(64));
  EXPECT_TRUE(fn->getArg(1)->getType()->isDoubleTy());
  EXPECT_TRUE(fn->getReturnType()->isDoubleTy());
}

TEST(CodeGen, VoidFuncNoReturnValue) {
  auto r = CG::from(
      "fn DoNothing() Void {}\n"
      "pub fn Main() Void { DoNothing() }");
  auto *fn = r.func("DoNothing");
  ASSERT_NE(fn, nullptr);
  EXPECT_TRUE(fn->getReturnType()->isVoidTy());
  EXPECT_EQ(fn->arg_size(), 0u);
}

TEST(CodeGen, ParamsCreateAllocas) {
  auto r = CG::from(
      "fn Add(a, b Int) Int { a + b }\n"
      "pub fn Main() Void {}");
  auto *add = r.func("Add");
  ASSERT_NE(add, nullptr);
  // Allocas may be named "a1", "b2" etc. because the LLVM arg already
  // has the name "a"/"b".  Just check that allocas exist.
  int alloca_count = 0;
  for (auto &bb : *add)
    for (auto &inst : bb)
      if (llvm::isa<llvm::AllocaInst>(&inst))
        alloca_count++;
  EXPECT_GE(alloca_count, 2) << "Should have allocas for both parameters";
}

TEST(CodeGen, BoolReturnType) {
  auto r = CG::from(
      "fn IsPositive(n Int) Bool { n > 0 }\n"
      "pub fn Main() Void {}");
  auto *fn = r.func("IsPositive");
  ASSERT_NE(fn, nullptr);
  EXPECT_TRUE(fn->getReturnType()->isIntegerTy(1));
}

TEST(CodeGen, CallChainResult) {
  // Add(10, Square(5)) — nested call as argument.
  auto r = CG::from(
      "fn Add(a, b Int) Int { a + b }\n"
      "fn Square(n Int) Int { n * n }\n"
      "pub fn Main() Void {\n"
      "  x := Add(10, Square(5))\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have calls to both Square and Add.
  bool found_sq = false, found_add = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        if (call->getCalledFunction()->getName() == "Square") found_sq = true;
        if (call->getCalledFunction()->getName() == "Add") found_add = true;
      }
  EXPECT_TRUE(found_sq);
  EXPECT_TRUE(found_add);
}

} // namespace mc



