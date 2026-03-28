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
  // { ptr data, i64 len, i64 refcount }
  EXPECT_EQ(st->getNumElements(), 3u);
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

// ===========================================================================
// Slice 6 — If/else branching
// ===========================================================================

TEST(CodeGen, IfWithoutElse) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 10\n"
      "  if x > 5 {\n"
      "    intrinsic_print(\"big\\n\")\n"
      "  }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have at least 3 basic blocks: entry, then, merge.
  EXPECT_GE(main->size(), 3u);
}

TEST(CodeGen, IfElseBranching) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 10\n"
      "  if x > 5 {\n"
      "    intrinsic_print(\"big\\n\")\n"
      "  } else {\n"
      "    intrinsic_print(\"small\\n\")\n"
      "  }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have at least 4 basic blocks: entry, then, else, merge.
  EXPECT_GE(main->size(), 4u);
}

TEST(CodeGen, IfExpressionWithPhi) {
  auto r = CG::from(
      "fn Max(a, b Int) Int {\n"
      "  if a > b { a } else { b }\n"
      "}\n"
      "pub fn Main() Void {}");
  auto *fn = r.func("Max");
  ASSERT_NE(fn, nullptr);
  // Should have a PHI node in the merge block.
  bool found_phi = false;
  for (auto &bb : *fn)
    for (auto &inst : bb)
      if (llvm::isa<llvm::PHINode>(&inst))
        found_phi = true;
  EXPECT_TRUE(found_phi) << "If-else expression should produce a PHI node";
}

TEST(CodeGen, IfExprReturnedAsTail) {
  auto r = CG::from(
      "fn Max(a, b Int) Int {\n"
      "  if a > b { a } else { b }\n"
      "}\n"
      "pub fn Main() Void {}");
  auto *fn = r.func("Max");
  ASSERT_NE(fn, nullptr);
  EXPECT_TRUE(fn->getReturnType()->isIntegerTy(64));
  // The merge block should have a ret.
  bool found_ret = false;
  for (auto &bb : *fn)
    if (auto *term = bb.getTerminator())
      if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(term))
        if (ret->getReturnValue() &&
            ret->getReturnValue()->getType()->isIntegerTy(64))
          found_ret = true;
  EXPECT_TRUE(found_ret);
}

TEST(CodeGen, IfStringExpression) {
  auto r = CG::from(
      "fn Label(n Int) String {\n"
      "  if n > 0 { \"pos\" } else { \"neg\" }\n"
      "}\n"
      "pub fn Main() Void {}");
  auto *fn = r.func("Label");
  ASSERT_NE(fn, nullptr);
  bool found_phi = false;
  for (auto &bb : *fn)
    for (auto &inst : bb)
      if (llvm::isa<llvm::PHINode>(&inst))
        found_phi = true;
  EXPECT_TRUE(found_phi);
}

TEST(CodeGen, IfWithEarlyReturn) {
  // if-without-else where the then block returns, code continues after.
  auto r = CG::from(
      "fn Guard(n Int) Int {\n"
      "  if n < 0 {\n"
      "    return 0\n"
      "  }\n"
      "  n\n"
      "}\n"
      "pub fn Main() Void {}");
  auto *fn = r.func("Guard");
  ASSERT_NE(fn, nullptr);
  // Should have at least 3 blocks: entry, then (with ret), merge (with ret n).
  EXPECT_GE(fn->size(), 3u);
  // Should have two ret instructions (one in then, one in merge).
  int ret_count = 0;
  for (auto &bb : *fn)
    if (auto *term = bb.getTerminator())
      if (llvm::isa<llvm::ReturnInst>(term))
        ret_count++;
  EXPECT_EQ(ret_count, 2);
}

TEST(CodeGen, IfBothBranchesReturn) {
  auto r = CG::from(
      "fn Choose(n Int) Int {\n"
      "  if n > 0 { n } else { -n }\n"
      "}\n"
      "pub fn Main() Void {}");
  auto *fn = r.func("Choose");
  ASSERT_NE(fn, nullptr);
  // The PHI result should be returned.
  bool found_phi = false;
  for (auto &bb : *fn)
    for (auto &inst : bb)
      if (llvm::isa<llvm::PHINode>(&inst))
        found_phi = true;
  EXPECT_TRUE(found_phi);
}

