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

  static CG from(const std::string &source, bool stdlib = true) {
    CG r;
    r.fileset.add_file(File::from_source("test.sg", source));
    Parser parser(r.fileset);
    r.ast = parser.parse();
    EXPECT_NE(r.ast, nullptr);
    EXPECT_TRUE(parser.errors.errors.empty());

    r.analyzer = std::make_unique<Analyzer>(r.fileset);
    r.analyzer->is_stdlib = stdlib;
    r.analyzer->package_resolver->sgi_search_paths.push_back(
        SAGA_STD_SGI_DIR);
    r.analyzer->analyze(*r.ast);
    EXPECT_TRUE(r.analyzer->errors.errors.empty());

    r.codegen = std::make_unique<CodeGen>("test", *r.analyzer);
    r.codegen->emit(*r.ast);
    return r;
  }

  llvm::Module &mod() { return *codegen->module; }

  llvm::Function *func(const std::string &name) {
    // Try exact name first (e.g. "main", runtime functions).
    if (auto *fn = codegen->module->getFunction(name))
      return fn;
    // Try mangled name.
    return codegen->module->getFunction(mangled(name));
  }

  /// Mangle a name the same way CodeGen does for package "test".
  static std::string mangled(const std::string &name) {
    std::string m = "test__" + name;
    // Replace '.' with '__' for struct methods like "Dog.Speak" → "test__Dog__Speak".
    for (size_t pos = 0; (pos = m.find('.', pos)) != std::string::npos; )
      m.replace(pos, 1, "__");
    return m;
  }

  /// Check if a call instruction calls a function with the given (unmangled) name.
  static bool calls_func(llvm::CallInst *call, const std::string &name) {
    if (!call || !call->getCalledFunction())
      return false;
    auto called = call->getCalledFunction()->getName();
    return called == name || called == mangled(name);
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
  auto *print_fn = r.func("saga_intrinsic_print");
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
  // Walk instructions to find a call to saga_intrinsic_print.
  bool found_call = false;
  for (auto &bb : *main) {
    for (auto &inst : bb) {
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        auto *callee = call->getCalledFunction();
        if (callee && callee->getName() == "saga_intrinsic_print") {
          found_call = true;
          EXPECT_EQ(call->arg_size(), 1u);
        }
      }
    }
  }
  EXPECT_TRUE(found_call) << "Expected a call to saga_intrinsic_print";
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
            call->getCalledFunction()->getName() == "saga_intrinsic_print")
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
            call->getCalledFunction()->getName() == "saga_intrinsic_print") {
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
  // Should have a call to saga_intrinsic_print.
  bool found = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "saga_intrinsic_print")
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
            call->getCalledFunction()->getName() == "saga_string_concat")
          found_concat = true;
  EXPECT_TRUE(found_concat) << "Expected call to saga_string_concat";
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
            call->getCalledFunction()->getName() == "saga_string_concat")
          found_concat = true;
  EXPECT_TRUE(found_concat) << "Expected call to saga_string_concat for +=";
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
            call->getCalledFunction()->getName() == "saga_string_compare")
          found_cmp = true;
  EXPECT_TRUE(found_cmp) << "Expected call to saga_string_compare";
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
            call->getCalledFunction()->getName() == "saga_string_compare")
          found_cmp = true;
  EXPECT_TRUE(found_cmp);
}

TEST(CodeGen, RuntimeConversionsDeclared) {
  auto r = CG::from("pub fn Main() Void {}");
  EXPECT_NE(r.func("saga_string_concat"), nullptr);
  EXPECT_NE(r.func("saga_string_compare"), nullptr);
  EXPECT_NE(r.func("saga_int_to_string"), nullptr);
  EXPECT_NE(r.func("saga_float_to_string"), nullptr);
  EXPECT_NE(r.func("saga_bool_to_string"), nullptr);
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
            call->getCalledFunction()->getName() == "saga_string_concat")
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
            CG::calls_func(call, "Add")) {
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
            CG::calls_func(call, "Mul"))
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
            CG::calls_func(call, "Double"))
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
        if (CG::calls_func(call, "Square")) found_sq = true;
        if (CG::calls_func(call, "Add")) found_add = true;
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
              CG::calls_func(call, "Inc"))
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
  // Should call saga_array_new.
  bool found = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "saga_array_new")
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
            call->getCalledFunction()->getName() == "saga_array_push")
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
            call->getCalledFunction()->getName() == "saga_array_at")
          found_at = true;
  EXPECT_TRUE(found_at);
}

TEST(CodeGen, ArraySizeMethod) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  arr := [1, 2, 3]\n"
      "  n := arr.Size()\n"
      "}", false);
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "array__Array__Size")
          found = true;
  EXPECT_TRUE(found);
}

TEST(CodeGen, ArrayPushMethod) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  arr := [1, 2]\n"
      "  arr.Push(3)\n"
      "}", false);
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // 2 saga_array_push from literal construction + 1 array__Array__Push from method call.
  int literal_push = 0;
  int method_push = 0;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction()) {
          if (call->getCalledFunction()->getName() == "saga_array_push")
            literal_push++;
          if (call->getCalledFunction()->getName() == "array__Array__Push")
            method_push++;
        }
  EXPECT_EQ(literal_push, 2);
  EXPECT_EQ(method_push, 1);
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
  EXPECT_NE(r.func("saga_array_new"), nullptr);
  EXPECT_NE(r.func("saga_array_push"), nullptr);
  EXPECT_NE(r.func("saga_array_at"), nullptr);
  EXPECT_NE(r.func("saga_array_size"), nullptr);
}

// ===========================================================================
// Slice 10 — String interpolation
// ===========================================================================

TEST(CodeGen, PlainStringNoInterp) {
  // A plain string should NOT call saga_string_concat.
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
            call->getCalledFunction()->getName() == "saga_string_concat")
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
  // Should have at least one saga_string_concat for the interpolation.
  int concat_count = 0;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "saga_string_concat")
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
  // Should call saga_int_to_string for the int interpolation.
  bool found = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "saga_int_to_string")
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
            call->getCalledFunction()->getName() == "saga_float_to_string")
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
            call->getCalledFunction()->getName() == "saga_bool_to_string")
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
            call->getCalledFunction()->getName() == "saga_int_to_string")
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
            call->getCalledFunction()->getName() == "saga_string_concat")
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
            (call->getCalledFunction()->getName() == "saga_int_to_string" ||
             call->getCalledFunction()->getName() == "saga_float_to_string" ||
             call->getCalledFunction()->getName() == "saga_bool_to_string"))
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
            call->getCalledFunction()->getName() == "saga_release_string")
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
            call->getCalledFunction()->getName() == "saga_release_array")
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
            call->getCalledFunction()->getName() == "saga_release_string")
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
            call->getCalledFunction()->getName() == "saga_release_string")
          release_count++;
  EXPECT_GE(release_count, 2) << "Old string released on +=, final at exit";
}

TEST(CodeGen, RetainReleaseDeclared) {
  auto r = CG::from("pub fn Main() Void {}");
  EXPECT_NE(r.func("saga_retain_string"), nullptr);
  EXPECT_NE(r.func("saga_release_string"), nullptr);
  EXPECT_NE(r.func("saga_retain_array"), nullptr);
  EXPECT_NE(r.func("saga_release_array"), nullptr);
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
            call->getCalledFunction()->getName() == "saga_release_string")
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
            (call->getCalledFunction()->getName() == "saga_release_string" ||
             call->getCalledFunction()->getName() == "saga_release_array"))
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

// ===========================================================================
// Switch/case — value matching
// ===========================================================================

TEST(CodeGen, SwitchIntBasicBlocks) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 1\n"
      "  switch x {\n"
      "    case 0: intrinsic_print(\"zero\\n\")\n"
      "    case 1: intrinsic_print(\"one\\n\")\n"
      "  }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have case blocks and a merge.
  bool found_case = false, found_merge = false;
  for (auto &bb : *main) {
    if (bb.getName().starts_with("sw.case")) found_case = true;
    if (bb.getName().starts_with("sw.merge")) found_merge = true;
  }
  EXPECT_TRUE(found_case) << "Should have sw.case blocks";
  EXPECT_TRUE(found_merge) << "Should have sw.merge block";
}

TEST(CodeGen, SwitchIntUsesSwInst) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 1\n"
      "  switch x {\n"
      "    case 0: intrinsic_print(\"zero\\n\")\n"
      "    case 1: intrinsic_print(\"one\\n\")\n"
      "  }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found_switch = false;
  for (auto &bb : *main)
    if (auto *term = bb.getTerminator())
      if (llvm::isa<llvm::SwitchInst>(term))
        found_switch = true;
  EXPECT_TRUE(found_switch) << "Should use LLVM switch instruction for int";
}

TEST(CodeGen, SwitchIntWithElse) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 5\n"
      "  switch x {\n"
      "    case 0: intrinsic_print(\"zero\\n\")\n"
      "    else: intrinsic_print(\"other\\n\")\n"
      "  }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found_default = false;
  for (auto &bb : *main)
    if (bb.getName().starts_with("sw.default"))
      found_default = true;
  EXPECT_TRUE(found_default) << "Should have sw.default block for else";
}

TEST(CodeGen, SwitchIntExpression) {
  // Switch used as an expression should produce a PHI.
  auto r = CG::from(
      "fn Describe(n Int) Int {\n"
      "  switch n {\n"
      "    case 0: 10\n"
      "    case 1: 20\n"
      "    else: 30\n"
      "  }\n"
      "}\n"
      "pub fn Main() Void {}");
  auto *fn = r.func("Describe");
  ASSERT_NE(fn, nullptr);
  bool found_phi = false;
  for (auto &bb : *fn)
    for (auto &inst : bb)
      if (llvm::isa<llvm::PHINode>(&inst))
        found_phi = true;
  EXPECT_TRUE(found_phi) << "Switch expression should produce a PHI node";
}

