// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

// Where owned values end: a local with its block, a temporary — an owned value
// its consumer only reads, held in a slot of its own — with its full
// expression.

#include "codegen_harness.hpp"
#include "ir_queries.hpp"

namespace saga {

TEST(Temporaries, ReceiverIsReleased) {
  auto r = CG::from("fn f(s string) int { s.Upper().Size() }\n"
                    "pub fn Main() void { _ := f(\"x\") }");
  auto upper = calls_to(r.func("f"), "Upper");
  ASSERT_EQ(upper.size(), 1u);
  EXPECT_TRUE(released_through_slot(upper[0], "saga_release_string"));
}

TEST(Temporaries, DiscardedResultIsReleased) {
  auto r = CG::from("fn f(s string) int {\n"
                    "  s.Upper()\n"
                    "  0\n"
                    "}\n"
                    "pub fn Main() void { _ := f(\"x\") }");
  auto upper = calls_to(r.func("f"), "Upper");
  ASSERT_EQ(upper.size(), 1u);
  EXPECT_TRUE(released_through_slot(upper[0], "saga_release_string"));
}

TEST(Temporaries, OperandIsReleased) {
  auto r = CG::from("fn f(s string) bool { s.Upper() == \"X\" }\n"
                    "pub fn Main() void { _ := f(\"x\") }");
  auto upper = calls_to(r.func("f"), "Upper");
  ASSERT_EQ(upper.size(), 1u);
  EXPECT_TRUE(released_through_slot(upper[0], "saga_release_string"));
}

// The element read out of the array is retained before the array goes.
TEST(Temporaries, IndexedCollectionOutlivesTheRead) {
  auto r = CG::from("fn mk() array{string} { [\"a\"] }\n"
                    "fn f() string { mk()[0] or { \"\" } }\n"
                    "pub fn Main() void { _ := f() }");
  auto *f = r.func("f");
  auto mk = calls_to(f, "mk");
  ASSERT_EQ(mk.size(), 1u);
  EXPECT_TRUE(released_through_slot(mk[0], "saga_release_array"));

  auto retains = calls_to(f, "saga_retain_string");
  auto releases = calls_to(f, "saga_release_array");
  ASSERT_EQ(retains.size(), 1u);
  ASSERT_EQ(releases.size(), 1u);
  ASSERT_EQ(retains[0]->getParent(), releases[0]->getParent());
  EXPECT_TRUE(retains[0]->comesBefore(releases[0]));
}

TEST(Temporaries, InterpolationReleasesThePiecesItMade) {
  auto r = CG::from("fn f(n int) string { \"a{n}b{n}\" }\n"
                    "pub fn Main() void { _ := f(1) }");
  auto *f = r.func("f");
  for (auto *conv : calls_to(f, "saga_int_to_string"))
    EXPECT_TRUE(released_directly(conv, "saga_release_string"));
  auto concats = calls_to(f, "saga_string_concat");
  ASSERT_EQ(concats.size(), 3u);
  EXPECT_TRUE(released_directly(concats[0], "saga_release_string"));
  EXPECT_TRUE(released_directly(concats[1], "saga_release_string"));
  EXPECT_FALSE(released_directly(concats[2], "saga_release_string"));
}

// A value made in one branch is released after the merge, so its slot is null
// on every path that did not make it.
TEST(Temporaries, SlotIsNullUntilTheValueIsMade) {
  auto r = CG::from("fn f(s string, c bool) int {\n"
                    "  if c { s.Upper().Size() } else { 0 }\n"
                    "}\n"
                    "pub fn Main() void { _ := f(\"x\", true) }");
  auto *f = r.func("f");
  auto upper = calls_to(f, "Upper");
  ASSERT_EQ(upper.size(), 1u);
  llvm::Value *slot = nullptr;
  for (auto *user : upper[0]->users())
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(user))
      slot = store->getPointerOperand();
  ASSERT_NE(slot, nullptr);

  bool nulled_at_entry = false;
  for (auto &inst : f->getEntryBlock())
    if (auto *store = llvm::dyn_cast<llvm::StoreInst>(&inst))
      if (store->getPointerOperand() == slot &&
          llvm::isa<llvm::ConstantPointerNull>(store->getValueOperand()))
        nulled_at_entry = true;
  EXPECT_TRUE(nulled_at_entry);
}