TEST(CodeGen, IfCondBranch) {
  // Verify the conditional branch instruction exists.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 1\n"
      "  if x > 0 {\n"
      "    intrinsic_print(\"yes\\n\")\n"
      "  }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found_br = false;
  for (auto &bb : *main)
    if (auto *term = bb.getTerminator())
      if (auto *br = llvm::dyn_cast<llvm::BranchInst>(term))
        if (br->isConditional())
          found_br = true;
  EXPECT_TRUE(found_br) << "Should have a conditional branch";
}

TEST(CodeGen, IfElseAssignVariable) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 10\n"
      "  y := if x > 5 { 1 } else { 0 }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(has_alloca_named(main, "y"));
}

// ===========================================================================
// Slice 7 — For loops (condition, C-style, infinite + break/next)
// ===========================================================================

TEST(CodeGen, ConditionLoop) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 0\n"
      "  for x < 5 {\n"
      "    x += 1\n"
      "  }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have loop blocks: entry, for.cond, for.body, for.update, for.exit.
  EXPECT_GE(main->size(), 4u);
  // Should have a conditional branch in the cond block.
  bool found_cond_br = false;
  for (auto &bb : *main)
    if (bb.getName().starts_with("for.cond"))
      if (auto *br = llvm::dyn_cast<llvm::BranchInst>(bb.getTerminator()))
        if (br->isConditional())
          found_cond_br = true;
  EXPECT_TRUE(found_cond_br);
}

TEST(CodeGen, CStyleLoop) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  for i := 0; i < 10; i++ {\n"
      "    intrinsic_print(\"x\\n\")\n"
      "  }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_GE(main->size(), 4u);
  // Should have an alloca for i.
  bool found_i = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *a = llvm::dyn_cast<llvm::AllocaInst>(&inst))
        if (a->getName() == "i")
          found_i = true;
  EXPECT_TRUE(found_i);
}

TEST(CodeGen, InfiniteLoopWithBreak) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 0\n"
      "  for {\n"
      "    if x >= 3 { break }\n"
      "    x += 1\n"
      "  }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have a for.exit block.
  bool found_exit = false;
  for (auto &bb : *main)
    if (bb.getName().starts_with("for.exit"))
      found_exit = true;
  EXPECT_TRUE(found_exit);
}

TEST(CodeGen, NextSkipsIteration) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  for i := 0; i < 5; i++ {\n"
      "    if i == 2 { next }\n"
      "    intrinsic_print(\".\\n\")\n"
      "  }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // The for.update block should exist (next jumps there).
  bool found_update = false;
  for (auto &bb : *main)
    if (bb.getName().starts_with("for.update"))
      found_update = true;
  EXPECT_TRUE(found_update);
}

TEST(CodeGen, LoopBackEdge) {
  // Verify the loop body branches back to the condition.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 0\n"
      "  for x < 3 {\n"
      "    x += 1\n"
      "  }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // for.update should branch to for.cond (back edge).
  for (auto &bb : *main) {
    if (bb.getName().starts_with("for.update")) {
      auto *br = llvm::dyn_cast<llvm::BranchInst>(bb.getTerminator());
      ASSERT_NE(br, nullptr);
      EXPECT_TRUE(br->isUnconditional());
      EXPECT_TRUE(br->getSuccessor(0)->getName().starts_with("for.cond"));
    }
  }
}

TEST(CodeGen, NestedLoops) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  for i := 0; i < 3; i++ {\n"
      "    for j := 0; j < 3; j++ {\n"
      "      intrinsic_print(\".\")\n"
      "    }\n"
      "  }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have multiple for.cond blocks.
  int cond_count = 0;
  for (auto &bb : *main)
    if (bb.getName().starts_with("for.cond"))
      cond_count++;
  EXPECT_GE(cond_count, 2) << "Nested loops should have 2 condition blocks";
}