TEST(CodeGen, SwitchIntExprReturnedAsTail) {
  auto r = CG::from(
      "fn Classify(n Int) Int {\n"
      "  switch n {\n"
      "    case 0: 100\n"
      "    else: 200\n"
      "  }\n"
      "}\n"
      "pub fn Main() Void {}");
  auto *fn = r.func("Classify");
  ASSERT_NE(fn, nullptr);
  // The merge block should have a ret with the PHI value.
  bool found_ret = false;
  for (auto &bb : *fn)
    if (auto *term = bb.getTerminator())
      if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(term))
        if (ret->getReturnValue() &&
            ret->getReturnValue()->getType()->isIntegerTy(64))
          found_ret = true;
  EXPECT_TRUE(found_ret);
}

TEST(CodeGen, SwitchEnumValue) {
  auto r = CG::from(
      "enum Colors { Red\n Green\n Blue }\n"
      "pub fn Main() Void {\n"
      "  c := Colors.Green\n"
      "  switch c {\n"
      "    case Colors.Red: intrinsic_print(\"red\\n\")\n"
      "    case Colors.Green: intrinsic_print(\"green\\n\")\n"
      "    case Colors.Blue: intrinsic_print(\"blue\\n\")\n"
      "  }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have a switch instruction.
  bool found_switch = false;
  for (auto &bb : *main)
    if (auto *term = bb.getTerminator())
      if (llvm::isa<llvm::SwitchInst>(term))
        found_switch = true;
  EXPECT_TRUE(found_switch);
}

TEST(CodeGen, SwitchEnumExpression) {
  auto r = CG::from(
      "enum Dir { North\n South\n East\n West }\n"
      "fn ToInt(d Dir) Int {\n"
      "  switch d {\n"
      "    case Dir.North: 0\n"
      "    case Dir.South: 1\n"
      "    case Dir.East: 2\n"
      "    else: 3\n"
      "  }\n"
      "}\n"
      "pub fn Main() Void {}");
  auto *fn = r.func("ToInt");
  ASSERT_NE(fn, nullptr);
  bool found_switch = false;
  for (auto &bb : *fn)
    if (auto *term = bb.getTerminator())
      if (auto *sw = llvm::dyn_cast<llvm::SwitchInst>(term)) {
        found_switch = true;
        EXPECT_EQ(sw->getNumCases(), 3u);
      }
  EXPECT_TRUE(found_switch);
}

TEST(CodeGen, SwitchWithBlockBody) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 2\n"
      "  switch x {\n"
      "    case 1: {\n"
      "      intrinsic_print(\"one\\n\")\n"
      "    }\n"
      "    case 2: {\n"
      "      intrinsic_print(\"two\\n\")\n"
      "    }\n"
      "  }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have print calls in the case blocks.
  int print_count = 0;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "saga_intrinsic_print")
          print_count++;
  EXPECT_EQ(print_count, 2);
}

TEST(CodeGen, SwitchStringUsesChainedCompare) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  s := \"hi\"\n"
      "  switch s {\n"
      "    case \"hello\": intrinsic_print(\"found hello\\n\")\n"
      "    case \"hi\": intrinsic_print(\"found hi\\n\")\n"
      "  }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // String switches should NOT use LLVM switch instruction.
  bool found_switch = false;
  for (auto &bb : *main)
    if (auto *term = bb.getTerminator())
      if (llvm::isa<llvm::SwitchInst>(term))
        found_switch = true;
  EXPECT_FALSE(found_switch)
      << "String switch should use chained compare, not switch inst";
  // Should have calls to saga_string_compare.
  int cmp_count = 0;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "saga_string_compare")
          cmp_count++;
  EXPECT_GE(cmp_count, 2) << "Should compare against each case pattern";
}

TEST(CodeGen, SwitchStringWithElse) {
  auto r = CG::from(
      "fn Greet(name String) String {\n"
      "  switch name {\n"
      "    case \"Alice\": \"Hi Alice!\"\n"
      "    else: \"Hello stranger\"\n"
      "  }\n"
      "}\n"
      "pub fn Main() Void {}");
  auto *fn = r.func("Greet");
  ASSERT_NE(fn, nullptr);
  // Should have a PHI for the expression result.
  bool found_phi = false;
  for (auto &bb : *fn)
    for (auto &inst : bb)
      if (llvm::isa<llvm::PHINode>(&inst))
        found_phi = true;
  EXPECT_TRUE(found_phi);
}

TEST(CodeGen, SwitchCasesNoFallThrough) {
  // Each case should branch directly to merge, not fall through.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 1\n"
      "  switch x {\n"
      "    case 0: intrinsic_print(\"zero\\n\")\n"
      "    case 1: intrinsic_print(\"one\\n\")\n"
      "    case 2: intrinsic_print(\"two\\n\")\n"
      "  }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Each case block should end with a branch to sw.merge.
  for (auto &bb : *main) {
    if (bb.getName().starts_with("sw.case")) {
      auto *term = bb.getTerminator();
      ASSERT_NE(term, nullptr);
      if (auto *br = llvm::dyn_cast<llvm::BranchInst>(term)) {
        EXPECT_TRUE(br->isUnconditional());
        EXPECT_TRUE(br->getSuccessor(0)->getName().starts_with("sw.merge"));
      }
    }
  }
}

TEST(CodeGen, SwitchMultipleCases) {
  // Three cases + else, each with side effects.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 2\n"
      "  switch x {\n"
      "    case 0: intrinsic_print(\"zero\\n\")\n"
      "    case 1: intrinsic_print(\"one\\n\")\n"
      "    case 2: intrinsic_print(\"two\\n\")\n"
      "    else: intrinsic_print(\"other\\n\")\n"
      "  }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have a switch with 3 cases.
  for (auto &bb : *main)
    if (auto *term = bb.getTerminator())
      if (auto *sw = llvm::dyn_cast<llvm::SwitchInst>(term))
        EXPECT_EQ(sw->getNumCases(), 3u);
}

TEST(CodeGen, SwitchSingleCase) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 42\n"
      "  switch x {\n"
      "    case 42: intrinsic_print(\"found\\n\")\n"
      "  }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found_switch = false;
  for (auto &bb : *main)
    if (auto *term = bb.getTerminator())
      if (auto *sw = llvm::dyn_cast<llvm::SwitchInst>(term)) {
        found_switch = true;
        EXPECT_EQ(sw->getNumCases(), 1u);
      }
  EXPECT_TRUE(found_switch);
}

TEST(CodeGen, SwitchCallsInCases) {
  auto r = CG::from(
      "fn Handle(code Int) Void {\n"
      "  switch code {\n"
      "    case 0: intrinsic_print(\"ok\\n\")\n"
      "    case 1: intrinsic_print(\"warn\\n\")\n"
      "    else: intrinsic_print(\"err\\n\")\n"
      "  }\n"
      "}\n"
      "pub fn Main() Void {}");
  auto *fn = r.func("Handle");
  ASSERT_NE(fn, nullptr);
  // Should have 3 print calls (one per case/else).
  int print_count = 0;
  for (auto &bb : *fn)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "saga_intrinsic_print")
          print_count++;
  EXPECT_EQ(print_count, 3);
}

// ===========================================================================
// Multiple return values
// ===========================================================================

TEST(CodeGen, MultiReturnFuncType) {
  auto r = CG::from(
      "fn Pair() Int, Int {\n"
      "  return 1, 2\n"
      "}\n"
      "pub fn Main() Void {}");
  auto *fn = r.func("Pair");
  ASSERT_NE(fn, nullptr);
  // Return type should be a struct { i64, i64 }.
  auto *ret = fn->getReturnType();
  ASSERT_TRUE(ret->isStructTy()) << "Multi-return should use struct type";
  auto *st = llvm::cast<llvm::StructType>(ret);
  EXPECT_EQ(st->getNumElements(), 2u);
  EXPECT_TRUE(st->getElementType(0)->isIntegerTy(64));
  EXPECT_TRUE(st->getElementType(1)->isIntegerTy(64));
}

TEST(CodeGen, MultiReturnStructName) {
  auto r = CG::from(
      "fn Pair() Int, Int {\n"
      "  return 1, 2\n"
      "}\n"
      "pub fn Main() Void {}");
  // Should have a named struct type for multi-return.
  auto *st = llvm::StructType::getTypeByName(
      r.mod().getContext(), "mc.ret." + CG::mangled("Pair"));
  ASSERT_NE(st, nullptr);
  EXPECT_EQ(st->getNumElements(), 2u);
}

TEST(CodeGen, MultiReturnPacksValues) {
  // Use non-constant values so LLVM can't constant-fold.
  auto r = CG::from(
      "fn Pair(a, b Int) Int, Int {\n"
      "  return a, b\n"
      "}\n"
      "pub fn Main() Void {}");
  auto *fn = r.func("Pair");
  ASSERT_NE(fn, nullptr);
  // Should have insertvalue instructions to pack the struct.
  int insert_count = 0;
  for (auto &bb : *fn)
    for (auto &inst : bb)
      if (llvm::isa<llvm::InsertValueInst>(&inst))
        insert_count++;
  EXPECT_GE(insert_count, 2) << "Should pack 2 values with insertvalue";
}

TEST(CodeGen, MultiReturnMixedTypes) {
  auto r = CG::from(
      "fn IntAndString() Int, String {\n"
      "  return 42, \"hello\"\n"
      "}\n"
      "pub fn Main() Void {}");
  auto *fn = r.func("IntAndString");
  ASSERT_NE(fn, nullptr);
  auto *ret = fn->getReturnType();
  ASSERT_TRUE(ret->isStructTy());
  auto *st = llvm::cast<llvm::StructType>(ret);
  EXPECT_EQ(st->getNumElements(), 2u);
  EXPECT_TRUE(st->getElementType(0)->isIntegerTy(64));
  EXPECT_TRUE(st->getElementType(1)->isPointerTy()); // String is ptr
}

