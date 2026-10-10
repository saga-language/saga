/* The shared box: a counted heap copy of a value nothing writes through. */

#include "runtime_test_types.h"
#include <gtest/gtest.h>

namespace {

saga_runtime_shared *header(void *value) {
  return reinterpret_cast<saga_runtime_shared *>(static_cast<char *>(value) -
                                                 sizeof(saga_runtime_shared));
}

int released = 0;

void count_release(void *) { ++released; }
void no_retain(void *) {}

const saga_runtime_elem_ops counting_ops = {no_retain, count_release};

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
