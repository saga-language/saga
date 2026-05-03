/* Task & Context API tests for the Saga runtime. */

#include "runtime_test_types.h"
#include <gtest/gtest.h>
#include <cstring>
#include <thread>
#include <chrono>
#include <vector>

/* ── Helper: wait for actor under lock ─────────────────────────────────── */
static void wait_for_actor(saga_runtime_actor *a) {
  pthread_mutex_lock(&a->lock);
  while (a->status < SAGA_RUNTIME_ACTOR_COMPLETED)
    pthread_cond_wait(&a->done_cond, &a->lock);
  pthread_mutex_unlock(&a->lock);
}

/* ═══════════════════════════════════════════════════════════════════════ */
/* Fixture: starts/stops the executor per test.                           */
/* ═══════════════════════════════════════════════════════════════════════ */

class TaskContextTest : public ::testing::Test {
protected:
  void SetUp() override    { saga_executor_init(2); }
  void TearDown() override { saga_executor_shutdown(); }
};

/* ── Actor entries used by tests ───────────────────────────────────────── */

static void noop_entry(saga_runtime_actor *) {}

/* Exit with an int64_t value allocated in the arena. */
static void exit_with_value_entry(saga_runtime_actor *a) {
  int64_t *val = (int64_t *)saga_runtime_arena_alloc(a->arena, sizeof(int64_t));
  *val = 42;
  saga_context_exit(a, val, sizeof(int64_t));
  /* saga_context_exit does not return. */
}

/* Exit with a larger struct. */
struct Payload { int64_t x; int64_t y; };

static void exit_with_struct_entry(saga_runtime_actor *a) {
  Payload *p = (Payload *)saga_runtime_arena_alloc(a->arena, sizeof(Payload));
  p->x = 100;
  p->y = 200;
  saga_context_exit(a, p, sizeof(Payload));
}

/* Poll cancelled and exit cleanly when asked. */
static void poll_cancel_entry(saga_runtime_actor *a) {
  while (!saga_context_cancelled(a)) {
    /* spin */
  }
  /* Return normally — COMPLETED. */
}

/* Send several values through the channel, then exit. */
static void channel_sender_entry(saga_runtime_actor *a) {
  for (int64_t i = 1; i <= 5; i++)
    saga_context_send(a, &i);
  /* Return normally — worker will close the channel. */
}

/* Send values and exit with a result. */
static void send_then_exit_entry(saga_runtime_actor *a) {
  for (int64_t i = 10; i <= 12; i++)
    saga_context_send(a, &i);

  int64_t *val = (int64_t *)saga_runtime_arena_alloc(a->arena, sizeof(int64_t));
  *val = 99;
  saga_context_exit(a, val, sizeof(int64_t));
}

/* Busy-loop forever (used to test term). */
static void infinite_loop_entry(saga_runtime_actor *a) {
  while (1) {
    if (__atomic_load_n(&a->status, __ATOMIC_ACQUIRE) == SAGA_RUNTIME_ACTOR_KILLED)
      longjmp(a->trap, 1);
  }
}

/* ═══════════════════════════════════════════════════════════════════════ */
/* Task API tests                                                         */
/* ═══════════════════════════════════════════════════════════════════════ */

TEST_F(TaskContextTest, WaitReturnsResult) {
  saga_runtime_actor *a = saga_executor_spawn(exit_with_value_entry, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);

  int64_t status;
  void *result = saga_task_wait(a, &status);

  EXPECT_EQ(status, SAGA_RUNTIME_ACTOR_COMPLETED);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(*(int64_t *)result, 42);
  EXPECT_EQ(a->arena, nullptr); /* arena destroyed */

  saga_task_drop(a);
}

TEST_F(TaskContextTest, WaitReturnsStruct) {
  saga_runtime_actor *a = saga_executor_spawn(exit_with_struct_entry, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);

  int64_t status;
  void *result = saga_task_wait(a, &status);

  EXPECT_EQ(status, SAGA_RUNTIME_ACTOR_COMPLETED);
  ASSERT_NE(result, nullptr);
  Payload *p = (Payload *)result;
  EXPECT_EQ(p->x, 100);
  EXPECT_EQ(p->y, 200);

  saga_task_drop(a);
}