TEST(CodeGen, MultiReturnThreeValues) {
  auto r = CG::from(
      "fn Triple() Int, Int, Int {\n"
      "  return 1, 2, 3\n"
      "}\n"
      "pub fn Main() Void {}");
  auto *fn = r.func("Triple");
  ASSERT_NE(fn, nullptr);
  auto *ret = fn->getReturnType();
  ASSERT_TRUE(ret->isStructTy());
  EXPECT_EQ(llvm::cast<llvm::StructType>(ret)->getNumElements(), 3u);
}

TEST(CodeGen, MultiReturnUnpack) {
  auto r = CG::from(
      "fn Pair() Int, Int {\n"
      "  return 10, 20\n"
      "}\n"
      "pub fn Main() Void {\n"
      "  x, y := Pair()\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(has_alloca_named(main, "x"));
  EXPECT_TRUE(has_alloca_named(main, "y"));
  // Should have extractvalue instructions to unpack.
  int extract_count = 0;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (llvm::isa<llvm::ExtractValueInst>(&inst))
        extract_count++;
  EXPECT_GE(extract_count, 2) << "Should unpack 2 values with extractvalue";
}

TEST(CodeGen, MultiReturnUnpackMixedTypes) {
  auto r = CG::from(
      "fn GetData() Int, String {\n"
      "  return 42, \"hello\"\n"
      "}\n"
      "pub fn Main() Void {\n"
      "  n, s := GetData()\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(has_alloca_named(main, "n"));
  EXPECT_TRUE(has_alloca_named(main, "s"));
}

TEST(CodeGen, MultiReturnCallAndUse) {
  // Verify extracted values can be used after unpacking.
  auto r = CG::from(
      "fn Pair() Int, Int {\n"
      "  return 10, 20\n"
      "}\n"
      "pub fn Main() Void {\n"
      "  x, y := Pair()\n"
      "  z := x + y\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(has_alloca_named(main, "z"));
  // Should have a call to Pair.
  bool found_pair = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            CG::calls_func(call, "Pair"))
          found_pair = true;
  EXPECT_TRUE(found_pair);
  // Should have an add for z := x + y.
  EXPECT_GE(count_opcodes(main, llvm::Instruction::Add), 1);
}

TEST(CodeGen, MultiReturnRegistered) {
  auto r = CG::from(
      "fn Pair() Int, Int {\n"
      "  return 1, 2\n"
      "}\n"
      "pub fn Main() Void {}");
  EXPECT_TRUE(r.codegen->multi_return_types.count(CG::mangled("Pair")));
  EXPECT_EQ(r.codegen->multi_return_counts[CG::mangled("Pair")], 2u);
}

TEST(CodeGen, MultiReturnDoesNotAffectSingleReturn) {
  // Single-return functions should remain unchanged.
  auto r = CG::from(
      "fn Single() Int { 42 }\n"
      "fn Pair() Int, Int {\n"
      "  return 1, 2\n"
      "}\n"
      "pub fn Main() Void {}");
  auto *single = r.func("Single");
  ASSERT_NE(single, nullptr);
  EXPECT_TRUE(single->getReturnType()->isIntegerTy(64))
      << "Single-return should still return i64";
  EXPECT_FALSE(r.codegen->multi_return_types.count(CG::mangled("Single")));
}

TEST(CodeGen, MultiReturnRetInstEmitted) {
  auto r = CG::from(
      "fn Pair() Int, Int {\n"
      "  return 10, 20\n"
      "}\n"
      "pub fn Main() Void {}");
  auto *fn = r.func("Pair");
  ASSERT_NE(fn, nullptr);
  // Should have exactly one ret instruction returning the struct.
  int ret_count = 0;
  for (auto &bb : *fn)
    if (auto *term = bb.getTerminator())
      if (auto *ret = llvm::dyn_cast<llvm::ReturnInst>(term)) {
        ret_count++;
        ASSERT_NE(ret->getReturnValue(), nullptr);
        EXPECT_TRUE(ret->getReturnValue()->getType()->isStructTy());
      }
  EXPECT_EQ(ret_count, 1);
}

TEST(CodeGen, MultiReturnIntAndBool) {
  auto r = CG::from(
      "fn Check() Int, Bool {\n"
      "  return 42, true\n"
      "}\n"
      "pub fn Main() Void {\n"
      "  n, ok := Check()\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(has_alloca_named(main, "n"));
  EXPECT_TRUE(has_alloca_named(main, "ok"));
}

TEST(CodeGen, MultiReturnIntAndFloat) {
  auto r = CG::from(
      "fn Compute() Int, Float {\n"
      "  return 1, 3.14\n"
      "}\n"
      "pub fn Main() Void {\n"
      "  i, f := Compute()\n"
      "}");
  auto *fn = r.func("Compute");
  ASSERT_NE(fn, nullptr);
  auto *ret = fn->getReturnType();
  ASSERT_TRUE(ret->isStructTy());
  auto *st = llvm::cast<llvm::StructType>(ret);
  EXPECT_TRUE(st->getElementType(0)->isIntegerTy(64));
  EXPECT_TRUE(st->getElementType(1)->isDoubleTy());
}

// ===========================================================================
// Interface conformance — vtables and dynamic dispatch
// ===========================================================================

TEST(CodeGen, InterfaceVtableTypeCreated) {
  auto r = CG::from(
      "interface Greeter { Greet() String }\n"
      "pub fn Main() Void {}");
  auto *st = llvm::StructType::getTypeByName(
      r.mod().getContext(), "mc.vtable.Greeter");
  ASSERT_NE(st, nullptr);
  EXPECT_EQ(st->getNumElements(), 1u) << "Vtable should have 1 fn ptr";
  EXPECT_TRUE(st->getElementType(0)->isPointerTy());
}

TEST(CodeGen, InterfaceVtableMultipleMethods) {
  auto r = CG::from(
      "interface ReadWriter {\n"
      "  Read() String\n"
      "  Write(s String) Void\n"
      "}\n"
      "pub fn Main() Void {}");
  auto *st = llvm::StructType::getTypeByName(
      r.mod().getContext(), "mc.vtable.ReadWriter");
  ASSERT_NE(st, nullptr);
  EXPECT_EQ(st->getNumElements(), 2u);
}

TEST(CodeGen, InterfaceFatPtrTypeExists) {
  auto r = CG::from("pub fn Main() Void {}");
  auto *st = llvm::StructType::getTypeByName(
      r.mod().getContext(), "mc_iface");
  ASSERT_NE(st, nullptr);
  EXPECT_EQ(st->getNumElements(), 2u); // { ptr data, ptr vtable }
  EXPECT_TRUE(st->getElementType(0)->isPointerTy());
  EXPECT_TRUE(st->getElementType(1)->isPointerTy());
}

TEST(CodeGen, StructMethodDeclared) {
  // Use out-bound method style (receiver syntax) which the analyzer handles.
  auto r = CG::from(
      "struct Dog { name String }\n"
      "fn (d Dog) Speak() String { d.name }\n"
      "pub fn Main() Void {}");
  auto *fn = r.func("Dog.Speak");
  ASSERT_NE(fn, nullptr) << "Struct method should be declared with mangled name";
  EXPECT_EQ(fn->arg_size(), 1u) << "Should have self param only";
  EXPECT_TRUE(fn->getArg(0)->getType()->isPointerTy());
}

TEST(CodeGen, StructMethodWithParams) {
  auto r = CG::from(
      "struct Counter { n Int }\n"
      "fn (c Counter) Add(x Int) Int { c.n + x }\n"
      "pub fn Main() Void {}");
  auto *fn = r.func("Counter.Add");
  ASSERT_NE(fn, nullptr);
  EXPECT_EQ(fn->arg_size(), 2u) << "self + x";
  EXPECT_TRUE(fn->getReturnType()->isIntegerTy(64));
}

TEST(CodeGen, OutBoundMethodDeclared) {
  auto r = CG::from(
      "struct Point { x, y Int }\n"
      "fn (p Point) Magnitude() Int { p.x + p.y }\n"
      "pub fn Main() Void {}");
  auto *fn = r.func("Point.Magnitude");
  ASSERT_NE(fn, nullptr);
  EXPECT_EQ(fn->arg_size(), 1u);
}

TEST(CodeGen, StructMethodCall) {
  auto r = CG::from(
      "struct Counter { n Int }\n"
      "fn (c Counter) Value() Int { c.n }\n"
      "pub fn Main() Void {\n"
      "  c := Counter{n: 42}\n"
      "  x := c.Value()\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(has_alloca_named(main, "x"));
  // Should have a call to Counter.Value.
  bool found = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            CG::calls_func(call, "Counter.Value"))
          found = true;
  EXPECT_TRUE(found) << "Should call Counter.Value";
}

TEST(CodeGen, VtableGlobalCreated) {
  auto r = CG::from(
      "interface Speaker { Speak() String }\n"
      "struct Dog { name String }\n"
      "fn (d Dog) Speak() String { d.name }\n"
      "pub fn Main() Void {\n"
      "  d := Dog{name: \"Rex\"}\n"
      "  s Speaker = d\n"
      "}");
  // Should have a vtable global for Dog implementing Speaker.
  auto *vtable = r.mod().getNamedGlobal("mc.vtable.Dog.Speaker");
  ASSERT_NE(vtable, nullptr) << "Should create vtable for Dog::Speaker";
  EXPECT_TRUE(vtable->isConstant());
}

TEST(CodeGen, InterfaceBoxCreatesAllocas) {
  auto r = CG::from(
      "interface Speaker { Speak() String }\n"
      "struct Dog { name String }\n"
      "fn (d Dog) Speak() String { d.name }\n"
      "pub fn Main() Void {\n"
      "  d := Dog{name: \"Rex\"}\n"
      "  s Speaker = d\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have struct GEPs for the fat pointer.
  int gep_count = 0;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (llvm::isa<llvm::GetElementPtrInst>(&inst))
        gep_count++;
  EXPECT_GE(gep_count, 2) << "Should have GEPs for data and vtable in fat ptr";
}

TEST(CodeGen, InterfaceDynamicDispatch) {
  auto r = CG::from(
      "interface Speaker { Speak() String }\n"
      "struct Dog { name String }\n"
      "fn (d Dog) Speak() String { d.name }\n"
      "pub fn Main() Void {\n"
      "  d := Dog{name: \"Rex\"}\n"
      "  s Speaker = d\n"
      "  msg := s.Speak()\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have an indirect call (call through function pointer).
  bool found_indirect = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (!call->getCalledFunction()) // indirect call has no static callee
          found_indirect = true;
  EXPECT_TRUE(found_indirect) << "Interface dispatch should use indirect call";
}

TEST(CodeGen, InterfaceParamType) {
  auto r = CG::from(
      "interface Speaker { Speak() String }\n"
      "struct Dog { name String }\n"
      "fn (d Dog) Speak() String { d.name }\n"
      "fn UseSpeaker(s Speaker) String { s.Speak() }\n"
      "pub fn Main() Void {}");
  auto *fn = r.func("UseSpeaker");
  ASSERT_NE(fn, nullptr);
  EXPECT_EQ(fn->arg_size(), 1u);
  EXPECT_TRUE(fn->getArg(0)->getType()->isPointerTy())
      << "Interface param should be ptr (to fat pointer)";
}

TEST(CodeGen, InterfaceMethodNamesRegistered) {
  auto r = CG::from(
      "interface Stringer { String() String }\n"
      "pub fn Main() Void {}");
  auto it = r.codegen->iface_method_names.find("Stringer");
  ASSERT_NE(it, r.codegen->iface_method_names.end());
  ASSERT_EQ(it->second.size(), 1u);
  EXPECT_EQ(it->second[0], "String");
}

TEST(CodeGen, MultipleInterfacesDeclared) {
  auto r = CG::from(
      "interface Reader { Read() String }\n"
      "interface Writer { Write(s String) Void }\n"
      "pub fn Main() Void {}");
  EXPECT_TRUE(r.codegen->iface_vtable_types.count("Reader"));
  EXPECT_TRUE(r.codegen->iface_vtable_types.count("Writer"));
}

TEST(CodeGen, StructMethodRegisteredInLinks) {
  auto r = CG::from(
      "struct Cat { name String }\n"
      "fn (c Cat) Meow() String { \"meow\" }\n"
      "pub fn Main() Void {}");
  auto it = r.codegen->struct_method_links.find("Cat");
  ASSERT_NE(it, r.codegen->struct_method_links.end());
  ASSERT_EQ(it->second.size(), 1u);
  EXPECT_EQ(it->second[0].first, CG::mangled("Cat__Meow"));
  EXPECT_EQ(it->second[0].second, "Meow");
}

// ===========================================================================
// Union types and or-clause
// ===========================================================================

TEST(CodeGen, UnionTypeStructExists) {
  // Declaring a variable with a union type should produce a tagged union.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x Int | Error = 0\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have generated a union LLVM type.
  EXPECT_FALSE(r.codegen->union_llvm_types.empty());
}

TEST(CodeGen, DivisionProducesUnion) {
  // Division returns Int | Error, so it should produce a tagged union.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 10 / 2\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have a union type registered.
  EXPECT_FALSE(r.codegen->union_llvm_types.empty());
}

TEST(CodeGen, OrExprStripsErrorSimple) {
  // `x or { 0 }` should produce the non-error value.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 10 / 2 or { 0 }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have or.ok and or.err blocks.
  bool found_ok = false, found_err = false;
  for (auto &bb : *main) {
    if (bb.getName().starts_with("or.ok")) found_ok = true;
    if (bb.getName().starts_with("or.err")) found_err = true;
  }
  EXPECT_TRUE(found_ok) << "Should have or.ok block";
  EXPECT_TRUE(found_err) << "Should have or.err block";
}

TEST(CodeGen, OrExprWithPipe) {
  // `x or |err| { 0 }` should create the pipe variable.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 10 / 2 or |err| { 0 }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have an err alloca in the or.err block.
  bool found_err_var = false;
  for (auto &bb : *main) {
    for (auto &inst : bb) {
      if (auto *a = llvm::dyn_cast<llvm::AllocaInst>(&inst))
        if (a->getName() == "err")
          found_err_var = true;
    }
  }
  EXPECT_TRUE(found_err_var) << "Should have 'err' alloca in or block";
}

TEST(CodeGen, OrExprMergeBlock) {
  // The or expression should merge into a single block after ok/err.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 10 / 2 or { 0 }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found_merge = false;
  for (auto &bb : *main)
    if (bb.getName().starts_with("or.merge"))
      found_merge = true;
  EXPECT_TRUE(found_merge) << "Should have or.merge block";
}

TEST(CodeGen, UnionPayloadSize) {
  // A union of Int | Error should have a payload size of at least 8 bytes.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x Int | Error = 0\n"
      "}");
  // Check that the union type exists and has proper layout.
  EXPECT_FALSE(r.codegen->union_llvm_types.empty());
  for (auto &[key, st] : r.codegen->union_llvm_types) {
    EXPECT_EQ(st->getNumElements(), 2u);
    EXPECT_TRUE(st->getElementType(0)->isIntegerTy(8)) << "Tag should be i8";
    EXPECT_TRUE(st->getElementType(1)->isArrayTy()) << "Payload should be array";
  }
}

TEST(CodeGen, OrExprDefaultValue) {
  // Empty or block should produce zero value.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 10 / 2 or {}\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
}

TEST(CodeGen, UnionVarDeclExplicitType) {
  // Explicit union type annotation with a concrete initializer.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x Int | Error = 42\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
}

TEST(CodeGen, FuncReturnsUnion) {
  // A function with a union return type should produce the union struct.
  auto r = CG::from(
      "fn SafeDiv(a, b Int) Int | Error {\n"
      "  a / b\n"
      "}\n"
      "pub fn Main() Void {\n"
      "  x := SafeDiv(10, 2)\n"
      "}");
  auto *safe_div = r.func("SafeDiv");
  ASSERT_NE(safe_div, nullptr);
  // Return type should be the union struct, not i64.
  auto *ret_type = safe_div->getReturnType();
  EXPECT_TRUE(ret_type->isStructTy()) << "Return type should be a union struct";
  if (ret_type->isStructTy()) {
    auto *st = llvm::cast<llvm::StructType>(ret_type);
    EXPECT_EQ(st->getNumElements(), 2u);
    EXPECT_TRUE(st->getElementType(0)->isIntegerTy(8)) << "Tag i8";
  }
}

TEST(CodeGen, FuncReturnsUnionWithOrAtCallSite) {
  // Calling a function that returns a union and using or to unwrap.
  auto r = CG::from(
      "fn SafeDiv(a, b Int) Int | Error {\n"
      "  a / b\n"
      "}\n"
      "pub fn Main() Void {\n"
      "  x := SafeDiv(10, 2) or { 0 }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have or.ok and or.err blocks.
  bool found_ok = false, found_err = false;
  for (auto &bb : *main) {
    if (bb.getName().starts_with("or.ok")) found_ok = true;
    if (bb.getName().starts_with("or.err")) found_err = true;
  }
  EXPECT_TRUE(found_ok);
  EXPECT_TRUE(found_err);
}

TEST(CodeGen, OrExprPhiNode) {
  // The or expression should produce a PHI node merging ok/fallback values.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 10 / 2 or { 99 }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Find the merge block and check for PHI.
  bool found_phi = false;
  for (auto &bb : *main) {
    if (bb.getName().starts_with("or.merge")) {
      for (auto &inst : bb) {
        if (llvm::isa<llvm::PHINode>(&inst))
          found_phi = true;
      }
    }
  }
  EXPECT_TRUE(found_phi) << "or.merge should have a PHI node";
}

TEST(CodeGen, UnionTagIsZeroForFirstAlt) {
  // When wrapping a value of the first alternative, tag should be 0.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x Int | Error = 42\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Look for a store of i8 0 (the tag for Int, first alternative).
  bool found_tag_store = false;
  for (auto &bb : *main) {
    for (auto &inst : bb) {
      if (auto *si = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        if (auto *ci = llvm::dyn_cast<llvm::ConstantInt>(si->getValueOperand())) {
          if (ci->getType()->isIntegerTy(8) && ci->getZExtValue() == 0)
            found_tag_store = true;
        }
      }
    }
  }
  EXPECT_TRUE(found_tag_store) << "Should store tag 0 for Int in Int|Error";
}

TEST(CodeGen, DivisionOrThenUse) {
  // Division, or-unwrap, then use the result.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 10 / 2 or { 0 }\n"
      "  y := x + 1\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have an add instruction.
  bool found_add = false;
  for (auto &bb : *main) {
    for (auto &inst : bb) {
      if (inst.getOpcode() == llvm::Instruction::Add)
        found_add = true;
    }
  }
  EXPECT_TRUE(found_add) << "Should have add after or-unwrap";
}

// ===========================================================================
// Type matching on union types
// ===========================================================================

TEST(CodeGen, IfTypeMatchOnUnion) {
  // Type matching: `if x == Int` should compare the tag byte.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x Int | Error = 42\n"
      "  if x == Int {\n"
      "    intrinsic_print(\"yes\\n\")\n"
      "  }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have a then block and the tag comparison.
  bool found_then = false;
  bool found_icmp = false;
  for (auto &bb : *main) {
    if (bb.getName().starts_with("then")) found_then = true;
    for (auto &inst : bb) {
      if (auto *cmp = llvm::dyn_cast<llvm::ICmpInst>(&inst)) {
        // Should be comparing i8 values (tag).
        if (cmp->getOperand(0)->getType()->isIntegerTy(8))
          found_icmp = true;
      }
    }
  }
  EXPECT_TRUE(found_then) << "Should have 'then' block";
  EXPECT_TRUE(found_icmp) << "Should have i8 tag comparison";
}

TEST(CodeGen, SwitchTypeMatchOnUnion) {
  // Switch on union with type patterns should switch on tag.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x Int | String = 42\n"
      "  switch x {\n"
      "    case Int: { intrinsic_print(\"int\\n\") }\n"
      "    case String: { intrinsic_print(\"str\\n\") }\n"
      "  }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have a switch instruction on i8 tag.
  bool found_switch = false;
  for (auto &bb : *main) {
    if (auto *sw = llvm::dyn_cast<llvm::SwitchInst>(bb.getTerminator())) {
      if (sw->getCondition()->getType()->isIntegerTy(8))
        found_switch = true;
    }
  }
  EXPECT_TRUE(found_switch) << "Should have i8 switch on union tag";
}

TEST(CodeGen, IfTypeMatchNarrowsExtract) {
  // After type match, the narrowed variable should have the concrete type.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x Int | Error = 42\n"
      "  if x == Int {\n"
      "    y := x + 1\n"
      "  }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have an add instruction in the then block (x narrowed to Int).
  bool found_add = false;
  for (auto &bb : *main) {
    if (bb.getName().starts_with("then")) {
      for (auto &inst : bb) {
        if (inst.getOpcode() == llvm::Instruction::Add)
          found_add = true;
      }
    }
  }
  EXPECT_TRUE(found_add) << "Then block should have add (narrowed Int)";
}

TEST(CodeGen, CallUnionFuncThenOr) {
  // Full pipeline: function returns union, caller uses or to unwrap.
  auto r = CG::from(
      "fn SafeDiv(a, b Int) Int | Error {\n"
      "  a / b\n"
      "}\n"
      "pub fn Main() Void {\n"
      "  result := SafeDiv(10, 2) or { 0 }\n"
      "  y := result + 1\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have the full pipeline: call, or blocks, add.
  bool found_call = false, found_or = false, found_add = false;
  for (auto &bb : *main) {
    for (auto &inst : bb) {
      if (llvm::isa<llvm::CallInst>(&inst)) found_call = true;
      if (inst.getOpcode() == llvm::Instruction::Add) found_add = true;
    }
    if (bb.getName().starts_with("or.")) found_or = true;
  }
  EXPECT_TRUE(found_call) << "Should call SafeDiv";
  EXPECT_TRUE(found_or) << "Should have or blocks";
  EXPECT_TRUE(found_add) << "Should have add after unwrap";
}

TEST(CodeGen, UnionAssignThenTypeMatch) {
  // Assign a concrete value to a union, then type-match it.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x Int | String = 42\n"
      "  if x == Int {\n"
      "    intrinsic_print(\"is int\\n\")\n"
      "  } else {\n"
      "    intrinsic_print(\"is string\\n\")\n"
      "  }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have both then and else blocks.
  bool found_then = false, found_else = false;
  for (auto &bb : *main) {
    if (bb.getName().starts_with("then")) found_then = true;
    if (bb.getName().starts_with("else")) found_else = true;
  }
  EXPECT_TRUE(found_then);
  EXPECT_TRUE(found_else);
}

// ===========================================================================
// Slice — Maps: literal, indexing, methods, iteration, refcounting
// ===========================================================================

TEST(CodeGen, MapRuntimeDeclared) {
  auto r = CG::from("pub fn Main() Void {}");
  EXPECT_NE(r.func("saga_map_new"), nullptr);
  EXPECT_NE(r.func("saga_map_set"), nullptr);
  EXPECT_NE(r.func("saga_map_get"), nullptr);
  EXPECT_NE(r.func("saga_map_has"), nullptr);
  EXPECT_NE(r.func("saga_map_remove"), nullptr);
  EXPECT_NE(r.func("saga_map_size"), nullptr);
  EXPECT_NE(r.func("saga_map_key_at"), nullptr);
  EXPECT_NE(r.func("saga_map_value_at"), nullptr);
  EXPECT_NE(r.func("saga_retain_map"), nullptr);
  EXPECT_NE(r.func("saga_release_map"), nullptr);
}

TEST(CodeGen, MapLiteralCreated) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  m := {\"a\": 1, \"b\": 2}\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should call mc_map_new.
  bool found = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "saga_map_new")
          found = true;
  EXPECT_TRUE(found);
}

TEST(CodeGen, MapLiteralSetCalled) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  m := {\"a\": 1, \"b\": 2}\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  int set_count = 0;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "saga_map_set")
          set_count++;
  EXPECT_EQ(set_count, 2);
}

TEST(CodeGen, MapIntKeys) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  m := {1: \"one\", 2: \"two\"}\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found_new = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "saga_map_new")
          found_new = true;
  EXPECT_TRUE(found_new);
}

TEST(CodeGen, MapIndexAccess) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  m := {\"x\": 42}\n"
      "  v := m[\"x\"]\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found_get = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "saga_map_get")
          found_get = true;
  EXPECT_TRUE(found_get);
}

TEST(CodeGen, MapIndexAssignment) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  m := {\"a\": 1}\n"
      "  m[\"b\"] = 2\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have 2 mc_map_set calls: one from literal, one from assignment.
  int set_count = 0;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "saga_map_set")
          set_count++;
  EXPECT_EQ(set_count, 2);
}

TEST(CodeGen, MapSizeMethod) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  m := {\"a\": 1, \"b\": 2}\n"
      "  n := m.Size()\n"
      "}", false);
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "map__Map__Size")
          found = true;
  EXPECT_TRUE(found);
}

TEST(CodeGen, MapRemoveMethod) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  m := {\"a\": 1, \"b\": 2}\n"
      "  m.Remove(\"a\")\n"
      "}", false);
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "map__Map__Remove")
          found = true;
  EXPECT_TRUE(found);
}

