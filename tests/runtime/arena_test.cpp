/* Arena allocator tests for the Saga runtime. */

#include "runtime_test_types.h"
#include <gtest/gtest.h>
#include <cstring>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────
// Basic lifecycle
// ─────────────────────────────────────────────────────────────────────────

TEST(ArenaTest, NewAndDestroy) {
  saga_runtime_arena *a = saga_runtime_arena_new(0);
  ASSERT_NE(a, nullptr);
  EXPECT_NE(a->base, nullptr);
  EXPECT_EQ(a->offset, 0);
  EXPECT_GT(a->committed, 0);
  EXPECT_GT(a->reserved, 0);
  EXPECT_GT(a->max_limit, 0);
  saga_runtime_arena_destroy(a);
}

TEST(ArenaTest, NewWithCustomLimit) {
  int64_t limit = 1024 * 1024; // 1 MB
  saga_runtime_arena *a = saga_runtime_arena_new(limit);
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->max_limit, limit);
  saga_runtime_arena_destroy(a);
}

// ─────────────────────────────────────────────────────────────────────────
// Basic allocation
// ─────────────────────────────────────────────────────────────────────────

TEST(ArenaTest, AllocReturnsNonNull) {
  saga_runtime_arena *a = saga_runtime_arena_new(0);
  void *p = saga_runtime_arena_alloc(a, 64);
  ASSERT_NE(p, nullptr);
  saga_runtime_arena_destroy(a);
}

TEST(ArenaTest, AllocAdvancesOffset) {
  saga_runtime_arena *a = saga_runtime_arena_new(0);
  int64_t before = a->offset;
  saga_runtime_arena_alloc(a, 64);
  EXPECT_GT(a->offset, before);
  saga_runtime_arena_destroy(a);
}

TEST(ArenaTest, AllocIsAligned) {
  saga_runtime_arena *a = saga_runtime_arena_new(0);
  // Allocate various sizes and check 16-byte alignment.
  for (int size = 1; size <= 129; size++) {
    void *p = saga_runtime_arena_alloc(a, size);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(reinterpret_cast<uintptr_t>(p) % 16, 0u)
        << "Allocation of size " << size << " not 16-byte aligned";
  }
  saga_runtime_arena_destroy(a);
}

TEST(ArenaTest, AllocationsDoNotOverlap) {
  saga_runtime_arena *a = saga_runtime_arena_new(0);
  void *p1 = saga_runtime_arena_alloc(a, 100);
  void *p2 = saga_runtime_arena_alloc(a, 200);
  ASSERT_NE(p1, nullptr);
  ASSERT_NE(p2, nullptr);

  auto a1 = reinterpret_cast<uintptr_t>(p1);
  auto a2 = reinterpret_cast<uintptr_t>(p2);
  // p2 must start at or after the aligned end of p1
  EXPECT_GE(a2, a1 + 100);
  saga_runtime_arena_destroy(a);
}

TEST(ArenaTest, WrittenDataSurvives) {
  saga_runtime_arena *a = saga_runtime_arena_new(0);
  char *p1 = (char *)saga_runtime_arena_alloc(a, 16);
  char *p2 = (char *)saga_runtime_arena_alloc(a, 16);
  std::memcpy(p1, "hello, arena!!\0", 15);
  std::memcpy(p2, "second block!!\0", 15);

  // Both allocations should retain their data.
  EXPECT_STREQ(p1, "hello, arena!!");
  EXPECT_STREQ(p2, "second block!!");
  saga_runtime_arena_destroy(a);
}

// ─────────────────────────────────────────────────────────────────────────
// Pointer stability (the core invariant of the mmap design)
// ─────────────────────────────────────────────────────────────────────────

TEST(ArenaTest, PointersStableAcrossGrowth) {
  // Use a small limit so that the arena must commit new pages.
  int64_t limit = 4 * 1024 * 1024; // 4 MB
  saga_runtime_arena *a = saga_runtime_arena_new(limit);

  std::vector<void *> ptrs;
  // Allocate in 4 KB chunks until we exceed the initial commit (64 KB).
  for (int i = 0; i < 256; i++) { // 256 × 4 KB = 1 MB
    void *p = saga_runtime_arena_alloc(a, 4096);
    ASSERT_NE(p, nullptr) << "Allocation failed at iteration " << i;
    ptrs.push_back(p);
  }

  // Verify all pointers still fall within the arena's base region.
  for (size_t i = 0; i < ptrs.size(); i++) {
    auto addr = reinterpret_cast<uintptr_t>(ptrs[i]);
    auto base = reinterpret_cast<uintptr_t>(a->base);
    EXPECT_GE(addr, base);
    EXPECT_LT(addr, base + (uintptr_t)a->reserved)
        << "Pointer " << i << " escaped arena bounds";
  }
  saga_runtime_arena_destroy(a);
}