TEST_F(TaskContextTest, WaitNoopReturnsNull) {
  saga_runtime_actor *a = saga_executor_spawn(noop_entry, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);

  int64_t status;
  void *result = saga_task_wait(a, &status);

  EXPECT_EQ(status, SAGA_RUNTIME_ACTOR_COMPLETED);
  EXPECT_EQ(result, nullptr); /* no context_exit was called */

  saga_task_drop(a);
}

TEST_F(TaskContextTest, WaitNullStatusPtr) {
  saga_runtime_actor *a = saga_executor_spawn(exit_with_value_entry, nullptr, 0, 0);
  void *result = saga_task_wait(a, nullptr);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(*(int64_t *)result, 42);
  saga_task_drop(a);
}

TEST_F(TaskContextTest, WaitNullActor) {
  int64_t status = -1;
  void *result = saga_task_wait(nullptr, &status);
  EXPECT_EQ(result, nullptr);
  EXPECT_EQ(status, SAGA_RUNTIME_ACTOR_KILLED);
}

TEST_F(TaskContextTest, AliveBeforeCompletion) {
  saga_runtime_actor *a = saga_executor_spawn(poll_cancel_entry, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);

  /* Give it a moment to start. */
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  EXPECT_EQ(saga_task_alive(a), 1);

  saga_task_cancel(a);
  wait_for_actor(a);

  EXPECT_EQ(saga_task_alive(a), 0);
  saga_task_drop(a);
}

TEST_F(TaskContextTest, AliveNullReturnsFalse) {
  EXPECT_EQ(saga_task_alive(nullptr), 0);
}

/* ── Cancel ────────────────────────────────────────────────────────────── */