TEST(CodeGen, MapKeyCheckMethod) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  m := {\"a\": 1}\n"
      "  exists? := m.Key?(\"a\")\n"
      "}", false);
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "map__Map__Key?")
          found = true;
  EXPECT_TRUE(found);
}

TEST(CodeGen, MapSetMethod) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  m := {\"a\": 1}\n"
      "  m.Set(\"b\", 2)\n"
      "}", false);
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // 1 saga_map_set from literal + 1 map__Map__Set from method call.
  int literal_set = 0;
  int method_set = 0;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction()) {
          if (call->getCalledFunction()->getName() == "saga_map_set")
            literal_set++;
          if (call->getCalledFunction()->getName() == "map__Map__Set")
            method_set++;
        }
  EXPECT_EQ(literal_set, 1);
  EXPECT_EQ(method_set, 1);
}

TEST(CodeGen, MapReleaseAtFuncExit) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  m := {\"a\": 1}\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "saga_release_map")
          found = true;
  EXPECT_TRUE(found) << "Map locals should be released at function exit";
}

TEST(CodeGen, MapForRangeValue) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  m := {\"a\": 1, \"b\": 2}\n"
      "  for v : m {\n"
      "    intrinsic_print(\".\")\n"
      "  }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have for.cond block.
  bool found_cond = false;
  for (auto &bb : *main)
    if (bb.getName().starts_with("for.cond"))
      found_cond = true;
  EXPECT_TRUE(found_cond);
  // Should call mc_map_value_at in the body.
  bool found_val_at = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "saga_map_value_at")
          found_val_at = true;
  EXPECT_TRUE(found_val_at);
}