// ─────────────────────────────────────────────────────────────────────────
// Commit-on-demand
// ─────────────────────────────────────────────────────────────────────────

TEST(ArenaTest, CommittedGrowsOnDemand) {
  saga_runtime_arena *a = saga_runtime_arena_new(0);
  int64_t initial = a->committed;

  // Allocate more than the initial commit.
  saga_runtime_arena_alloc(a, initial + 1);
  EXPECT_GT(a->committed, initial);
  saga_runtime_arena_destroy(a);
}

// ─────────────────────────────────────────────────────────────────────────
// Quota enforcement
// ─────────────────────────────────────────────────────────────────────────

TEST(ArenaTest, ExceedingMaxLimitReturnsNull) {
  int64_t limit = 64 * 1024; // 64 KB
  saga_runtime_arena *a = saga_runtime_arena_new(limit);

  // Fill up to the limit.
  void *p = saga_runtime_arena_alloc(a, limit - 16); // leave room for alignment
  ASSERT_NE(p, nullptr);

  // Next allocation should fail.
  void *over = saga_runtime_arena_alloc(a, 1024);
  EXPECT_EQ(over, nullptr);
  saga_runtime_arena_destroy(a);
}

TEST(ArenaTest, ZeroSizeAllocReturnsNull) {
  saga_runtime_arena *a = saga_runtime_arena_new(0);
  EXPECT_EQ(saga_runtime_arena_alloc(a, 0), nullptr);
  EXPECT_EQ(saga_runtime_arena_alloc(a, -1), nullptr);
  saga_runtime_arena_destroy(a);
}

TEST(ArenaTest, NullArenaAllocReturnsNull) {
  EXPECT_EQ(saga_runtime_arena_alloc(nullptr, 64), nullptr);
}

// ─────────────────────────────────────────────────────────────────────────
// Arena-aware string helper
// ─────────────────────────────────────────────────────────────────────────

TEST(ArenaTest, AllocString) {
  saga_runtime_arena *a = saga_runtime_arena_new(0);
  const char *msg = "hello arena";
  saga_runtime_string *s = saga_runtime_arena_alloc_string(a, msg, 11);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->len, 11);
  EXPECT_EQ(s->refcount, -1); // arena-owned
  EXPECT_EQ(std::memcmp(s->data, msg, 11), 0);
  saga_runtime_arena_destroy(a);
}

TEST(ArenaTest, AllocStringEmpty) {
  saga_runtime_arena *a = saga_runtime_arena_new(0);
  saga_runtime_string *s = saga_runtime_arena_alloc_string(a, nullptr, 0);
  ASSERT_NE(s, nullptr);
  EXPECT_EQ(s->len, 0);
  EXPECT_EQ(s->refcount, -1);
  saga_runtime_arena_destroy(a);
}

// ─────────────────────────────────────────────────────────────────────────
// Arena-aware array helper
// ─────────────────────────────────────────────────────────────────────────

TEST(ArenaTest, AllocArray) {
  saga_runtime_arena *a = saga_runtime_arena_new(0);
  saga_runtime_array *arr = saga_runtime_arena_alloc_array(a, sizeof(int64_t), 16);
  ASSERT_NE(arr, nullptr);
  EXPECT_EQ(arr->len, 0);
  EXPECT_EQ(arr->cap, 16);
  EXPECT_EQ(arr->elem_size, (int64_t)sizeof(int64_t));
  EXPECT_EQ(arr->refcount, -1);
  EXPECT_NE(arr->data, nullptr);
  saga_runtime_arena_destroy(a);
}

TEST(ArenaTest, AllocArrayMinCap) {
  saga_runtime_arena *a = saga_runtime_arena_new(0);
  saga_runtime_array *arr = saga_runtime_arena_alloc_array(a, sizeof(int64_t), 1);
  ASSERT_NE(arr, nullptr);
  EXPECT_GE(arr->cap, 4); // minimum cap enforced
  saga_runtime_arena_destroy(a);
}

// ─────────────────────────────────────────────────────────────────────────
// Destroy is safe to call on NULL
// ─────────────────────────────────────────────────────────────────────────

TEST(ArenaTest, DestroyNullIsSafe) {
  saga_runtime_arena_destroy(nullptr); // must not crash
}
