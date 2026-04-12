/* Copy-on-Write helper tests for the Saga runtime. */

#include "runtime_test_types.h"
#include <gtest/gtest.h>
#include <cstring>

/* Helper: allocate a heap mc_string (mimics mc_alloc_string). */
static mc_string *heap_string(const char *buf, int64_t len) {
  char *data = (char *)malloc((size_t)len);
  if (len > 0) memcpy(data, buf, (size_t)len);
  mc_string *s = (mc_string *)malloc(sizeof(mc_string));
  s->data = data;
  s->len = len;
  s->refcount = 1;
  return s;
}

/* Helper: allocate a heap mc_array. */
static mc_array *heap_array(int64_t elem_size, int64_t cap) {
  mc_array *arr = (mc_array *)malloc(sizeof(mc_array));
  arr->data = malloc((size_t)(elem_size * cap));
  arr->len = 0;
  arr->cap = cap;
  arr->elem_size = elem_size;
  arr->refcount = 1;
  return arr;
}

// ─────────────────────────────────────────────────────────────────────────
// COW String
// ─────────────────────────────────────────────────────────────────────────

TEST(CowTest, StringCopyIntoArena) {
  mc_arena *a = mc_arena_new(0);
  mc_string *src = heap_string("hello", 5);
  saga_retain_string(src); // refcount = 2 (shared)

  mc_string *copy = mc_cow_copy_string(a, src);
  ASSERT_NE(copy, nullptr);
  EXPECT_NE(copy, src);  // different object
  EXPECT_EQ(copy->len, 5);
  EXPECT_EQ(std::memcmp(copy->data, "hello", 5), 0);
  EXPECT_EQ(copy->refcount, -1); // arena-owned

  // src should have had its refcount decremented (from 2 → 1)
  EXPECT_EQ(src->refcount, 1);

  saga_release_string(src); // free the original
  mc_arena_destroy(a);
}

TEST(CowTest, StringCopyNullArena) {
  mc_string *src = heap_string("test", 4);
  mc_string *result = mc_cow_copy_string(nullptr, src);
  EXPECT_EQ(result, src); // returns original on null arena
  saga_release_string(src);
}

TEST(CowTest, StringCopyNullSrc) {
  mc_arena *a = mc_arena_new(0);
  mc_string *result = mc_cow_copy_string(a, nullptr);
  EXPECT_EQ(result, nullptr);
  mc_arena_destroy(a);
}

TEST(CowTest, StringCopyEmpty) {
  mc_arena *a = mc_arena_new(0);
  mc_string *src = heap_string("", 0);
  saga_retain_string(src);

  mc_string *copy = mc_cow_copy_string(a, src);
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(copy->len, 0);
  EXPECT_EQ(copy->refcount, -1);

  saga_release_string(src);
  mc_arena_destroy(a);
}

// ─────────────────────────────────────────────────────────────────────────
// COW Array
// ─────────────────────────────────────────────────────────────────────────

TEST(CowTest, ArrayCopyIntoArena) {
  mc_arena *a = mc_arena_new(0);
  mc_array *src = heap_array(sizeof(int64_t), 8);

  // Push some data.
  for (int64_t i = 0; i < 5; i++) {
    memcpy((char *)src->data + i * sizeof(int64_t), &i, sizeof(int64_t));
    src->len++;
  }

  saga_retain_array(src); // refcount = 2

  mc_array *copy = mc_cow_copy_array(a, src);
  ASSERT_NE(copy, nullptr);
  EXPECT_NE(copy, src);
  EXPECT_EQ(copy->len, 5);
  EXPECT_EQ(copy->elem_size, (int64_t)sizeof(int64_t));
  EXPECT_EQ(copy->refcount, -1);

  // Verify data was copied.
  for (int64_t i = 0; i < 5; i++) {
    int64_t val;
    memcpy(&val, (char *)copy->data + i * sizeof(int64_t), sizeof(int64_t));
    EXPECT_EQ(val, i);
  }

  // src refcount decremented (2 → 1)
  EXPECT_EQ(src->refcount, 1);

  saga_release_array(src);
  mc_arena_destroy(a);
}

TEST(CowTest, ArrayCopyNullArena) {
  mc_array *src = heap_array(sizeof(int64_t), 4);
  mc_array *result = mc_cow_copy_array(nullptr, src);
  EXPECT_EQ(result, src);
  saga_release_array(src);
}

TEST(CowTest, ArrayCopyNullSrc) {
  mc_arena *a = mc_arena_new(0);
  mc_array *result = mc_cow_copy_array(a, nullptr);
  EXPECT_EQ(result, nullptr);
  mc_arena_destroy(a);
}

TEST(CowTest, ArrayCopyEmpty) {
  mc_arena *a = mc_arena_new(0);
  mc_array *src = heap_array(sizeof(int64_t), 4);
  saga_retain_array(src);

  mc_array *copy = mc_cow_copy_array(a, src);
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(copy->len, 0);
  EXPECT_EQ(copy->refcount, -1);

  saga_release_array(src);
  mc_arena_destroy(a);
}

// ─────────────────────────────────────────────────────────────────────────
// COW barrier pattern: only copy when refcount > 1
// ─────────────────────────────────────────────────────────────────────────

TEST(CowTest, BarrierSkipsWhenUnique) {
  mc_arena *a = mc_arena_new(0);
  mc_string *src = heap_string("unique", 6);
  // refcount == 1 (only one owner) — no COW needed.
  // The codegen barrier is: if (refcount > 1) cow_copy else use directly.
  EXPECT_EQ(src->refcount, 1);
  // Simulate: no copy needed, just use src directly.
  EXPECT_EQ(src->len, 6);
  saga_release_string(src);
  mc_arena_destroy(a);
}

TEST(CowTest, BarrierCopiesWhenShared) {
  mc_arena *a = mc_arena_new(0);
  mc_string *src = heap_string("shared", 6);
  saga_retain_string(src); // refcount = 2

  // Simulate the barrier: refcount > 1 → copy.
  EXPECT_GT(src->refcount, 1);
  mc_string *copy = mc_cow_copy_string(a, src);
  EXPECT_NE(copy, src);
  EXPECT_EQ(copy->refcount, -1);
  EXPECT_EQ(src->refcount, 1); // original now unique

  saga_release_string(src);
  mc_arena_destroy(a);
}