TEST(CodeGen, MapForRangeKeyValue) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  m := {\"a\": 1, \"b\": 2}\n"
      "  for k, v : m {\n"
      "    intrinsic_print(\".\")\n"
      "  }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should call both mc_map_key_at and mc_map_value_at.
  bool found_key = false, found_val = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "saga_map_key_at")
          found_key = true;
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "saga_map_value_at")
          found_val = true;
      }
  EXPECT_TRUE(found_key);
  EXPECT_TRUE(found_val);
}

TEST(CodeGen, MapMultipleEntries) {
  // A map with multiple entries should call mc_map_set for each.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  m := {\"a\": 1, \"b\": 2, \"c\": 3}\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  int set_count = 0;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "saga_map_set")
          set_count++;
  EXPECT_EQ(set_count, 3);
}

TEST(CodeGen, MapStringKeyFlag) {
  // String-keyed maps should pass is_string_key=1 to saga_map_new.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  m := {\"key\": 42}\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "saga_map_new") {
          // arg2 = is_string_key
          auto *arg2 = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(2));
          ASSERT_NE(arg2, nullptr);
          EXPECT_EQ(arg2->getSExtValue(), 1) << "String key map should pass is_string_key=1";
        }
}

TEST(CodeGen, MapIntKeySizeEight) {
  // Int-keyed maps should pass 8 as key_size.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  m := {1: \"one\"}\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "saga_map_new") {
          auto *arg0 = llvm::dyn_cast<llvm::ConstantInt>(call->getArgOperand(0));
          ASSERT_NE(arg0, nullptr);
          EXPECT_EQ(arg0->getSExtValue(), 8) << "Int key map should pass 8";
        }
}

// ===========================================================================
// Closures / Function Expressions
// ===========================================================================

TEST(CodeGen, SimpleFuncExprCreatesClosureStruct) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  f := fn () Int { 42 }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have an alloca for the closure fat pointer.
  bool found_closure_alloca = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *a = llvm::dyn_cast<llvm::AllocaInst>(&inst))
        if (a->getAllocatedType() == r.codegen->closure_fat_ptr_type)
          found_closure_alloca = true;
  EXPECT_TRUE(found_closure_alloca)
      << "FuncExpr should create a closure fat pointer alloca";
}