TEST(CodeGen, LoopWithFunctionCall) {
  auto r = CG::from(
      "fn Inc(n Int) Int { n + 1 }\n"
      "pub fn Main() Void {\n"
      "  x := 0\n"
      "  for x < 3 {\n"
      "    x = Inc(x)\n"
      "  }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have a call to Inc inside the loop body.
  bool found_inc = false;
  for (auto &bb : *main)
    if (bb.getName().starts_with("for.body"))
      for (auto &inst : bb)
        if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
          if (call->getCalledFunction() &&
              call->getCalledFunction()->getName() == "Inc")
            found_inc = true;
  EXPECT_TRUE(found_inc);
}

// ===========================================================================
// Slice 8 — Structs: declaration, literal, field access, assignment
// ===========================================================================

TEST(CodeGen, StructTypeCreated) {
  auto r = CG::from(
      "struct Point { x, y Int }\n"
      "pub fn Main() Void {}");
  auto *st = llvm::StructType::getTypeByName(
      r.mod().getContext(), "mc.Point");
  ASSERT_NE(st, nullptr);
  EXPECT_EQ(st->getNumElements(), 2u);
  EXPECT_TRUE(st->getElementType(0)->isIntegerTy(64));
  EXPECT_TRUE(st->getElementType(1)->isIntegerTy(64));
}

TEST(CodeGen, StructLiteralAlloca) {
  auto r = CG::from(
      "struct Point { x, y Int }\n"
      "pub fn Main() Void {\n"
      "  p := Point{x: 1, y: 2}\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have an alloca for the struct.
  bool found = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *a = llvm::dyn_cast<llvm::AllocaInst>(&inst))
        if (a->getAllocatedType()->isStructTy())
          found = true;
  EXPECT_TRUE(found) << "Struct literal should create a struct alloca";
}

TEST(CodeGen, StructFieldGEP) {
  auto r = CG::from(
      "struct Point { x, y Int }\n"
      "pub fn Main() Void {\n"
      "  p := Point{x: 10, y: 20}\n"
      "  a := p.x\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have GEP instructions for field access.
  int gep_count = 0;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (llvm::isa<llvm::GetElementPtrInst>(&inst))
        gep_count++;
  EXPECT_GE(gep_count, 1) << "Field access should use GEP";
}

TEST(CodeGen, StructFieldWrite) {
  auto r = CG::from(
      "struct Point { x, y Int }\n"
      "pub fn Main() Void {\n"
      "  p := Point{x: 1, y: 2}\n"
      "  p.x = 42\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have stores after GEPs.
  EXPECT_GE(count_opcodes(main, llvm::Instruction::Store), 3);
}

TEST(CodeGen, StructReturnType) {
  auto r = CG::from(
      "struct Point { x, y Int }\n"
      "fn Make() Point { Point{x: 0, y: 0} }\n"
      "pub fn Main() Void {}");
  auto *fn = r.func("Make");
  ASSERT_NE(fn, nullptr);
  // Structs are returned as ptr.
  EXPECT_TRUE(fn->getReturnType()->isPointerTy());
}

TEST(CodeGen, StructMixedFieldTypes) {
  auto r = CG::from(
      "struct User {\n"
      "  name String\n"
      "  age Int\n"
      "}\n"
      "pub fn Main() Void {\n"
      "  u := User{name: \"Alice\", age: 30}\n"
      "}");
  auto *st = llvm::StructType::getTypeByName(
      r.mod().getContext(), "mc.User");
  ASSERT_NE(st, nullptr);
  EXPECT_EQ(st->getNumElements(), 2u);
  // name is ptr (string), age is i64.
  EXPECT_TRUE(st->getElementType(0)->isPointerTy());
  EXPECT_TRUE(st->getElementType(1)->isIntegerTy(64));
}

TEST(CodeGen, StructFieldReadWrite) {
  // Full round-trip: write a field then read it back.
  auto r = CG::from(
      "struct Counter { n Int }\n"
      "pub fn Main() Void {\n"
      "  c := Counter{n: 0}\n"
      "  c.n = 42\n"
      "  x := c.n\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(has_alloca_named(main, "x"));
}

TEST(CodeGen, MultipleStructTypes) {
  auto r = CG::from(
      "struct A { x Int }\n"
      "struct B { y Float }\n"
      "pub fn Main() Void {}");
  EXPECT_NE(llvm::StructType::getTypeByName(r.mod().getContext(), "mc.A"),
            nullptr);
  EXPECT_NE(llvm::StructType::getTypeByName(r.mod().getContext(), "mc.B"),
            nullptr);
}

// ===========================================================================
// Slice 9 — Arrays: literal, indexing, for-range, Size, Push
// ===========================================================================

TEST(CodeGen, ArrayLiteralCreated) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  arr := [1, 2, 3]\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should call mc_array_new.
  bool found = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "mc_array_new")
          found = true;
  EXPECT_TRUE(found);
}

TEST(CodeGen, ArrayPushCalled) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  arr := [10, 20]\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  int push_count = 0;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "mc_array_push")
          push_count++;
  EXPECT_EQ(push_count, 2);
}