// Held by address, so a method that writes through the temporary is seen by
// the release.
TEST(Temporaries, AggregateIsHeldByAddress) {
  auto r = CG::from("struct P { n string }\n"
                    "fn (p P) Len() int { p.n.Size() }\n"
                    "fn mk() P { P{n: \"a\"} }\n"
                    "fn f() int { mk().Len() }\n"
                    "pub fn Main() void { _ := f() }");
  auto mk = calls_to(r.func("f"), "mk");
  ASSERT_EQ(mk.size(), 1u);
  EXPECT_TRUE(released_through_slot(mk[0]->getArgOperand(0),
                                    "release_fields"));
}

TEST(Temporaries, WalkOfNullIsANoOp) {
  auto r = CG::from("struct P { n string }\n"
                    "fn mk() P { P{n: \"a\"} }\n"
                    "pub fn Main() void { _ := mk().n.Size() }");
  llvm::Function *walk = nullptr;
  for (auto &fn : r.mod())
    if (fn.getName().ends_with("__release_fields"))
      walk = &fn;
  ASSERT_NE(walk, nullptr);
  auto *branch =
      llvm::dyn_cast<llvm::BranchInst>(walk->getEntryBlock().getTerminator());
  ASSERT_NE(branch, nullptr);
  ASSERT_TRUE(branch->isConditional());
  auto *test = llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition());
  ASSERT_NE(test, nullptr);
  EXPECT_EQ(test->getOperand(0), walk->getArg(0));
}

TEST(Scopes, LoopBodyLocalEndsEachPass) {
  auto r = CG::from("fn f() int {\n"
                    "  n := 0\n"
                    "  for i : 0..3 {\n"
                    "    s := \"x{i}\"\n"
                    "    n += s.Size()\n"
                    "  }\n"
                    "  n\n"
                    "}\n"
                    "pub fn Main() void { _ := f() }");
  auto *f = r.func("f");
  auto *slot = local_slot(f, "s");
  ASSERT_NE(slot, nullptr);
  auto releases = calls_on_slot(slot, "saga_release_string");
  ASSERT_EQ(releases.size(), 1u);
  EXPECT_TRUE(branches_to(releases[0]->getParent(), "for.update"));
}

// Released only where it was declared, never at the function's exit, where a
// path that skipped the branch would find its slot unset.
TEST(Scopes, BranchLocalEndsWithTheBranch) {
  auto r = CG::from("fn f(c bool) int {\n"
                    "  if c {\n"
                    "    s := \"x{c}\"\n"
                    "    _ := s.Size()\n"
                    "  }\n"
                    "  0\n"
                    "}\n"
                    "pub fn Main() void { _ := f(true) }");
  auto *f = r.func("f");
  auto *slot = local_slot(f, "s");
  ASSERT_NE(slot, nullptr);
  auto releases = calls_on_slot(slot, "saga_release_string");
  ASSERT_EQ(releases.size(), 1u);
  EXPECT_TRUE(branches_to(releases[0]->getParent(), "merge"));
}

TEST(Scopes, JoinRetainsAValueReadOutOfABranchLocal) {
  auto r = CG::from("fn f(c bool) string {\n"
                    "  if c {\n"
                    "    t := \"x{c}\"\n"
                    "    t\n"
                    "  } else {\n"
                    "    \"y\"\n"
                    "  }\n"
                    "}\n"
                    "pub fn Main() void { _ := f(true) }");
  auto *f = r.func("f");
  auto *slot = local_slot(f, "t");
  ASSERT_NE(slot, nullptr);
  auto retains = calls_on_slot(slot, "saga_retain_string");
  auto releases = calls_on_slot(slot, "saga_release_string");
  ASSERT_EQ(retains.size(), 1u);
  ASSERT_EQ(releases.size(), 1u);
  ASSERT_EQ(retains[0]->getParent(), releases[0]->getParent());
  EXPECT_TRUE(retains[0]->comesBefore(releases[0]));
}

} // namespace saga