TEST(CodeGen, FuncExprGeneratesTrampolineFunction) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  f := fn () Int { 42 }\n"
      "}");
  // Should have a trampoline function (internal linkage).
  bool found = false;
  for (auto &fn : r.mod()) {
    if (fn.getName().starts_with("mc.closure."))
      found = true;
  }
  EXPECT_TRUE(found) << "Should generate a trampoline function for the closure";
}

TEST(CodeGen, FuncExprCallProducesIndirectCall) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  f := fn () Int { 42 }\n"
      "  x := f()\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have an indirect call (CallInst with no getCalledFunction).
  bool found_indirect = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (!call->getCalledFunction())
          found_indirect = true;
  EXPECT_TRUE(found_indirect)
      << "Calling a closure variable should produce an indirect call";
}

TEST(CodeGen, FuncExprWithParams) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  add := fn (a, b Int) Int { a + b }\n"
      "  x := add(10, 32)\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // The trampoline should have 3 params: env + a + b.
  llvm::Function *tramp = nullptr;
  for (auto &fn : r.mod())
    if (fn.getName().starts_with("mc.closure."))
      tramp = &fn;
  ASSERT_NE(tramp, nullptr);
  EXPECT_EQ(tramp->arg_size(), 3u)
      << "Trampoline should have env + 2 params";
}

TEST(CodeGen, ClosureCapturesVariable) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 10\n"
      "  f := fn () Int { x }\n"
      "  y := f()\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have a GEP to store captured x into the env struct.
  bool found_env_gep = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&inst))
        if (gep->getName().str().find("env") != std::string::npos)
          found_env_gep = true;
  EXPECT_TRUE(found_env_gep) << "Closure should GEP into the env struct";
}

TEST(CodeGen, ClosureCapturesMultipleVars) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  a := 10\n"
      "  b := 20\n"
      "  f := fn () Int { a + b }\n"
      "  x := f()\n"
      "}");
  // The env struct should have 2 fields.
  llvm::StructType *env_st = nullptr;
  for (auto &fn : r.mod()) {
    if (fn.getName().starts_with("mc.closure.")) {
      // First param is env ptr.
      // Find the env struct type from GEP instructions in the body.
      for (auto &bb : fn)
        for (auto &inst : bb)
          if (auto *gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&inst))
            if (auto *st = llvm::dyn_cast<llvm::StructType>(
                    gep->getSourceElementType()))
              env_st = st;
    }
  }
  ASSERT_NE(env_st, nullptr);
  EXPECT_EQ(env_st->getNumElements(), 2u)
      << "Env struct should have 2 fields for 2 captures";
}

TEST(CodeGen, ClosureWithParamsAndCaptures) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  offset := 100\n"
      "  f := fn (x Int) Int { x + offset }\n"
      "  y := f(42)\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Trampoline should have 2 params: env + x.
  llvm::Function *tramp = nullptr;
  for (auto &fn : r.mod())
    if (fn.getName().starts_with("mc.closure."))
      tramp = &fn;
  ASSERT_NE(tramp, nullptr);
  EXPECT_EQ(tramp->arg_size(), 2u);
}

TEST(CodeGen, NoCaptureNoEnvStruct) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  f := fn (x Int) Int { x + 1 }\n"
      "  y := f(5)\n"
      "}");
  // No env struct should be created when there are no captures.
  bool found_env_struct = false;
  for (auto &st : r.mod().getIdentifiedStructTypes())
    if (st->getName().str().find(".env") != std::string::npos)
      found_env_struct = true;
  EXPECT_FALSE(found_env_struct)
      << "No env struct needed when nothing is captured";
}

TEST(CodeGen, ClosureReturnVoid) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  f := fn () Void {}\n"
      "  f()\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  llvm::Function *tramp = nullptr;
  for (auto &fn : r.mod())
    if (fn.getName().starts_with("mc.closure."))
      tramp = &fn;
  ASSERT_NE(tramp, nullptr);
  EXPECT_TRUE(tramp->getReturnType()->isVoidTy());
}

TEST(CodeGen, ClosureFatPtrTypeExists) {
  auto r = CG::from("pub fn Main() Void {}");
  auto *st = llvm::StructType::getTypeByName(
      r.mod().getContext(), "mc_closure");
  ASSERT_NE(st, nullptr);
  EXPECT_EQ(st->getNumElements(), 2u);
  EXPECT_TRUE(st->getElementType(0)->isPointerTy());
  EXPECT_TRUE(st->getElementType(1)->isPointerTy());
}

TEST(CodeGen, ClosureTrampolineHasInternalLinkage) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  f := fn () Int { 1 }\n"
      "}");
  for (auto &fn : r.mod()) {
    if (fn.getName().starts_with("mc.closure.")) {
      EXPECT_EQ(fn.getLinkage(), llvm::Function::InternalLinkage)
          << "Closure trampolines should have internal linkage";
    }
  }
}

TEST(CodeGen, ClosureStringCapture) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  msg := \"hello\"\n"
      "  f := fn () Void { intrinsic_print(msg) }\n"
      "  f()\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // The trampoline should call saga_intrinsic_print.
  llvm::Function *tramp = nullptr;
  for (auto &fn : r.mod())
    if (fn.getName().starts_with("mc.closure."))
      tramp = &fn;
  ASSERT_NE(tramp, nullptr);
  bool found_print = false;
  for (auto &bb : *tramp)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "saga_intrinsic_print")
          found_print = true;
  EXPECT_TRUE(found_print)
      << "Closure trampoline should call intrinsic_print with captured string";
}

TEST(CodeGen, PassClosureToFunction) {
  auto r = CG::from(
      "fn Apply(f fn(Int) Int, x Int) Int { f(x) }\n"
      "pub fn Main() Void {\n"
      "  double := fn (n Int) Int { n + n }\n"
      "  y := Apply(double, 21)\n"
      "}");
  auto *apply = r.func("Apply");
  ASSERT_NE(apply, nullptr);
  // Apply should have an indirect call (calling through the fn parameter).
  bool found_indirect = false;
  for (auto &bb : *apply)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (!call->getCalledFunction())
          found_indirect = true;
  EXPECT_TRUE(found_indirect)
      << "Apply should perform an indirect call through fn parameter";
}

// ===========================================================================
// Zero-value initialization for VarDecl without initializer
// ===========================================================================

TEST(CodeGen, VarDeclZeroInitInt) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x Int\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(has_alloca_named(main, "x"));
  // Should store a zero value (i64 0).
  EXPECT_GE(count_opcodes(main, llvm::Instruction::Store), 1);
}

TEST(CodeGen, VarDeclZeroInitFloat) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x Float\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(has_alloca_named(main, "x"));
  EXPECT_GE(count_opcodes(main, llvm::Instruction::Store), 1);
}

TEST(CodeGen, VarDeclZeroInitBool) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x Bool\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(has_alloca_named(main, "x"));
  EXPECT_GE(count_opcodes(main, llvm::Instruction::Store), 1);
}

TEST(CodeGen, VarDeclZeroInitString) {
  // String zero value should be an empty string "", not a null pointer.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  s String\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(has_alloca_named(main, "s"));
  // Should NOT store a null pointer. Instead, should reference a string
  // constant for the empty string "".
  bool found_null_store = false;
  bool found_store = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *si = llvm::dyn_cast<llvm::StoreInst>(&inst)) {
        if (auto *alloca = llvm::dyn_cast<llvm::AllocaInst>(si->getPointerOperand())) {
          if (alloca->getName() == "s") {
            found_store = true;
            if (llvm::isa<llvm::ConstantPointerNull>(si->getValueOperand()))
              found_null_store = true;
          }
        }
      }
  EXPECT_TRUE(found_store) << "Should store a value into s";
  EXPECT_FALSE(found_null_store) << "String zero value must not be null";
}

TEST(CodeGen, VarDeclZeroInitStringUsable) {
  // A zero-initialized string should be usable (e.g. passed to print).
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  s String\n"
      "  intrinsic_print(s)\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found_print = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "saga_intrinsic_print")
          found_print = true;
  EXPECT_TRUE(found_print);
}

TEST(CodeGen, VarDeclZeroInitStringReleased) {
  // A zero-initialized string should be released at function exit.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  s String\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found_release = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "saga_release_string")
          found_release = true;
  EXPECT_TRUE(found_release)
      << "Zero-initialized string should be released at scope exit";
}

TEST(CodeGen, VarDeclZeroInitStruct) {
  // Struct zero value: all fields should be zeroed.
  auto r = CG::from(
      "struct Point { x, y Int }\n"
      "pub fn Main() Void {\n"
      "  p Point\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Should have an alloca for the struct.
  bool found_struct_alloca = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *a = llvm::dyn_cast<llvm::AllocaInst>(&inst))
        if (a->getAllocatedType()->isStructTy())
          found_struct_alloca = true;
  EXPECT_TRUE(found_struct_alloca)
      << "Struct VarDecl should allocate the struct type";
}

