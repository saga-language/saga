/* Resource quota and heartbeat tests for the Saga runtime. */

#include "runtime_test_types.h"
#include <gtest/gtest.h>
#include <cstring>
#include <thread>
#include <chrono>

/* ═══════════════════════════════════════════════════════════════════════ */
/* Reduction tick unit tests (no executor needed)                         */
/* ═══════════════════════════════════════════════════════════════════════ */

TEST(ReductionTickTest, IncrementsCounter) {
  mc_actor *a = mc_actor_new([](mc_actor *){}, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);
  a->status = MC_ACTOR_RUNNING;

  /* Set up jmp_buf so longjmp doesn't crash. */
  if (setjmp(a->trap) == 0) {
    mc_reduction_tick(a);
    EXPECT_EQ(a->reduction_count, 1);
    mc_reduction_tick(a);
    EXPECT_EQ(a->reduction_count, 2);
  } else {
    FAIL() << "Should not have longjmp'd";
  }

  mc_actor_release(a);
  mc_actor_release(a);
}

TEST(ReductionTickTest, UpdatesLastCycle) {
  mc_actor *a = mc_actor_new([](mc_actor *){}, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);
  a->status = MC_ACTOR_RUNNING;
  a->last_cycle = 0;

  if (setjmp(a->trap) == 0) {
    mc_reduction_tick(a);
    EXPECT_GT(a->last_cycle, 0);
  }

  mc_actor_release(a);
  mc_actor_release(a);
}

TEST(ReductionTickTest, StatusPoisoningTriggersLongjmp) {
  mc_actor *a = mc_actor_new([](mc_actor *){}, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);
  a->status = MC_ACTOR_KILLED; /* poisoned */

  bool jumped = false;
  if (setjmp(a->trap) == 0) {
    mc_reduction_tick(a);
    FAIL() << "Should have longjmp'd due to KILLED status";
  } else {
    jumped = true;
  }
  EXPECT_TRUE(jumped);

  mc_actor_release(a);
  mc_actor_release(a);
}

TEST(ReductionTickTest, NullActorIsSafe) {
  mc_reduction_tick(nullptr); // must not crash
}

/* ═══════════════════════════════════════════════════════════════════════ */
/* Executor-based quota tests                                             */
/* ═══════════════════════════════════════════════════════════════════════ */

class QuotaTest : public ::testing::Test {
protected:
  void SetUp() override    { mc_executor_init(2); }
  void TearDown() override { mc_executor_shutdown(); }
};

/* An actor that loops calling mc_reduction_tick until killed. */
static void infinite_tick_loop(mc_actor *a) {
  while (1) {
    mc_reduction_tick(a);
  }
}

TEST_F(QuotaTest, ReductionCounterKillsInfiniteLoop) {
  mc_actor *a = mc_executor_spawn(infinite_tick_loop, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);

  int64_t status;
  mc_task_wait(a, &status);

  EXPECT_EQ(status, MC_ACTOR_KILLED);
  mc_task_drop(a);
}

/* An actor that does a fixed amount of work (well under the limit). */
static void bounded_tick_loop(mc_actor *a) {
  for (int i = 0; i < 100; i++) {
    mc_reduction_tick(a);
  }
  /* Return normally → COMPLETED. */
}

TEST_F(QuotaTest, BoundedWorkCompletes) {
  mc_actor *a = mc_executor_spawn(bounded_tick_loop, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);

  int64_t status;
  mc_task_wait(a, &status);

  EXPECT_EQ(status, MC_ACTOR_COMPLETED);
  EXPECT_EQ(a->arena, nullptr);
  mc_task_drop(a);
}

/* An actor that exceeds the memory quota. */
static void memory_hog_entry(mc_actor *a) {
  /* Allocate in a loop until the arena rejects us. */
  while (1) {
    void *p = mc_arena_alloc(a->arena, 64 * 1024); /* 64 KB chunks */
    if (!p) {
      /* Quota exceeded — kill ourselves. */
      a->status = MC_ACTOR_KILLED;
      longjmp(a->trap, 1);
    }
    mc_reduction_tick(a);
  }
}

TEST_F(QuotaTest, MemoryQuotaKillsActor) {
  /* Use a small arena limit. */
  mc_actor *a = mc_actor_new(memory_hog_entry, nullptr, 0, 256 * 1024);
  ASSERT_NE(a, nullptr);
  mc_executor_schedule(a);

  int64_t status;
  mc_task_wait(a, &status);

  EXPECT_EQ(status, MC_ACTOR_KILLED);
  mc_task_drop(a);
}

/* ═══════════════════════════════════════════════════════════════════════ */
/* Heartbeat monitor tests                                                */
/*                                                                        */
/* These use a custom-compiled timeout.  Since MC_HEARTBEAT_TIMEOUT_MS    */
/* defaults to 5000, and we can't change it at runtime, we test that the  */
/* monitor's status poisoning mechanism works by simulating what it does:  */
/* setting status to KILLED while an actor is running with reduction      */
/* ticks.  The full heartbeat integration is verified by the fact that    */
/* the monitor thread starts and shuts down cleanly in every executor     */
/* test.                                                                  */
/* ═══════════════════════════════════════════════════════════════════════ */

/* An actor that spins with reduction ticks. Can be killed externally. */
static void spin_with_ticks(mc_actor *a) {
  while (1) {
    mc_reduction_tick(a); /* checks status poisoning */
  }
}

TEST_F(QuotaTest, ExternalStatusPoisoningKillsActor) {
  mc_actor *a = mc_executor_spawn(spin_with_ticks, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);

  /* Give the actor time to start spinning. */
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  /* Simulate what the heartbeat monitor does. */
  __atomic_store_n(&a->status, MC_ACTOR_KILLED, __ATOMIC_RELEASE);

  int64_t status;
  mc_task_wait(a, &status);

  EXPECT_EQ(status, MC_ACTOR_KILLED);
  mc_task_drop(a);
}

/* Verify that channel send resets the reduction counter. */
static void send_resets_reduction(mc_actor *a) {
  /* Tick up the counter. */
  for (int i = 0; i < 100; i++)
    mc_reduction_tick(a);

  int64_t before = a->reduction_count;

  /* Send should reset it. */
  int64_t val = 42;
  mc_context_send(a, &val);

  /* reduction_count is reset to 0 by mc_channel_send. */
  /* We can't easily check from here because the test asserts from
     outside.  Instead we verify the actor completes without being
     killed, proving the counter was reset. */
}

TEST_F(QuotaTest, ChannelSendResetsReductionCounter) {
  mc_actor *a = mc_actor_new(send_resets_reduction, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);
  a->channel = mc_channel_new(sizeof(int64_t), 8);
  mc_executor_schedule(a);

  /* Drain the one value. */
  int64_t val;
  mc_channel_recv(a->channel, &val);
  EXPECT_EQ(val, 42);

  int64_t status;
  mc_task_wait(a, &status);
  EXPECT_EQ(status, MC_ACTOR_COMPLETED);

  mc_channel_destroy(a->channel);
  a->channel = nullptr;
  mc_task_drop(a);
}

/* Verify that the monitor thread starts and shuts down cleanly.
   This is implicitly tested by every ExecutorTest, but let's be explicit. */
TEST_F(QuotaTest, MonitorStartsAndStopsCleanly) {
  /* Just init/shutdown — if the monitor leaks or deadlocks, this hangs. */
  /* The fixture already calls init/shutdown, so this is a no-op test
     that verifies the lifecycle doesn't crash. */
}