TEST(CodeGen, ArrayIndexAccess) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  arr := [10, 20, 30]\n"
      "  x := arr[1]\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found_at = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "mc_array_at")
          found_at = true;
  EXPECT_TRUE(found_at);
}

TEST(CodeGen, ArraySizeMethod) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  arr := [1, 2, 3]\n"
      "  n := arr.Size()\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "mc_array_size")
          found = true;
  EXPECT_TRUE(found);
}

TEST(CodeGen, ArrayPushMethod) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  arr := [1, 2]\n"
      "  arr.Push(3)\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have 3 push calls: 2 from literal + 1 from .Push()
  int push_count = 0;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "mc_array_push")
          push_count++;
  EXPECT_EQ(push_count, 3);
}

TEST(CodeGen, ForRangeArray) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  arr := [10, 20, 30]\n"
      "  for v : arr {\n"
      "    intrinsic_print(\".\")\n"
      "  }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have for.cond and for.body blocks.
  bool found_cond = false;
  for (auto &bb : *main)
    if (bb.getName().starts_with("for.cond"))
      found_cond = true;
  EXPECT_TRUE(found_cond);
}

TEST(CodeGen, ForRangeKeyValue) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  arr := [10, 20]\n"
      "  for k, v : arr {\n"
      "    intrinsic_print(\"*\")\n"
      "  }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_GE(main->size(), 4u);
}

TEST(CodeGen, ArrayRuntimeDeclared) {
  auto r = CG::from("pub fn Main() Void {}");
  EXPECT_NE(r.func("mc_array_new"), nullptr);
  EXPECT_NE(r.func("mc_array_push"), nullptr);
  EXPECT_NE(r.func("mc_array_at"), nullptr);
  EXPECT_NE(r.func("mc_array_size"), nullptr);
}

// ===========================================================================
// Slice 10 — String interpolation
// ===========================================================================

TEST(CodeGen, PlainStringNoInterp) {
  // A plain string should NOT call mc_string_concat.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  intrinsic_print(\"hello\")\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  int concat_count = 0;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "mc_string_concat")
          concat_count++;
  EXPECT_EQ(concat_count, 0);
}

TEST(CodeGen, InterpStringConcat) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  name := \"world\"\n"
      "  intrinsic_print(\"hello {name}\")\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have at least one mc_string_concat for the interpolation.
  int concat_count = 0;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "mc_string_concat")
          concat_count++;
  EXPECT_GE(concat_count, 1);
}

TEST(CodeGen, InterpIntConversion) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 42\n"
      "  intrinsic_print(\"val={x}\")\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should call mc_int_to_string for the int interpolation.
  bool found = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "mc_int_to_string")
          found = true;
  EXPECT_TRUE(found);
}

TEST(CodeGen, InterpFloatConversion) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 3.14\n"
      "  intrinsic_print(\"pi={x}\")\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "mc_float_to_string")
          found = true;
  EXPECT_TRUE(found);
}

TEST(CodeGen, InterpBoolConversion) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := true\n"
      "  intrinsic_print(\"flag={x}\")\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "mc_bool_to_string")
          found = true;
  EXPECT_TRUE(found);
}