TEST(CodeGen, VarDeclZeroInitStructFieldAccess) {
  // After zero init, struct fields should be accessible and have zero values.
  auto r = CG::from(
      "struct Point { x, y Int }\n"
      "pub fn Main() Void {\n"
      "  p Point\n"
      "  a := p.x\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(has_alloca_named(main, "a"));
  // Should have a GEP to access p.x.
  int gep_count = 0;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (llvm::isa<llvm::GetElementPtrInst>(&inst))
        gep_count++;
  EXPECT_GE(gep_count, 1) << "Should GEP into zero-initialized struct";
}

TEST(CodeGen, VarDeclZeroInitIntWithExplicit) {
  // VarDecl with explicit type AND initializer should still work.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x Int = 42\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(has_alloca_named(main, "x"));
}

TEST(CodeGen, VarDeclZeroInitStringWithExplicit) {
  // VarDecl with explicit String type and initializer.
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  s String = \"hello\"\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  EXPECT_TRUE(has_alloca_named(main, "s"));
}

// ===========================================================================
// Intrinsics — yield, atomic_add, trap
// ===========================================================================

// Helper: check if any function in the module contains a call to a named fn.
static bool module_has_call_to(llvm::Module &mod, const std::string &callee_name) {
  for (auto &fn : mod)
    for (auto &bb : fn)
      for (auto &inst : bb)
        if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
          if (call->getCalledFunction() &&
              call->getCalledFunction()->getName() == callee_name)
            return true;
  return false;
}

// Helper: check if any function in the module contains an atomicrmw instruction.
static bool module_has_atomicrmw(llvm::Module &mod) {
  for (auto &fn : mod)
    for (auto &bb : fn)
      for (auto &inst : bb)
        if (llvm::isa<llvm::AtomicRMWInst>(&inst))
          return true;
  return false;
}

TEST(CodeGen, IntrinsicYieldInsideSpawn) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  t := spawn |ctx| {\n"
      "    intrinsic_yield()\n"
      "    42\n"
      "  }\n"
      "}");
  // The outlined spawn body should call saga_actor_yield.
  EXPECT_TRUE(module_has_call_to(r.mod(), "saga_actor_yield"));
}

TEST(CodeGen, IntrinsicYieldOutsideSpawnIsNoop) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  intrinsic_yield()\n"
      "}");
  // Outside a spawn block, yield is a no-op — no call emitted.
  EXPECT_FALSE(module_has_call_to(r.mod(), "saga_actor_yield"));
}

TEST(CodeGen, IntrinsicTrapInsideSpawn) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  t := spawn |ctx| {\n"
      "    intrinsic_trap(\"fatal\")\n"
      "    42\n"
      "  }\n"
      "}");
  EXPECT_TRUE(module_has_call_to(r.mod(), "saga_actor_trap"));
}

TEST(CodeGen, IntrinsicTrapOutsideSpawnIsNoop) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  intrinsic_trap(\"oops\")\n"
      "}");
  // Outside a spawn block, trap is a no-op.
  EXPECT_FALSE(module_has_call_to(r.mod(), "saga_actor_trap"));
}

TEST(CodeGen, IntrinsicAtomicAdd) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  x := 0\n"
      "  old := intrinsic_atomic_add(x, 1)\n"
      "}");
  // Should produce an atomicrmw add instruction.
  EXPECT_TRUE(module_has_atomicrmw(r.mod()));
}

// ===========================================================================
// Spawn codegen — basic structure
// ===========================================================================

TEST(CodeGen, SpawnBasicEmitsExecutorInit) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  t := spawn { 42 }\n"
      "}");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  // Main should call saga_executor_init and saga_executor_shutdown.
  EXPECT_TRUE(module_has_call_to(r.mod(), "saga_executor_init"));
  EXPECT_TRUE(module_has_call_to(r.mod(), "saga_executor_shutdown"));
}

TEST(CodeGen, SpawnEmitsOutlinedFunction) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  t := spawn { 42 }\n"
      "}");
  // An outlined function should exist (named spawn_entry or similar).
  bool found_outlined = false;
  for (auto &fn : r.mod()) {
    if (fn.getName().starts_with("mc.spawn."))
      found_outlined = true;
  }
  EXPECT_TRUE(found_outlined);
}

TEST(CodeGen, SpawnEmitsExecutorSpawnCall) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  t := spawn { 42 }\n"
      "}");
  EXPECT_TRUE(module_has_call_to(r.mod(), "saga_executor_spawn"));
}

TEST(CodeGen, SpawnWithPipeVariable) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  t := spawn |ctx| {\n"
      "    ctx.Cancelled?()\n"
      "    42\n"
      "  }\n"
      "}");
  // Should call saga_context_cancelled in the outlined function.
  EXPECT_TRUE(module_has_call_to(r.mod(), "saga_context_cancelled"));
}

TEST(CodeGen, SpawnReductionTickInLoop) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  t := spawn {\n"
      "    i := 0\n"
      "    for i < 10 {\n"
      "      i = i + 1\n"
      "    }\n"
      "    i\n"
      "  }\n"
      "}");
  // Loop inside spawn should emit saga_reduction_tick calls.
  EXPECT_TRUE(module_has_call_to(r.mod(), "saga_reduction_tick"));
}

TEST(CodeGen, NoReductionTickOutsideSpawn) {
  auto r = CG::from(
      "pub fn Main() Void {\n"
      "  i := 0\n"
      "  for i < 10 {\n"
      "    i = i + 1\n"
      "  }\n"
      "}");
  // Loop outside spawn should NOT emit saga_reduction_tick.
  EXPECT_FALSE(module_has_call_to(r.mod(), "saga_reduction_tick"));
}

// ===========================================================================
// Cross-package codegen (module function calls)
// ===========================================================================

TEST(CodeGen, ModuleFuncCallDeclared) {
  // When calling a module function, declare_import should create an
  // external declaration with the mangled name.
  FileSet fs;
  fs.add_file(File::from_source("test.sg",
      "import \"mathlib\"\n"
      "pub fn Main() Void {\n"
      "  x := mathlib.Add(1, 2)\n"
      "}"));

  Parser parser(fs);
  auto ast = parser.parse();
  ASSERT_NE(ast, nullptr);
  EXPECT_TRUE(parser.errors.errors.empty());

  Analyzer analyzer(fs);
  // Mock the module so we don't need real files.
  auto mod_type = make_module_type("mathlib", "mathlib", {
      {"Add", make_func_type({make_int_type(), make_int_type()},
                              {make_int_type()})},
  });
  analyzer.package_resolver->mock_packages["mathlib"] = mod_type;
  analyzer.analyze(*ast);
  EXPECT_TRUE(analyzer.errors.errors.empty())
      << "Analyzer errors";
  for (auto &e : analyzer.errors.errors)
    std::cerr << "  " << e.message << "\n";

  CodeGen codegen("testpkg", analyzer);
  codegen.emit(*ast);

  // The external function should be declared with mangled name.
  auto *ext = codegen.module->getFunction("mathlib__Add");
  ASSERT_NE(ext, nullptr) << "Should have extern decl for mathlib__Add";
  EXPECT_TRUE(ext->isDeclaration()) << "Should be a declaration, not definition";
  EXPECT_EQ(ext->arg_size(), 2u);
  EXPECT_TRUE(ext->getReturnType()->isIntegerTy(64));

  // main() should contain a call to mathlib__Add.
  auto *main_fn = codegen.module->getFunction("main");
  ASSERT_NE(main_fn, nullptr);
  bool found = false;
  for (auto &bb : *main_fn)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName() == "mathlib__Add")
          found = true;
  EXPECT_TRUE(found) << "Main should call mathlib__Add";
}

TEST(CodeGen, ModuleFuncCallVoid) {
  FileSet fs;
  fs.add_file(File::from_source("test.sg",
      "import \"printer\"\n"
      "pub fn Main() Void {\n"
      "  printer.Print(\"hello\")\n"
      "}"));

  Parser parser(fs);
  auto ast = parser.parse();
  ASSERT_NE(ast, nullptr);

  Analyzer analyzer(fs);
  auto mod_type = make_module_type("printer", "printer", {
      {"Print", make_func_type({make_string_type()}, {make_void_type()})},
  });
  analyzer.package_resolver->mock_packages["printer"] = mod_type;
  analyzer.analyze(*ast);
  EXPECT_TRUE(analyzer.errors.errors.empty());

  CodeGen codegen("testpkg", analyzer);
  codegen.emit(*ast);

  auto *ext = codegen.module->getFunction("printer__Print");
  ASSERT_NE(ext, nullptr);
  EXPECT_TRUE(ext->getReturnType()->isVoidTy());
}

TEST(CodeGen, MangleNames) {
  // Test the mangle static method directly.
  EXPECT_EQ(CodeGen::mangle("io", "Println"), "io__Println");
  EXPECT_EQ(CodeGen::mangle("os", "File__Write"), "os__File__Write");
  EXPECT_EQ(CodeGen::mangle("mylib", "Add"), "mylib__Add");
}

TEST(CodeGen, LocalFuncsMangledWithPackage) {
  // Local functions should use the package name in their mangled link name.
  auto r = CG::from(
      "fn Helper() Int { 42 }\n"
      "pub fn Main() Void {\n"
      "  x := Helper()\n"
      "}");
  // "test" is the package name in CG::from.
  auto *helper = r.codegen->module->getFunction("test__Helper");
  ASSERT_NE(helper, nullptr) << "Helper should be mangled as test__Helper";
  // Main should still be 'main' (not mangled).
  auto *main_fn = r.func("main");
  ASSERT_NE(main_fn, nullptr);
}

// ===========================================================================
// Automatic closing — structs implementing Closer (Close() Void)
// ===========================================================================

TEST(CodeGen, AutoCloseCalledAtFuncExit) {
  auto r = CG::from(
      "struct Res {\n"
      "  n Int\n"
      "  pub fn Close() Void { }\n"
      "}\n"
      "pub fn Main() Void {\n"
      "  r := Res{n: 1}\n"
      "}\n");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName().contains("Res__Close"))
          found = true;
  EXPECT_TRUE(found)
      << "Struct with Close() should have Close called at function exit";
}

TEST(CodeGen, AutoCloseCalledBeforeReturn) {
  auto r = CG::from(
      "struct Handle {\n"
      "  x Int\n"
      "  pub fn Close() Void { }\n"
      "}\n"
      "fn f() Int {\n"
      "  h := Handle{x: 42}\n"
      "  return 1\n"
      "}\n");
  auto *fn = r.func("f");
  ASSERT_NE(fn, nullptr);
  bool found = false;
  for (auto &bb : *fn)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName().contains("Handle__Close"))
          found = true;
  EXPECT_TRUE(found)
      << "Close should be called before explicit return";
}

