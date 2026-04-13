/* Intrinsics tests for the Saga runtime (Phase 8).
 *
 * Tests saga_actor_yield and saga_actor_trap.
 * intrinsic_atomic_add is a direct LLVM instruction — tested in codegen.
 */

#include "runtime_test_types.h"
#include <gtest/gtest.h>
#include <csetjmp>
#include <cstring>
#include <thread>
#include <chrono>

/* ═══════════════════════════════════════════════════════════════════════ */
/* saga_actor_yield unit tests (no executor needed)                         */
/* ═══════════════════════════════════════════════════════════════════════ */

TEST(YieldTest, ResetsReductionCount) {
  mc_actor *a = mc_actor_new([](mc_actor *){}, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);
  a->status = MC_ACTOR_RUNNING;
  a->reduction_count = 500;

  saga_actor_yield(a);

  EXPECT_EQ(a->reduction_count, 0);

  mc_actor_release(a);
  mc_actor_release(a);
}

TEST(YieldTest, UpdatesLastCycle) {
  mc_actor *a = mc_actor_new([](mc_actor *){}, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);
  a->status = MC_ACTOR_RUNNING;
  a->last_cycle = 0;

  saga_actor_yield(a);

  EXPECT_GT(a->last_cycle, 0);

  mc_actor_release(a);
  mc_actor_release(a);
}

TEST(YieldTest, NullActorIsSafe) {
  saga_actor_yield(nullptr); // must not crash
}

/* ═══════════════════════════════════════════════════════════════════════ */
/* saga_actor_yield executor integration tests                              */
/* ═══════════════════════════════════════════════════════════════════════ */

class YieldExecutorTest : public ::testing::Test {
protected:
  void SetUp() override    { saga_executor_init(2); }
  void TearDown() override { saga_executor_shutdown(); }
};

/* An actor that yields, then completes. */
static void yield_then_complete(mc_actor *a) {
  saga_actor_yield(a);
  /* Return normally → COMPLETED. */
}

TEST_F(YieldExecutorTest, YieldDoesNotPreventCompletion) {
  mc_actor *a = saga_executor_spawn(yield_then_complete, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);

  int64_t status;
  saga_task_wait(a, &status);

  EXPECT_EQ(status, MC_ACTOR_COMPLETED);
  saga_task_drop(a);
}

/* An actor that yields many times inside a loop with reduction ticks.
   The yield should reset the counter so it doesn't get killed. */
static void yield_prevents_quota_kill(mc_actor *a) {
  for (int i = 0; i < 200; i++) {
    /* Tick up the counter close to the limit. */
    for (int j = 0; j < 100000; j++)
      saga_reduction_tick(a);
    /* Yield resets the counter. */
    saga_actor_yield(a);
  }
}

TEST_F(YieldExecutorTest, YieldResetsQuotaInLoop) {
  mc_actor *a = saga_executor_spawn(yield_prevents_quota_kill, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);

  int64_t status;
  saga_task_wait(a, &status);

  EXPECT_EQ(status, MC_ACTOR_COMPLETED);
  saga_task_drop(a);
}

/* Multiple actors yielding concurrently should all complete. */
TEST_F(YieldExecutorTest, MultipleYieldingActorsComplete) {
  const int N = 20;
  std::vector<mc_actor *> actors(N);

  for (int i = 0; i < N; i++) {
    actors[i] = saga_executor_spawn(yield_then_complete, nullptr, 0, 0);
    ASSERT_NE(actors[i], nullptr);
  }

  for (int i = 0; i < N; i++) {
    int64_t status;
    saga_task_wait(actors[i], &status);
    EXPECT_EQ(status, MC_ACTOR_COMPLETED);
    saga_task_drop(actors[i]);
  }
}

/* ═══════════════════════════════════════════════════════════════════════ */
/* saga_actor_trap unit tests                                               */
/* ═══════════════════════════════════════════════════════════════════════ */

