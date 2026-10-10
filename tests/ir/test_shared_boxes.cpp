// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// An error, and a union alternative that contains itself, live in a shared
// box: counted, freed with what it holds once the last reference goes, and
// handed between an `or` or a `?` and the root expression it escapes to with
// exactly one reference.

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

const char *kNode = "struct Node {\n"
                    "  name string\n"
                    "  tail Node | Missing\n"
                    "}\n";

bool calls_walk(llvm::Function *fn, llvm::StringRef walk) {
  return !calls_to(fn, walk).empty();
}

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

TEST(SharedBoxes, SelfContainingAlternativeIsASharedBox) {
  auto r = CG::from(std::string(kNode) +
                    "fn wrap(n Node) Node | Missing { n }\n"
                    "pub fn Main() void {\n"
                    "  _ := wrap(Node{name: \"a\", tail: Missing{}})\n"
                    "}");
  auto made = calls_to(r.func("wrap"), "saga_shared_new");
  ASSERT_EQ(made.size(), 1u);
  EXPECT_TRUE(made[0]->getArgOperand(1)->getName().contains("Node"));
}

TEST(SharedBoxes, UnionReleaseReleasesItsBox) {
  auto r = CG::from(std::string(kNode) +
                    "pub fn Main() void {\n"
                    "  _ := Node{name: \"a\", tail: Missing{}}\n"
                    "}");
  llvm::Function *walk = nullptr;
  for (auto &fn : r.mod())
    if (fn.getName().contains("Node | ") && fn.getName().ends_with("__release"))
      walk = &fn;
  ASSERT_NE(walk, nullptr);
  EXPECT_TRUE(calls_walk(walk, "saga_shared_release"));
}

// The box takes over what it is given, so a borrowed value is retained first.
TEST(SharedBoxes, BorrowedValueBoxedTakesItsOwnReferences) {
  auto r = CG::from(std::string(kNode) +
                    "fn wrap(n Node) Node | Missing { n }\n"
                    "pub fn Main() void {\n"
                    "  _ := wrap(Node{name: \"a\", tail: Missing{}})\n"
                    "}");
  EXPECT_TRUE(calls_walk(r.func("wrap"), "Node__retain_fields"));
}

// A node taken out of a call's result keeps its own references, and the box
// goes with the result.
TEST(SharedBoxes, ValueTakenOutOfAnOwnedUnionLeavesTheBox) {
  auto r = CG::from(std::string(kNode) +
                    "fn mk() Node | Missing { Missing{} }\n"
                    "fn f() Node { mk() or { Node{name: \"z\"} } }\n"
                    "pub fn Main() void { _ := f() }");
  bool released = false;
  for (auto *call : calls_to(r.func("f"), "__release"))
    if (call->getParent()->getName().starts_with("or.ok"))
      released = true;
  EXPECT_TRUE(calls_walk(r.func("f"), "Node__retain_fields"));
  EXPECT_TRUE(released);
}

TEST(SharedBoxes, BorrowedUnionKeepsItsBox) {
  auto r = CG::from(std::string(kNode) +
                    "fn f(u Node | Missing) Node {\n"
                    "  u or { Node{name: \"z\"} }\n"
                    "}\n"
                    "pub fn Main() void { _ := f(Missing{}) }");
  for (auto *call : calls_to(r.func("f"), "__release"))
    if (call->getParent()->getName().starts_with("or.ok"))
      EXPECT_FALSE(call->getCalledFunction()->getName().contains("Node | "));
}

// An element read out of an array is boxed into the lookup's union, and the
// box needs references of its own.
TEST(SharedBoxes, LookupBoxesItsOwnCopy) {
  auto r = CG::from(std::string(kNode) +
                    "fn f(nodes array{Node}) Node {\n"
                    "  nodes[0] or { Node{name: \"z\"} }\n"
                    "}\n"
                    "pub fn Main() void { _ := f([]) }");
  EXPECT_TRUE(calls_walk(r.func("f"), "Node__retain_fields"));
}

} // namespace saga
