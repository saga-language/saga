/* Executor, actor, and deque tests for the Saga runtime. */

#include "runtime_test_types.h"
#include <gtest/gtest.h>
#include <csetjmp>
#include <cstring>
#include <atomic>
#include <vector>
#include <thread>
#include <chrono>

/* ── Helpers ───────────────────────────────────────────────────────────── */

/* Deque init/destroy are static in runtime.c, so we replicate init here
   for standalone deque tests. */
static void test_deque_init(saga_runtime_deque *d) {
  d->buffer = (saga_runtime_actor **)calloc(256, sizeof(saga_runtime_actor *));
  d->head = 0;
  d->tail = 0;
  d->cap  = 256;
  pthread_mutex_init(&d->lock, NULL);
}

static void test_deque_destroy(saga_runtime_deque *d) {
  free(d->buffer);
  pthread_mutex_destroy(&d->lock);
}

/* A trivial actor entry that does nothing. */
static void noop_entry(saga_runtime_actor *) {}

/* An entry that writes a marker value into its result so we can check it. */
static void marker_entry(saga_runtime_actor *a) {
  /* Store a 64-bit marker in result.  We malloc it here because the
     worker loop expects result to be heap-allocated (survives arena). */
  a->result = malloc(sizeof(int64_t));
  a->result_size = sizeof(int64_t);
  *(int64_t *)a->result = 0xCAFECAFE;
}

/* An entry that busy-waits until cancelled. */
static void wait_for_cancel_entry(saga_runtime_actor *a) {
  while (!__atomic_load_n(&a->cancelled, __ATOMIC_ACQUIRE)) {
    /* spin */
  }
  /* Exit gracefully — status will be set to COMPLETED by the worker. */
}

/* ═══════════════════════════════════════════════════════════════════════ */
/* Actor unit tests                                                       */
/* ═══════════════════════════════════════════════════════════════════════ */

TEST(ActorTest, NewAndRelease) {
  saga_runtime_actor *a = saga_runtime_actor_new(noop_entry, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->refcount, 2);
  EXPECT_EQ(a->status, SAGA_RUNTIME_ACTOR_PENDING);
  EXPECT_NE(a->arena, nullptr);
  EXPECT_EQ(a->closure_data, nullptr);

  /* Simulate the two owners dropping. */
  saga_runtime_actor_release(a); /* refcount → 1 */
  saga_runtime_actor_release(a); /* refcount → 0 → freed */
  /* No crash = success. */
}

TEST(ActorTest, ClosureDataCopiedIntoArena) {
  int64_t payload = 42;
  saga_runtime_actor *a = saga_runtime_actor_new(noop_entry, &payload, sizeof(payload), 0);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(a->closure_data, nullptr);

  /* The data should be inside the arena region. */
  auto addr = reinterpret_cast<uintptr_t>(a->closure_data);
  auto base = reinterpret_cast<uintptr_t>(a->arena->base);
  EXPECT_GE(addr, base);
  EXPECT_LT(addr, base + (uintptr_t)a->arena->reserved);

  /* Verify the copied value. */
  EXPECT_EQ(*(int64_t *)a->closure_data, 42);

  saga_runtime_actor_release(a);
  saga_runtime_actor_release(a);
}

TEST(ActorTest, RetainIncrementsRefcount) {
  saga_runtime_actor *a = saga_runtime_actor_new(noop_entry, nullptr, 0, 0);
  EXPECT_EQ(a->refcount, 2);
  saga_runtime_actor_retain(a);
  EXPECT_EQ(a->refcount, 3);
  saga_runtime_actor_release(a);
  saga_runtime_actor_release(a);
  saga_runtime_actor_release(a);
}

/* ═══════════════════════════════════════════════════════════════════════ */
/* Deque unit tests                                                       */
/* ═══════════════════════════════════════════════════════════════════════ */