TEST_F(TaskContextTest, CancelSetsFlag) {
  saga_runtime_actor *a = saga_executor_spawn(poll_cancel_entry, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  saga_task_cancel(a);

  int64_t status;
  saga_task_wait(a, &status);
  EXPECT_EQ(status, SAGA_RUNTIME_ACTOR_COMPLETED); /* exited cleanly after cancel */

  saga_task_drop(a);
}

TEST_F(TaskContextTest, CancelNullIsSafe) {
  saga_task_cancel(nullptr); // must not crash
}

/* ── Term ──────────────────────────────────────────────────────────────── */

TEST_F(TaskContextTest, TermKillsActor) {
  saga_runtime_actor *a = saga_executor_spawn(infinite_loop_entry, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);

  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  saga_task_term(a);

  int64_t status;
  saga_task_wait(a, &status);
  EXPECT_EQ(status, SAGA_RUNTIME_ACTOR_KILLED);

  saga_task_drop(a);
}

TEST_F(TaskContextTest, TermAfterCompletionIsNoop) {
  saga_runtime_actor *a = saga_executor_spawn(noop_entry, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);

  saga_task_wait(a, nullptr);
  saga_task_term(a); // already completed — should be safe
  EXPECT_EQ(a->status, SAGA_RUNTIME_ACTOR_COMPLETED);

  saga_task_drop(a);
}

TEST_F(TaskContextTest, TermNullIsSafe) {
  saga_task_term(nullptr); // must not crash
}

/* ── Drop ──────────────────────────────────────────────────────────────── */

TEST_F(TaskContextTest, DropReleasesRef) {
  saga_runtime_actor *a = saga_executor_spawn(noop_entry, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);
  saga_task_wait(a, nullptr);

  /* After wait, worker has released its ref.  refcount should be 1
     (parent only).  Drop releases the last ref and frees. */
  EXPECT_EQ(a->refcount, 1);
  saga_task_drop(a);
  /* No crash = success. */
}

/* ═══════════════════════════════════════════════════════════════════════ */
/* Context API tests                                                      */
/* ═══════════════════════════════════════════════════════════════════════ */

TEST_F(TaskContextTest, ContextCancelledReflectsTaskCancel) {
  saga_runtime_actor *a = saga_executor_spawn(poll_cancel_entry, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);

  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  /* Actor is polling cancelled — it should still be running. */
  EXPECT_EQ(saga_task_alive(a), 1);

  saga_task_cancel(a);
  saga_task_wait(a, nullptr);
  saga_task_drop(a);
}

TEST_F(TaskContextTest, ContextCancelledNullReturnsFalse) {
  EXPECT_EQ(saga_context_cancelled(nullptr), 0);
}

/* ── context_send + channel iteration ──────────────────────────────────── */

TEST_F(TaskContextTest, SendThroughChannel) {
  /* Create actor, attach channel, THEN schedule — no race. */
  saga_runtime_actor *a = saga_runtime_actor_new(channel_sender_entry, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);
  a->channel = saga_channel_new(sizeof(int64_t), 8);
  ASSERT_NE(a->channel, nullptr);
  saga_executor_schedule(a);

  /* Iterate — recv returns -1 after actor exits and channel is closed. */
  std::vector<int64_t> received;
  int64_t val;
  while (saga_channel_recv(a->channel, &val) == 0)
    received.push_back(val);

  ASSERT_EQ(received.size(), 5u);
  EXPECT_EQ(received[0], 1);
  EXPECT_EQ(received[4], 5);

  saga_channel_destroy(a->channel);
  a->channel = nullptr;
  saga_task_drop(a);
}

TEST_F(TaskContextTest, SendAndExitWithResult) {
  saga_runtime_actor *a = saga_runtime_actor_new(send_then_exit_entry, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);
  a->channel = saga_channel_new(sizeof(int64_t), 8);
  ASSERT_NE(a->channel, nullptr);
  saga_executor_schedule(a);

  /* Drain the channel. */
  std::vector<int64_t> received;
  int64_t val;
  while (saga_channel_recv(a->channel, &val) == 0)
    received.push_back(val);

  ASSERT_EQ(received.size(), 3u);
  EXPECT_EQ(received[0], 10);
  EXPECT_EQ(received[2], 12);

  /* Also get the exit result. */
  int64_t status;
  void *result = saga_task_wait(a, &status);
  EXPECT_EQ(status, SAGA_RUNTIME_ACTOR_COMPLETED);
  ASSERT_NE(result, nullptr);
  EXPECT_EQ(*(int64_t *)result, 99);

  saga_channel_destroy(a->channel);
  a->channel = nullptr;
  saga_task_drop(a);
}

TEST_F(TaskContextTest, SendWithNoChannelReturnsError) {
  saga_runtime_actor *a = saga_executor_spawn(noop_entry, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);
  saga_task_wait(a, nullptr);

  /* context_send with no channel attached should return -1. */
  int64_t val = 1;
  EXPECT_EQ(saga_context_send(a, &val), -1);

  saga_task_drop(a);
}

TEST_F(TaskContextTest, SendNullActorReturnsError) {
  int64_t val = 1;
  EXPECT_EQ(saga_context_send(nullptr, &val), -1);
}

/* ═══════════════════════════════════════════════════════════════════════ */
/* Multiple spawns with results                                           */
/* ═══════════════════════════════════════════════════════════════════════ */

TEST_F(TaskContextTest, MultipleActorsWithResults) {
  const int N = 20;
  std::vector<saga_runtime_actor *> actors(N);

  for (int i = 0; i < N; i++) {
    actors[i] = saga_executor_spawn(exit_with_value_entry, nullptr, 0, 0);
    ASSERT_NE(actors[i], nullptr);
  }

  for (int i = 0; i < N; i++) {
    int64_t status;
    void *result = saga_task_wait(actors[i], &status);
    EXPECT_EQ(status, SAGA_RUNTIME_ACTOR_COMPLETED);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(*(int64_t *)result, 42);
    saga_task_drop(actors[i]);
  }
}