TEST(CodeGen, InterpExpressionInBraces) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 5\n"
      "  intrinsic_print(\"{x + x}\")\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have an add and an int_to_string.
  bool found_add = false, found_conv = false;
  for (auto &bb : *main)
    for (auto &inst : bb) {
      if (inst.getOpcode() == llvm::Instruction::Add) found_add = true;
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "mc_int_to_string")
          found_conv = true;
    }
  EXPECT_TRUE(found_add);
  EXPECT_TRUE(found_conv);
}

TEST(CodeGen, InterpMultipleParts) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  a := 1\n"
      "  b := 2\n"
      "  intrinsic_print(\"{a} and {b}\")\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have multiple concat calls (at least 3 parts: "1", " and ", "2").
  int concat_count = 0;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "mc_string_concat")
          concat_count++;
  EXPECT_GE(concat_count, 2);
}

TEST(CodeGen, InterpStringVariable) {
  // Interpolating a string variable should NOT call a conversion function.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  s := \"world\"\n"
      "  intrinsic_print(\"hello {s}\")\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found_conv = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            (call->getCalledFunction()->getName() == "mc_int_to_string" ||
             call->getCalledFunction()->getName() == "mc_float_to_string" ||
             call->getCalledFunction()->getName() == "mc_bool_to_string"))
          found_conv = true;
  EXPECT_FALSE(found_conv) << "String interp should not convert strings";
}

// ===========================================================================
// Memory management — refcounting
// ===========================================================================

TEST(CodeGen, StringRefcountField) {
  auto r = CG::from("pub fn Main() Void {}");
  auto *st = llvm::StructType::getTypeByName(
      r.mod().getContext(), "mc_string");
  ASSERT_NE(st, nullptr);
  // { ptr data, i64 len, i64 refcount }
  EXPECT_EQ(st->getNumElements(), 3u);
  EXPECT_TRUE(st->getElementType(2)->isIntegerTy(64));
}

TEST(CodeGen, StaticStringRefcountMinusOne) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  intrinsic_print(\"hello\")\n"
      "}");
  auto *g = r.mod().getNamedGlobal(".mc_str");
  ASSERT_NE(g, nullptr);
  auto *init = g->getInitializer();
  ASSERT_NE(init, nullptr);
  auto *cs = llvm::dyn_cast<llvm::ConstantStruct>(init);
  ASSERT_NE(cs, nullptr);
  // Third field (index 2) should be -1.
  auto *rc = llvm::dyn_cast<llvm::ConstantInt>(cs->getOperand(2));
  ASSERT_NE(rc, nullptr);
  EXPECT_EQ(rc->getSExtValue(), -1);
}

TEST(CodeGen, ReleaseStringAtFuncExit) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  s := \"hello\"\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "mc_release_string")
          found = true;
  EXPECT_TRUE(found) << "String locals should be released at function exit";
}

TEST(CodeGen, ReleaseArrayAtFuncExit) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  arr := [1, 2, 3]\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "mc_release_array")
          found = true;
  EXPECT_TRUE(found) << "Array locals should be released at function exit";
}

TEST(CodeGen, ReleaseOnReassignment) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  s := \"first\"\n"
      "  s = \"second\"\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have at least 2 release calls: one for reassignment, one at exit.
  int release_count = 0;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "mc_release_string")
          release_count++;
  EXPECT_GE(release_count, 2);
}

TEST(CodeGen, ReleaseOnStringConcat) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  s := \"a\"\n"
      "  s += \"b\"\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // The old value of s should be released after +=.
  int release_count = 0;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "mc_release_string")
          release_count++;
  EXPECT_GE(release_count, 2) << "Old string released on +=, final at exit";
}

TEST(CodeGen, RetainReleaseDeclared) {
  auto r = CG::from("pub fn Main() Void {}");
  EXPECT_NE(r.func("mc_retain_string"), nullptr);
  EXPECT_NE(r.func("mc_release_string"), nullptr);
  EXPECT_NE(r.func("mc_retain_array"), nullptr);
  EXPECT_NE(r.func("mc_release_array"), nullptr);
}

TEST(CodeGen, ReleaseBeforeReturn) {
  auto r = CG::from(
      "fn Greet() Void {\n"
      "  s := \"hello\"\n"
      "  intrinsic_print(s)\n"
      "}\n"
      "pub fn Main() Void { Greet() }");
  auto *fn = r.func("Greet");
  ASSERT_NE(fn, nullptr);
  bool found = false;
  for (auto &bb : *fn)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "mc_release_string")
          found = true;
  EXPECT_TRUE(found);
}