TEST(DequeTest, PushAndPop) {
  saga_runtime_deque d;
  test_deque_init(&d);

  saga_runtime_actor *a1 = saga_runtime_actor_new(noop_entry, nullptr, 0, 0);
  saga_runtime_actor *a2 = saga_runtime_actor_new(noop_entry, nullptr, 0, 0);

  saga_runtime_deque_push(&d, a1);
  saga_runtime_deque_push(&d, a2);

  /* Pop is LIFO — should get a2 first. */
  saga_runtime_actor *got = saga_runtime_deque_pop(&d);
  EXPECT_EQ(got, a2);
  got = saga_runtime_deque_pop(&d);
  EXPECT_EQ(got, a1);
  got = saga_runtime_deque_pop(&d);
  EXPECT_EQ(got, nullptr); /* empty */

  test_deque_destroy(&d);
  saga_runtime_actor_release(a1); saga_runtime_actor_release(a1);
  saga_runtime_actor_release(a2); saga_runtime_actor_release(a2);
}

TEST(DequeTest, StealIsFIFO) {
  saga_runtime_deque d;
  test_deque_init(&d);

  saga_runtime_actor *a1 = saga_runtime_actor_new(noop_entry, nullptr, 0, 0);
  saga_runtime_actor *a2 = saga_runtime_actor_new(noop_entry, nullptr, 0, 0);
  saga_runtime_actor *a3 = saga_runtime_actor_new(noop_entry, nullptr, 0, 0);

  saga_runtime_deque_push(&d, a1);
  saga_runtime_deque_push(&d, a2);
  saga_runtime_deque_push(&d, a3);

  /* Steal is FIFO — should get a1 first. */
  EXPECT_EQ(saga_runtime_deque_steal(&d), a1);
  EXPECT_EQ(saga_runtime_deque_steal(&d), a2);
  EXPECT_EQ(saga_runtime_deque_steal(&d), a3);
  EXPECT_EQ(saga_runtime_deque_steal(&d), nullptr);

  test_deque_destroy(&d);
  saga_runtime_actor_release(a1); saga_runtime_actor_release(a1);
  saga_runtime_actor_release(a2); saga_runtime_actor_release(a2);
  saga_runtime_actor_release(a3); saga_runtime_actor_release(a3);
}

TEST(DequeTest, PopEmptyReturnsNull) {
  saga_runtime_deque d;
  test_deque_init(&d);
  EXPECT_EQ(saga_runtime_deque_pop(&d), nullptr);
  test_deque_destroy(&d);
}

TEST(DequeTest, StealEmptyReturnsNull) {
  saga_runtime_deque d;
  test_deque_init(&d);
  EXPECT_EQ(saga_runtime_deque_steal(&d), nullptr);
  test_deque_destroy(&d);
}

TEST(DequeTest, Drain) {
  saga_runtime_deque src, dst;
  test_deque_init(&src);
  test_deque_init(&dst);

  saga_runtime_actor *a1 = saga_runtime_actor_new(noop_entry, nullptr, 0, 0);
  saga_runtime_actor *a2 = saga_runtime_actor_new(noop_entry, nullptr, 0, 0);
  saga_runtime_deque_push(&src, a1);
  saga_runtime_deque_push(&src, a2);

  saga_runtime_deque_drain(&src, &dst);

  /* src should be empty. */
  EXPECT_EQ(saga_runtime_deque_pop(&src), nullptr);

  /* dst should have both actors (drain steals FIFO, push LIFO — order may
     differ, but both should be there). */
  saga_runtime_actor *g1 = saga_runtime_deque_pop(&dst);
  saga_runtime_actor *g2 = saga_runtime_deque_pop(&dst);
  ASSERT_NE(g1, nullptr);
  ASSERT_NE(g2, nullptr);
  EXPECT_NE(g1, g2);
  EXPECT_EQ(saga_runtime_deque_pop(&dst), nullptr);

  test_deque_destroy(&src);
  test_deque_destroy(&dst);
  saga_runtime_actor_release(a1); saga_runtime_actor_release(a1);
  saga_runtime_actor_release(a2); saga_runtime_actor_release(a2);
}

/* ── Wait helper ───────────────────────────────────────────────────────── */
/* The worker publishes the final status under actor->lock, so we must     */
/* check it there.  This ensures arena cleanup is complete before return.  */
static void wait_for_actor(saga_runtime_actor *a) {
  pthread_mutex_lock(&a->lock);
  while (a->status < SAGA_RUNTIME_ACTOR_COMPLETED)
    pthread_cond_wait(&a->done_cond, &a->lock);
  pthread_mutex_unlock(&a->lock);
}

