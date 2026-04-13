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

  mc_set_current_actor_for_test(a);
  saga_actor_yield();
  mc_set_current_actor_for_test(nullptr);

  EXPECT_EQ(a->reduction_count, 0);

  mc_actor_release(a);
  mc_actor_release(a);
}

TEST(YieldTest, UpdatesLastCycle) {
  mc_actor *a = mc_actor_new([](mc_actor *){}, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);
  a->status = MC_ACTOR_RUNNING;
  a->last_cycle = 0;

  mc_set_current_actor_for_test(a);
  saga_actor_yield();
  mc_set_current_actor_for_test(nullptr);

  EXPECT_GT(a->last_cycle, 0);

  mc_actor_release(a);
  mc_actor_release(a);
}

TEST(YieldTest, NullActorIsSafe) {
  /* No current actor published → yield is a safe no-op. */
  mc_set_current_actor_for_test(nullptr);
  saga_actor_yield();
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
  (void)a;
  saga_actor_yield();
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
    saga_actor_yield();
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
  mc_set_current_actor_for_test(a);
  if (setjmp(a->trap) == 0) {
    saga_actor_trap(reason);
    FAIL() << "Should have longjmp'd";
  } else {
    jumped = true;
  }
  mc_set_current_actor_for_test(nullptr);

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
  mc_set_current_actor_for_test(a);
  if (setjmp(a->trap) == 0) {
    saga_actor_trap(nullptr);
    FAIL() << "Should have longjmp'd";
  } else {
    jumped = true;
  }
  mc_set_current_actor_for_test(nullptr);

  EXPECT_TRUE(jumped);
  EXPECT_EQ(a->status, MC_ACTOR_ZOMBIE);
  EXPECT_EQ(a->result, nullptr); /* no reason → no result string */

  mc_actor_release(a);
  mc_actor_release(a);
}

TEST(TrapTest, NullActorIsSafe) {
  /* No current actor published → trap is a safe no-op. */
  mc_set_current_actor_for_test(nullptr);
  saga_actor_trap(nullptr);
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
  saga_actor_trap(reason);
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
  (void)a;
  saga_actor_trap(nullptr);
}

TEST_F(TrapExecutorTest, TrapWithoutReasonCompletes) {
  mc_actor *a = saga_executor_spawn(trap_no_reason, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);

  int64_t status;
  saga_task_wait(a, &status);

  EXPECT_GE(status, MC_ACTOR_COMPLETED);
  saga_task_drop(a);
}

/* ═══════════════════════════════════════════════════════════════════════ */
/* P1 end-to-end: trap reason round-trips through Wait → Error interface.  */
/* Mirrors the Saga-level `task.Wait() or |err| { err.Message() }` path.   */
/* ═══════════════════════════════════════════════════════════════════════ */

/* Nested call: the trap happens several frames below the actor's entry,
   exercising the thread-local current-actor lookup (P1.1). */
static void trap_deep_inner(void) {
  mc_string *reason = mc_arena_alloc_string(
      mc_get_current_actor()->arena, "boom", 4);
  saga_actor_trap(reason);
}
static void trap_deep_mid(void)   { trap_deep_inner(); }
static void trap_deep_entry(mc_actor *a) { (void)a; trap_deep_mid(); }

typedef mc_string *(*msg_fn_t)(void *);

TEST_F(TrapExecutorTest, WaitErrorBranchCarriesTrapReason) {
  mc_actor *a = saga_executor_spawn(trap_deep_entry, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);

  int64_t status = 0;
  saga_task_wait(a, &status);

  /* Worker loop reclassifies ZOMBIE as-is (not COMPLETED).  The only
     thing the Wait() lowering branches on is status != COMPLETED. */
  EXPECT_EQ(status, MC_ACTOR_ZOMBIE);

  /* Build the Error fat pointer the same way codegen does. */
  mc_iface_fat_ptr *fat = (mc_iface_fat_ptr *)saga_error_from_trap(a);
  ASSERT_NE(fat, nullptr);
  ASSERT_NE(fat->data, nullptr);
  ASSERT_NE(fat->vtable, nullptr);

  /* Dispatch Message() through the vtable, just like the codegen'd
     `err.Message()` call would. */
  auto *vt = (mc_trap_error_vtable *)fat->vtable;
  auto msg_fn = (msg_fn_t)vt->message_fn;
  mc_string *msg = msg_fn(fat->data);
  ASSERT_NE(msg, nullptr);
  EXPECT_EQ(msg->len, 4);
  EXPECT_EQ(std::memcmp(msg->data, "boom", 4), 0);

  saga_release_string(msg);
  free(fat->data);
  free(fat);
  saga_task_drop(a);
}

/* Success path: ctx.Exit copies the result onto the heap before longjmp,
   so the returned pointer must contain the value we exited with.  This
   regression-tests the bug where the worker loop's memcpy of
   result_in_arena could read clobbered stack memory. */
static void exit_with_forty_two(mc_actor *a) {
  int64_t val = 42;
  saga_context_exit(a, &val, sizeof(val));
}

TEST_F(TrapExecutorTest, ExitResultSurvivesLongjmp) {
  mc_actor *a = saga_executor_spawn(exit_with_forty_two, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);

  int64_t status = 0;
  void *result = saga_task_wait(a, &status);

  EXPECT_EQ(status, MC_ACTOR_COMPLETED);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(*(int64_t *)result, 42);

  saga_task_drop(a);
}