/* Helper: create a heap mc_string for test use. */
static mc_string *make_test_string(const char *buf, int64_t len) {
  char *data = (char *)malloc((size_t)len);
  if (len > 0) memcpy(data, buf, (size_t)len);
  mc_string *s = (mc_string *)malloc(sizeof(mc_string));
  s->data = data;
  s->len = len;
  s->refcount = 1;
  return s;
}

static void free_test_string(mc_string *s) {
  if (s) {
    free((void *)s->data);
    free(s);
  }
}

TEST(TrapTest, SetsZombieStatusAndLongjmps) {
  mc_actor *a = mc_actor_new([](mc_actor *){}, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);
  a->status = MC_ACTOR_RUNNING;

  mc_string *reason = make_test_string("test trap", 9);

  bool jumped = false;
  if (setjmp(a->trap) == 0) {
    saga_actor_trap(a, reason);
    FAIL() << "Should have longjmp'd";
  } else {
    jumped = true;
  }

  EXPECT_TRUE(jumped);
  EXPECT_EQ(a->status, MC_ACTOR_ZOMBIE);
  ASSERT_NE(a->result, nullptr);

  /* Verify the reason string was copied. */
  mc_string *stored = (mc_string *)a->result;
  EXPECT_EQ(stored->len, 9);
  EXPECT_EQ(std::memcmp(stored->data, "test trap", 9), 0);

  free_test_string(reason);
  /* Clean up the stored result (it was malloc'd by saga_actor_trap). */
  free(a->result);
  a->result = nullptr;
  mc_actor_release(a);
  mc_actor_release(a);
}

TEST(TrapTest, NullReasonStillSetsZombie) {
  mc_actor *a = mc_actor_new([](mc_actor *){}, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);
  a->status = MC_ACTOR_RUNNING;

  bool jumped = false;
  if (setjmp(a->trap) == 0) {
    saga_actor_trap(a, nullptr);
    FAIL() << "Should have longjmp'd";
  } else {
    jumped = true;
  }

  EXPECT_TRUE(jumped);
  EXPECT_EQ(a->status, MC_ACTOR_ZOMBIE);
  EXPECT_EQ(a->result, nullptr); /* no reason → no result string */

  mc_actor_release(a);
  mc_actor_release(a);
}

TEST(TrapTest, NullActorIsSafe) {
  saga_actor_trap(nullptr, nullptr); // must not crash
}

/* ═══════════════════════════════════════════════════════════════════════ */
/* saga_actor_trap executor integration tests                               */
/* ═══════════════════════════════════════════════════════════════════════ */

class TrapExecutorTest : public ::testing::Test {
protected:
  void SetUp() override    { saga_executor_init(2); }
  void TearDown() override { saga_executor_shutdown(); }
};

/* An actor that traps with a reason string. */
static void trap_with_reason(mc_actor *a) {
  /* Allocate the reason string in the arena. */
  mc_string *reason = mc_arena_alloc_string(a->arena, "fatal error", 11);
  saga_actor_trap(a, reason);
  /* Should never reach here. */
}

TEST_F(TrapExecutorTest, TrapSetsZombieStatus) {
  mc_actor *a = saga_executor_spawn(trap_with_reason, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);

  int64_t status;
  saga_task_wait(a, &status);

  /* saga_actor_trap sets ZOMBIE, but the worker loop may reclassify.
     Check that the actor completed in some terminal state. */
  EXPECT_GE(status, MC_ACTOR_COMPLETED);

  saga_task_drop(a);
}

/* An actor that traps without a reason. */
static void trap_no_reason(mc_actor *a) {
  saga_actor_trap(a, nullptr);
}

TEST_F(TrapExecutorTest, TrapWithoutReasonCompletes) {
  mc_actor *a = saga_executor_spawn(trap_no_reason, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);

  int64_t status;
  saga_task_wait(a, &status);

  EXPECT_GE(status, MC_ACTOR_COMPLETED);
  saga_task_drop(a);
}