/* ═══════════════════════════════════════════════════════════════════════ */
/* Executor integration tests                                             */
/*                                                                        */
/* These tests start the executor, spawn actors, and verify they run.     */
/* The executor is a singleton so we init/shutdown per test.               */
/* ═══════════════════════════════════════════════════════════════════════ */

class ExecutorTest : public ::testing::Test {
protected:
  void SetUp() override {
    saga_executor_init(2); /* 2 workers for determinism */
  }
  void TearDown() override {
    saga_executor_shutdown();
  }
};

TEST_F(ExecutorTest, SpawnNoopCompletes) {
  saga_runtime_actor *a = saga_executor_spawn(noop_entry, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);

  wait_for_actor(a);

  EXPECT_EQ(a->status, SAGA_RUNTIME_ACTOR_COMPLETED);
  EXPECT_EQ(a->arena, nullptr); /* arena destroyed by worker */

  saga_runtime_actor_release(a); /* drop parent ref */
}

TEST_F(ExecutorTest, SpawnWithMarker) {
  saga_runtime_actor *a = saga_executor_spawn(marker_entry, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);

  wait_for_actor(a);

  EXPECT_EQ(a->status, SAGA_RUNTIME_ACTOR_COMPLETED);
  ASSERT_NE(a->result, nullptr);
  EXPECT_EQ(*(int64_t *)a->result, (int64_t)0xCAFECAFE);

  saga_runtime_actor_release(a);
}

TEST_F(ExecutorTest, SpawnMultiple) {
  const int N = 50;
  std::vector<saga_runtime_actor *> actors(N);

  for (int i = 0; i < N; i++) {
    actors[i] = saga_executor_spawn(noop_entry, nullptr, 0, 0);
    ASSERT_NE(actors[i], nullptr);
  }

  for (int i = 0; i < N; i++) {
    wait_for_actor(actors[i]);
    EXPECT_EQ(actors[i]->status, SAGA_RUNTIME_ACTOR_COMPLETED);
    saga_runtime_actor_release(actors[i]);
  }
}

TEST_F(ExecutorTest, SpawnWithClosureData) {
  int64_t payload = 99;
  saga_runtime_actor *a = saga_executor_spawn(noop_entry, &payload, sizeof(payload), 0);
  ASSERT_NE(a, nullptr);

  wait_for_actor(a);

  EXPECT_EQ(a->status, SAGA_RUNTIME_ACTOR_COMPLETED);
  saga_runtime_actor_release(a);
}

/* ── Cancellation ──────────────────────────────────────────────────────── */

TEST_F(ExecutorTest, CancelFlag) {
  saga_runtime_actor *a = saga_executor_spawn(wait_for_cancel_entry, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);

  /* Give the actor a moment to start. */
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  /* Set the cancel flag. */
  __atomic_store_n(&a->cancelled, 1, __ATOMIC_RELEASE);

  wait_for_actor(a);

  EXPECT_EQ(a->status, SAGA_RUNTIME_ACTOR_COMPLETED);
  saga_runtime_actor_release(a);
}

/* ── setjmp/longjmp kill path ──────────────────────────────────────────── */

static void self_kill_entry(saga_runtime_actor *a) {
  /* Set the desired terminal status, then longjmp.  The worker loop
     captures this via final_status and publishes it under the lock
     after arena cleanup. */
  a->status = SAGA_RUNTIME_ACTOR_KILLED;
  longjmp(a->trap, 1);
}

TEST_F(ExecutorTest, LongjmpKillPath) {
  saga_runtime_actor *a = saga_executor_spawn(self_kill_entry, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);

  wait_for_actor(a);

  EXPECT_EQ(a->status, SAGA_RUNTIME_ACTOR_KILLED);
  EXPECT_EQ(a->arena, nullptr); /* arena destroyed after longjmp */
  saga_runtime_actor_release(a);
}

/* ── Worker replacement ────────────────────────────────────────────────── */

TEST_F(ExecutorTest, ReplaceWorkerAndContinue) {
  /* Replace worker 0 — should not crash or deadlock. */
  saga_runtime_executor_replace_worker(0);

  /* Spawn work after the replacement and verify it completes. */
  saga_runtime_actor *a = saga_executor_spawn(noop_entry, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);

  wait_for_actor(a);

  EXPECT_EQ(a->status, SAGA_RUNTIME_ACTOR_COMPLETED);
  saga_runtime_actor_release(a);
}
