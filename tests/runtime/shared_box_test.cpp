/* The shared box: the counted heap copy every copy of a value shares. */

#include "runtime_test_types.h"
#include <gtest/gtest.h>

namespace {

saga_runtime_shared *header(void *value) {
  return reinterpret_cast<saga_runtime_shared *>(static_cast<char *>(value) -
                                                 sizeof(saga_runtime_shared));
}

int retained = 0;
int released = 0;

void count_retain(void *) { ++retained; }
void count_release(void *) { ++released; }
void no_retain(void *) {}

const saga_runtime_elem_ops counting_ops = {no_retain, count_release};
const saga_runtime_elem_ops retaining_ops = {count_retain, count_release};

/* A box whose value is the next box in a chain, as a list's node holds the
   rest of the list. */
void release_next(void *value) { saga_shared_release(*(void **)value); }

const saga_runtime_elem_ops chain_ops = {no_retain, release_next};

} // namespace

TEST(SharedBoxTest, NewBoxIsZeroedWithOneReference) {
  auto *value = static_cast<int64_t *>(saga_shared_new(16, nullptr));
  EXPECT_EQ(header(value)->refcount, 1);
  EXPECT_EQ(value[0], 0);
  EXPECT_EQ(value[1], 0);
  saga_shared_release(value);
}

TEST(SharedBoxTest, LastReleaseReleasesWhatTheBoxHolds) {
  released = 0;
  void *value = saga_shared_new(8, &counting_ops);
  saga_shared_retain(value);
  saga_shared_release(value);
  EXPECT_EQ(released, 0);
  saga_shared_release(value);
  EXPECT_EQ(released, 1);
}

TEST(SharedBoxTest, ConstantIsNeverFreed) {
  released = 0;
  struct {
    saga_runtime_shared header;
    int64_t value;
  } constant = {{-1, &counting_ops}, 7};
  saga_shared_retain(&constant.value);
  saga_shared_release(&constant.value);
  EXPECT_EQ(constant.header.refcount, -1);
  EXPECT_EQ(released, 0);
}

TEST(SharedBoxTest, NullIsANoOp) {
  saga_shared_retain(nullptr);
  saga_shared_release(nullptr);
}

TEST(SharedBoxTest, MakeUniqueKeepsABoxNoOneElseHolds) {
  void *value = saga_shared_new(8, &retaining_ops);
  EXPECT_EQ(saga_shared_make_unique(value, 8), value);
  EXPECT_EQ(header(value)->refcount, 2);
  saga_shared_release(value);
  saga_shared_release(value);
}

TEST(SharedBoxTest, MakeUniqueCopiesASharedBox) {
  retained = 0;
  released = 0;
  auto *value = static_cast<int64_t *>(saga_shared_new(8, &retaining_ops));
  *value = 42;
  saga_shared_retain(value);

  auto *copy = static_cast<int64_t *>(saga_shared_make_unique(value, 8));
  ASSERT_NE(copy, value);
  EXPECT_EQ(*copy, 42);
  EXPECT_EQ(header(copy)->refcount, 1);
  EXPECT_EQ(header(value)->refcount, 2);
  EXPECT_EQ(retained, 1);

  saga_shared_release(copy);
  saga_shared_release(value);
  saga_shared_release(value);
  EXPECT_EQ(released, 2);
}

TEST(SharedBoxTest, MakeUniqueCopiesAConstant) {
  struct {
    saga_runtime_shared header;
    int64_t value;
  } constant = {{-1, nullptr}, 7};
  auto *copy =
      static_cast<int64_t *>(saga_shared_make_unique(&constant.value, 8));
  ASSERT_NE(copy, &constant.value);
  EXPECT_EQ(*copy, 7);
  EXPECT_EQ(constant.header.refcount, -1);
  saga_shared_release(copy);
}

TEST(SharedBoxTest, MakeUniqueOfNullIsNull) {
  EXPECT_EQ(saga_shared_make_unique(nullptr, 8), nullptr);
}

/* Deep enough to overflow the stack if each box freed the next from inside
   its own release. */
TEST(SharedBoxTest, LongChainIsFreedWithoutRecursing) {
  void *head = nullptr;
  for (int i = 0; i < 2000000; ++i) {
    void *node = saga_shared_new(sizeof(void *), &chain_ops);
    *(void **)node = head;
    head = node;
  }
  saga_shared_release(head);
}