TEST(CodeGen, IntLocalNotReleased) {
  // Int locals should NOT get release calls.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 42\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            (call->getCalledFunction()->getName() == "mc_release_string" ||
             call->getCalledFunction()->getName() == "mc_release_array"))
          FAIL() << "Int locals should not be released";
}

// ===========================================================================
// Enums
// ===========================================================================

TEST(CodeGen, EnumVariantValues) {
  auto r = CG::from(
      "enum Colors { Red\n Green\n Blue }\n"
      "pub fn Main() Void {\n"
      "  r := Colors.Red\n"
      "  g := Colors.Green\n"
      "  b := Colors.Blue\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(has_alloca_named(main, "r"));
  EXPECT_TRUE(has_alloca_named(main, "g"));
  EXPECT_TRUE(has_alloca_named(main, "b"));
}

TEST(CodeGen, EnumComparison) {
  auto r = CG::from(
      "enum Colors { Red\n Green\n Blue }\n"
      "pub fn Main() Void {\n"
      "  c := Colors.Red\n"
      "  x := c == Colors.Red\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_GE(count_opcodes(main, llvm::Instruction::ICmp), 1);
}

TEST(CodeGen, EnumCustomIndex) {
  auto r = CG::from(
      "enum Suits {\n"
      "  Clubs {index: 1}\n"
      "  Diamonds\n"
      "  Hearts {index: 5}\n"
      "  Spades\n"
      "}\n"
      "pub fn Main() Void {\n"
      "  c := Suits.Clubs\n"
      "  s := Suits.Spades\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Verify by checking store values.
  // Clubs=1, Spades=6 — just check the function compiles.
  EXPECT_TRUE(has_alloca_named(main, "c"));
  EXPECT_TRUE(has_alloca_named(main, "s"));
}

TEST(CodeGen, EnumAsParam) {
  auto r = CG::from(
      "enum Dir { North\n South }\n"
      "fn Check(d Dir) Bool { d == Dir.North }\n"
      "pub fn Main() Void {}");
  auto *fn = r.func("Check");
  ASSERT_NE(fn, nullptr);
  EXPECT_EQ(fn->arg_size(), 1u);
  EXPECT_TRUE(fn->getArg(0)->getType()->isIntegerTy(64));
  EXPECT_TRUE(fn->getReturnType()->isIntegerTy(1));
}

TEST(CodeGen, EnumReturnType) {
  auto r = CG::from(
      "enum Dir { North\n South\n East\n West }\n"
      "fn Default() Dir { Dir.North }\n"
      "pub fn Main() Void {}");
  auto *fn = r.func("Default");
  ASSERT_NE(fn, nullptr);
  EXPECT_TRUE(fn->getReturnType()->isIntegerTy(64));
}

TEST(CodeGen, EnumInIf) {
  auto r = CG::from(
      "enum Colors { Red\n Green\n Blue }\n"
      "pub fn Main() Void {\n"
      "  c := Colors.Green\n"
      "  if c == Colors.Green {\n"
      "    intrinsic_print(\"green\\n\")\n"
      "  }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_GE(main->size(), 3u); // entry, then, merge
}

TEST(CodeGen, BuiltinComparisonEnum) {
  // The built-in Comparison enum should be registered.
  auto r = CG::from("pub fn Main() Void {}");
  // Check that the codegen knows about Comparison variants.
  EXPECT_TRUE(r.codegen->enum_variants.count("Comparison.Less"));
  EXPECT_TRUE(r.codegen->enum_variants.count("Comparison.Equal"));
  EXPECT_TRUE(r.codegen->enum_variants.count("Comparison.Greater"));
  EXPECT_EQ(r.codegen->enum_variants["Comparison.Less"], 0);
  EXPECT_EQ(r.codegen->enum_variants["Comparison.Equal"], 1);
  EXPECT_EQ(r.codegen->enum_variants["Comparison.Greater"], 2);
}

} // namespace mc










