// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// An error lives in a shared box: counted, freed with what it holds once the
// last reference goes, and handed between an `or` or a `?` and the root
// expression it escapes to with exactly one reference.

#include "codegen_harness.hpp"
#include "ir_queries.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>

namespace saga {

namespace {

const char *kBad = "error Bad { code int }\n"
                   "fn g(n int) int | error {\n"
                   "  if n > 0 { return Bad{code: n} }\n"
                   "  n\n"
                   "}\n";

// The error an `or` handler or a `?` reads out of the union: in the handler,
// or in the escape to the root's landing.
llvm::LoadInst *error_read(llvm::Function *fn, bool in_handler) {
  for (auto &bb : *fn)
    if (bb.getName().starts_with("or.err") == in_handler)
      for (auto &inst : bb)
        if (auto *load = llvm::dyn_cast<llvm::LoadInst>(&inst))
          if (load->getName().starts_with("err.payload.val"))
            return load;
  return nullptr;
}

} // namespace

TEST(SharedBoxes, ErrorLiteralIsASharedBox) {
  auto r = CG::from(std::string(kBad) + "pub fn Main() void { _ := g(1) }");
  auto made = calls_to(r.func("g"), "saga_shared_new");
  ASSERT_EQ(made.size(), 1u);
  auto *ops = llvm::dyn_cast<llvm::GlobalVariable>(made[0]->getArgOperand(1));
  ASSERT_NE(ops, nullptr);
  auto *table = llvm::cast<llvm::ConstantStruct>(ops->getInitializer());
  EXPECT_TRUE(table->getOperand(1)->getName().ends_with("__release_fields"));
}

TEST(SharedBoxes, ConstantErrorIsNeverFreed) {
  auto r = CG::from("fn f(xs array{int}) int { xs[3] or { 0 } }\n"
                    "pub fn Main() void { _ := f([1]) }");
  llvm::GlobalVariable *singleton = nullptr;
  for (auto &global : r.mod().globals())
    if (global.getName().contains(".singleton"))
      singleton = &global;
  ASSERT_NE(singleton, nullptr);
  auto *box = llvm::cast<llvm::ConstantStruct>(singleton->getInitializer());
  auto *count = llvm::cast<llvm::ConstantInt>(box->getOperand(0));
  EXPECT_EQ(count->getSExtValue(), -1);
}

TEST(SharedBoxes, OwnedErrorIsHeldPastTheHandler) {
  auto r = CG::from(std::string(kBad) +
                    "fn f(n int) int {\n"
                    "  g(n) or |err| { err.message.Size() }\n"
                    "}\n"
                    "pub fn Main() void { _ := f(1) }");
  auto *error = error_read(r.func("f"), true);
  ASSERT_NE(error, nullptr);
  EXPECT_TRUE(released_through_slot(error, "saga_shared_release"));
}

TEST(SharedBoxes, BorrowedErrorIsNotReleasedByItsHandler) {
  auto r = CG::from("fn f(u int | error) int {\n"
                    "  u or |err| { err.message.Size() }\n"
                    "}\n"
                    "pub fn Main() void { _ := f(1) }");
  auto *error = error_read(r.func("f"), true);
  ASSERT_NE(error, nullptr);
  EXPECT_FALSE(released_through_slot(error, "saga_shared_release"));
}

// A call's value is owned, so the error escaping in its place must be too.
TEST(SharedBoxes, BorrowedErrorRaisedIntoAnOwnedRootIsRetained) {
  auto r = CG::from("fn take(n int) int { n }\n"
                    "fn f(u int | error) int { take(u) or { 0 } }\n"
                    "pub fn Main() void { _ := f(1) }");
  auto *error = error_read(r.func("f"), false);
  ASSERT_NE(error, nullptr);
  EXPECT_TRUE(released_directly(error, "saga_shared_retain"));
}

// An element read out of an array is borrowed, so the error escaping in its
// place is held for the statement instead of handed over.
TEST(SharedBoxes, OwnedErrorRaisedIntoABorrowedRootIsHeld) {
  auto r = CG::from(std::string(kBad) +
                    "fn f(names array{string}, n int) string {\n"
                    "  names.At(g(n)) or { \"\" }\n"
                    "}\n"
                    "pub fn Main() void { _ := f([\"a\"], 0) }");
  auto *error = error_read(r.func("f"), false);
  ASSERT_NE(error, nullptr);
  EXPECT_FALSE(released_directly(error, "saga_shared_retain"));
  EXPECT_TRUE(released_through_slot(error, "saga_shared_release"));
}

} // namespace saga
