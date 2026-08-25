/* Copy-on-Write helper tests for the Saga runtime. */

#include "runtime_test_types.h"
#include <gtest/gtest.h>
#include <cstring>

/* Helper: allocate a heap saga_runtime_string (mimics saga_runtime_alloc_string). */
static saga_runtime_string *heap_string(const char *buf, int64_t len) {
  char *data = (char *)malloc((size_t)len);
  if (len > 0) memcpy(data, buf, (size_t)len);
  saga_runtime_string *s = (saga_runtime_string *)malloc(sizeof(saga_runtime_string));
  s->data = data;
  s->len = len;
  s->refcount = 1;
  return s;
}

/* Helper: allocate a heap saga_runtime_array. */
static saga_runtime_array *heap_array(int64_t elem_size, int64_t cap) {
  saga_runtime_array *arr = (saga_runtime_array *)malloc(sizeof(saga_runtime_array));
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
  saga_runtime_arena *a = saga_runtime_arena_new(0);
  saga_runtime_string *src = heap_string("hello", 5);
  saga_retain_string(src); // refcount = 2 (shared)

  saga_runtime_string *copy = saga_runtime_cow_copy_string(a, src);
  ASSERT_NE(copy, nullptr);
  EXPECT_NE(copy, src);  // different object
  EXPECT_EQ(copy->len, 5);
  EXPECT_EQ(std::memcmp(copy->data, "hello", 5), 0);
  EXPECT_EQ(copy->refcount, -1); // arena-owned

  // src should have had its refcount decremented (from 2 → 1)
  EXPECT_EQ(src->refcount, 1);

  saga_release_string(src); // free the original
  saga_runtime_arena_destroy(a);
}

TEST(CowTest, StringCopyNullArena) {
  saga_runtime_string *src = heap_string("test", 4);
  saga_runtime_string *result = saga_runtime_cow_copy_string(nullptr, src);
  EXPECT_EQ(result, src); // returns original on null arena
  saga_release_string(src);
}

TEST(CowTest, StringCopyNullSrc) {
  saga_runtime_arena *a = saga_runtime_arena_new(0);
  saga_runtime_string *result = saga_runtime_cow_copy_string(a, nullptr);
  EXPECT_EQ(result, nullptr);
  saga_runtime_arena_destroy(a);
}

TEST(CowTest, StringCopyEmpty) {
  saga_runtime_arena *a = saga_runtime_arena_new(0);
  saga_runtime_string *src = heap_string("", 0);
  saga_retain_string(src);

  saga_runtime_string *copy = saga_runtime_cow_copy_string(a, src);
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(copy->len, 0);
  EXPECT_EQ(copy->refcount, -1);

  saga_release_string(src);
  saga_runtime_arena_destroy(a);
}

// ─────────────────────────────────────────────────────────────────────────
// COW Array
// ─────────────────────────────────────────────────────────────────────────

TEST(CowTest, ArrayCopyIntoArena) {
  saga_runtime_arena *a = saga_runtime_arena_new(0);
  saga_runtime_array *src = heap_array(sizeof(int64_t), 8);

  // Push some data.
  for (int64_t i = 0; i < 5; i++) {
    memcpy((char *)src->data + i * sizeof(int64_t), &i, sizeof(int64_t));
    src->len++;
  }

  saga_retain_array(src); // refcount = 2

  saga_runtime_array *copy = saga_runtime_cow_copy_array(a, src);
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
  saga_runtime_arena_destroy(a);
}

TEST(CowTest, ArrayCopyNullArena) {
  saga_runtime_array *src = heap_array(sizeof(int64_t), 4);
  saga_runtime_array *result = saga_runtime_cow_copy_array(nullptr, src);
  EXPECT_EQ(result, src);
  saga_release_array(src);
}

TEST(CowTest, ArrayCopyNullSrc) {
  saga_runtime_arena *a = saga_runtime_arena_new(0);
  saga_runtime_array *result = saga_runtime_cow_copy_array(a, nullptr);
  EXPECT_EQ(result, nullptr);
  saga_runtime_arena_destroy(a);
}

TEST(CowTest, ArrayCopyEmpty) {
  saga_runtime_arena *a = saga_runtime_arena_new(0);
  saga_runtime_array *src = heap_array(sizeof(int64_t), 4);
  saga_retain_array(src);

  saga_runtime_array *copy = saga_runtime_cow_copy_array(a, src);
  ASSERT_NE(copy, nullptr);
  EXPECT_EQ(copy->len, 0);
  EXPECT_EQ(copy->refcount, -1);

  saga_release_array(src);
  saga_runtime_arena_destroy(a);
}

// ─────────────────────────────────────────────────────────────────────────
// COW barrier pattern: only copy when refcount > 1
// ─────────────────────────────────────────────────────────────────────────

TEST(CowTest, BarrierSkipsWhenUnique) {
  saga_runtime_arena *a = saga_runtime_arena_new(0);
  saga_runtime_string *src = heap_string("unique", 6);
  // refcount == 1 (only one owner) — no COW needed.
  // The codegen barrier is: if (refcount > 1) cow_copy else use directly.
  EXPECT_EQ(src->refcount, 1);
  // Simulate: no copy needed, just use src directly.
  EXPECT_EQ(src->len, 6);
  saga_release_string(src);
  saga_runtime_arena_destroy(a);
}