TEST(CodeGen, NonCloseableStructNotClosed) {
  auto r = CG::from(
      "struct Plain { n Int }\n"
      "pub fn Main() Void {\n"
      "  p := Plain{n: 1}\n"
      "}\n");
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  bool found = false;
  for (auto &bb : *main)
    for (auto &inst : bb)
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst))
        if (call->getCalledFunction() &&
            call->getCalledFunction()->getName().contains("Close"))
          found = true;
  EXPECT_FALSE(found)
      << "Struct without Close() should NOT have auto-close calls";
}

TEST(CodeGen, AutoCloseMethodDeclared) {
  auto r = CG::from(
      "struct Res {\n"
      "  n Int\n"
      "  pub fn Close() Void { }\n"
      "}\n"
      "pub fn Main() Void {\n"
      "  r := Res{n: 1}\n"
      "}\n");
  // The Close function should exist (either declared or defined) in the module.
  bool found = false;
  for (auto &fn : r.mod()) {
    if (fn.getName().contains("Res__Close")) {
      found = true;
      break;
    }
  }
  EXPECT_TRUE(found) << "Close method should be declared in the module";
}

// ===========================================================================
// Intrinsic type receiver methods
// ===========================================================================

TEST(CodeGen, IntrinsicMethodDeclared) {
  auto r = CG::from(
      "fn (self Int) Double() Int { self * 2 }\n"
      "pub fn Main() Void {}", true);
  // The method should be declared with mangled name test__Int__Double.
  auto *func = r.mod().getFunction("test__Int__Double");
  ASSERT_NE(func, nullptr);
  // First param is i64 (self), return type is i64.
  EXPECT_EQ(func->arg_size(), 1u);
  EXPECT_TRUE(func->getArg(0)->getType()->isIntegerTy(64));
  EXPECT_TRUE(func->getReturnType()->isIntegerTy(64));
}

TEST(CodeGen, IntrinsicMethodHasBody) {
  auto r = CG::from(
      "fn (self Int) Double() Int { self * 2 }\n"
      "pub fn Main() Void {}", true);
  auto *func = r.mod().getFunction("test__Int__Double");
  ASSERT_NE(func, nullptr);
  EXPECT_FALSE(func->empty()) << "Method should have a body";
}

TEST(CodeGen, IntrinsicMethodCalledCorrectly) {
  auto r = CG::from(
      "fn (self Int) Double() Int { self * 2 }\n"
      "pub fn Main() Void {\n"
      "  x := 5\n"
      "  y := x.Double()\n"
      "}", true);
  // Verify Main exists and the method function exists.
  auto *main = r.func("main");
  ASSERT_NE(main, nullptr);
  auto *method = r.mod().getFunction("test__Int__Double");
  ASSERT_NE(method, nullptr);

  // Find a call to Int__Double in main's body.
  bool found_call = false;
  for (auto &bb : *main) {
    for (auto &inst : bb) {
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        if (call->getCalledFunction() == method) {
          found_call = true;
          break;
        }
      }
    }
    if (found_call) break;
  }
  EXPECT_TRUE(found_call) << "Main should call Int__Double";
}

TEST(CodeGen, StringIntrinsicMethodDeclared) {
  auto r = CG::from(
      "fn (self String) TestSize() Int { 0 }\n"
      "pub fn Main() Void {}", true);
  auto *func = r.mod().getFunction("test__String__TestSize");
  ASSERT_NE(func, nullptr);
  // First param is ptr (String = mc_string*), return type is i64.
  EXPECT_EQ(func->arg_size(), 1u);
  EXPECT_TRUE(func->getArg(0)->getType()->isPointerTy());
  EXPECT_TRUE(func->getReturnType()->isIntegerTy(64));
}

TEST(CodeGen, IntrinsicMethodNotDeclaredAsRegularFunc) {
  auto r = CG::from(
      "fn (self Int) Double() Int { self * 2 }\n"
      "pub fn Main() Void {}", true);
  // Should NOT be declared as a regular function (test__Double).
  auto *regular = r.mod().getFunction("test__Double");
  EXPECT_EQ(regular, nullptr)
      << "Intrinsic receiver method should not be declared as a regular function";
}

// ===========================================================================
// Phase 2: New stdlib intrinsics
// ===========================================================================

// Helper to find a call to a named function inside a given function.
static llvm::CallInst *find_call(llvm::Function *fn, const std::string &callee_name) {
  if (!fn) return nullptr;
  for (auto &bb : *fn) {
    for (auto &inst : bb) {
      if (auto *call = llvm::dyn_cast<llvm::CallInst>(&inst)) {
        if (auto *cf = call->getCalledFunction()) {
          if (cf->getName() == callee_name)
            return call;
        }
      }
    }
  }
  return nullptr;
}

// Helper to find the first instruction of a given opcode inside a function.
template <typename InstTy>
static InstTy *find_inst(llvm::Function *fn) {
  if (!fn) return nullptr;
  for (auto &bb : *fn) {
    for (auto &inst : bb) {
      if (auto *t = llvm::dyn_cast<InstTy>(&inst))
        return t;
    }
  }
  return nullptr;
}

TEST(CodeGen, IntrinsicSitofpProducesSIToFP) {
  auto r = CG::from(
      "fn f(x Int) Float { intrinsic_sitofp(x) }\n"
      "pub fn Main() Void {}");
  auto *func = r.func("f");
  ASSERT_NE(func, nullptr);
  // Should contain an sitofp instruction.
  auto *conv = find_inst<llvm::SIToFPInst>(func);
  EXPECT_NE(conv, nullptr) << "Expected sitofp instruction";
  if (conv) {
    EXPECT_TRUE(conv->getType()->isDoubleTy());
  }
}

TEST(CodeGen, IntrinsicFptosiProducesFPToSI) {
  auto r = CG::from(
      "fn f(x Float) Int { intrinsic_fptosi(x) }\n"
      "pub fn Main() Void {}");
  auto *func = r.func("f");
  ASSERT_NE(func, nullptr);
  auto *conv = find_inst<llvm::FPToSIInst>(func);
  EXPECT_NE(conv, nullptr) << "Expected fptosi instruction";
  if (conv) {
    EXPECT_TRUE(conv->getType()->isIntegerTy(64));
  }
}

TEST(CodeGen, IntrinsicZextWidens) {
  auto r = CG::from(
      "fn f(x Int) Int { intrinsic_zext(x, 32) }\n"
      "pub fn Main() Void {}");
  auto *func = r.func("f");
  ASSERT_NE(func, nullptr);
  // Should contain a trunc (64→32) instruction since we're narrowing.
  auto *trunc = find_inst<llvm::TruncInst>(func);
  EXPECT_NE(trunc, nullptr) << "Expected trunc instruction for zext(i64, 32)";
}

TEST(CodeGen, IntrinsicSextWidens) {
  auto r = CG::from(
      "fn f(x Int) Int { intrinsic_sext(x, 32) }\n"
      "pub fn Main() Void {}");
  auto *func = r.func("f");
  ASSERT_NE(func, nullptr);
  auto *trunc = find_inst<llvm::TruncInst>(func);
  EXPECT_NE(trunc, nullptr) << "Expected trunc instruction for sext(i64, 32)";
}

TEST(CodeGen, IntrinsicRuntimeCallsNamedFunction) {
  auto r = CG::from(
      "fn f(x Int) String { intrinsic_runtime(\"saga_int_to_string\", x) }\n"
      "pub fn Main() Void {}");
  auto *func = r.func("f");
  ASSERT_NE(func, nullptr);
  auto *call = find_call(func, "saga_int_to_string");
  EXPECT_NE(call, nullptr) << "Expected call to saga_int_to_string";
}

TEST(CodeGen, IntrinsicRuntimeAutoPromotesScalar) {
  // saga_array_push expects (ptr, ptr) — the second ptr needs auto-promotion.
  auto r = CG::from(
      "fn f(arr [Int], val Int) Void {\n"
      "  intrinsic_runtime(\"saga_array_push\", arr, val)\n"
      "}\n"
      "pub fn Main() Void {}");
  auto *func = r.func("f");
  ASSERT_NE(func, nullptr);
  auto *call = find_call(func, "saga_array_push");
  EXPECT_NE(call, nullptr) << "Expected call to saga_array_push";
  if (call) {
    // Both args should be pointers (second was auto-promoted from i64).
    EXPECT_TRUE(call->getArgOperand(1)->getType()->isPointerTy())
        << "Second arg should be auto-promoted to pointer";
  }
}

TEST(CodeGen, IntrinsicFieldLoadsFromStruct) {
  auto r = CG::from(
      "fn f(s String) Int { intrinsic_field(s, 1) }\n"
      "pub fn Main() Void {}");
  auto *func = r.func("f");
  ASSERT_NE(func, nullptr);
  // Should contain a GEP + load.
  auto *load = find_inst<llvm::LoadInst>(func);
  EXPECT_NE(load, nullptr) << "Expected load instruction from field access";
}

TEST(CodeGen, IntrinsicCallGateRejectsNonStdlib) {
  // When is_stdlib = false, intrinsic_* calls should be rejected by the analyzer.
  CG r;
  r.fileset.add_file(File::from_source("test.sg",
      "fn f(x Int) Float { intrinsic_sitofp(x) }\n"
      "pub fn Main() Void {}"));
  Parser parser(r.fileset);
  r.ast = parser.parse();
  ASSERT_NE(r.ast, nullptr);
  r.analyzer = std::make_unique<Analyzer>(r.fileset);
  r.analyzer->is_stdlib = false;
  r.analyzer->analyze(*r.ast);
  EXPECT_FALSE(r.analyzer->errors.errors.empty())
      << "Non-stdlib intrinsic call should be rejected";
  bool found = false;
  for (auto &e : r.analyzer->errors.errors)
    if (e.message.find("only be called from stdlib") != std::string::npos)
      found = true;
  EXPECT_TRUE(found) << "Should have a 'stdlib only' error message";
}

} // namespace mc