TEST(CowTest, BarrierCopiesWhenShared) {
  saga_runtime_arena *a = saga_runtime_arena_new(0);
  saga_runtime_string *src = heap_string("shared", 6);
  saga_retain_string(src); // refcount = 2

  // Simulate the barrier: refcount > 1 → copy.
  EXPECT_GT(src->refcount, 1);
  saga_runtime_string *copy = saga_runtime_cow_copy_string(a, src);
  EXPECT_NE(copy, src);
  EXPECT_EQ(copy->refcount, -1);
  EXPECT_EQ(src->refcount, 1); // original now unique

  saga_release_string(src);
  saga_runtime_arena_destroy(a);
}

// ─────────────────────────────────────────────────────────────────────────
// make_unique: the barrier an in-place write applies before it writes
//
// It hands back a +1 reference and leaves the source's count alone — the
// caller drops the reference the slot held, so the slot owns exactly one
// either way.
// ─────────────────────────────────────────────────────────────────────────

static saga_runtime_map *int_map() {
  return saga_map_new(sizeof(int64_t), sizeof(int64_t), 1 /* INT64 */,
                      nullptr);
}

static void map_put(saga_runtime_map *m, int64_t k, int64_t v) {
  saga_map_set(m, &k, &v);
}

static int64_t map_at(saga_runtime_map *m, int64_t k) {
  void *v = saga_map_get(m, &k);
  return v ? *(int64_t *)v : -1;
}

static saga_runtime_array *int_array(std::initializer_list<int64_t> vals) {
  saga_runtime_array *arr = heap_array(sizeof(int64_t), 8);
  for (int64_t v : vals) {
    memcpy((char *)arr->data + arr->len * sizeof(int64_t), &v, sizeof(int64_t));
    arr->len++;
  }
  return arr;
}

TEST(MakeUniqueTest, MapSharedIsCloned) {
  saga_runtime_map *m = int_map();
  map_put(m, 1, 10);
  map_put(m, 2, 20);
  saga_retain_map(m); // a second name

  saga_runtime_map *u = saga_map_make_unique(m);
  ASSERT_NE(u, m);
  EXPECT_EQ(u->refcount, 1);
  EXPECT_EQ(m->refcount, 2);

  map_put(u, 1, 99);
  EXPECT_EQ(map_at(u, 1), 99);
  EXPECT_EQ(map_at(m, 1), 10);
  EXPECT_EQ(saga_map_size(u), 2);

  saga_release_map(u);
  saga_release_map(m);
  saga_release_map(m);
}

TEST(MakeUniqueTest, MapUniqueIsRetainedInPlace) {
  saga_runtime_map *m = int_map();
  map_put(m, 1, 10);

  saga_runtime_map *u = saga_map_make_unique(m);
  EXPECT_EQ(u, m);
  EXPECT_EQ(m->refcount, 2);

  saga_release_map(m);
  EXPECT_EQ(m->refcount, 1);
  saga_release_map(m);
}

TEST(MakeUniqueTest, MapCloneOwnsItsEntryBlocks) {
  // A struct-shallow copy would share the key/value allocations and free
  // each of them twice.
  saga_runtime_map *m = int_map();
  map_put(m, 1, 10);

  saga_runtime_map *c = saga_map_clone(m);
  ASSERT_EQ(c->len, 1);
  EXPECT_NE(c->entries[0].key, m->entries[0].key);
  EXPECT_NE(c->entries[0].value, m->entries[0].value);
  EXPECT_EQ(map_at(c, 1), 10);

  saga_release_map(c);
  saga_release_map(m);
}

TEST(MakeUniqueTest, MapStaticIsCloned) {
  // refcount -1 marks storage the runtime must neither write nor free.
  saga_runtime_map *m = int_map();
  map_put(m, 1, 10);
  m->refcount = -1;

  saga_runtime_map *u = saga_map_make_unique(m);
  ASSERT_NE(u, m);
  EXPECT_EQ(u->refcount, 1);
  map_put(u, 1, 99);
  EXPECT_EQ(map_at(m, 1), 10);

  saga_release_map(u);
  m->refcount = 1;
  saga_release_map(m);
}

TEST(MakeUniqueTest, ArraySharedIsCloned) {
  saga_runtime_array *a = int_array({1, 2, 3});
  saga_retain_array(a);

  saga_runtime_array *u = saga_array_make_unique(a);
  ASSERT_NE(u, a);
  EXPECT_EQ(u->refcount, 1);
  EXPECT_EQ(a->refcount, 2);

  int64_t v = 99;
  memcpy(u->data, &v, sizeof(int64_t));
  EXPECT_EQ(*(int64_t *)u->data, 99);
  EXPECT_EQ(*(int64_t *)a->data, 1);

  saga_release_array(u);
  saga_release_array(a);
  saga_release_array(a);
}

TEST(MakeUniqueTest, ArrayUniqueIsRetainedInPlace) {
  saga_runtime_array *a = int_array({1, 2, 3});

  saga_runtime_array *u = saga_array_make_unique(a);
  EXPECT_EQ(u, a);
  EXPECT_EQ(a->refcount, 2);

  saga_release_array(a);
  EXPECT_EQ(a->refcount, 1);
  saga_release_array(a);
}

TEST(MakeUniqueTest, NullIsPassedThrough) {
  EXPECT_EQ(saga_map_make_unique(nullptr), nullptr);
  EXPECT_EQ(saga_array_make_unique(nullptr), nullptr);
  EXPECT_EQ(saga_map_clone(nullptr), nullptr);
}
