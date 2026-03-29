/* Executor, actor, and deque tests for the Saga runtime. */

#include <gtest/gtest.h>
#include <csetjmp>
#include <cstring>
#include <atomic>
#include <vector>
#include <thread>
#include <chrono>
#include <pthread.h>

extern "C" {

/* ── Types from runtime.c ─────────────────────────────────────────────── */

typedef struct {
  char    *base;
  int64_t  offset;
  int64_t  committed;
  int64_t  reserved;
  int64_t  max_limit;
} mc_arena;

typedef struct mc_channel mc_channel;

typedef struct mc_actor {
  int64_t          refcount;
  void            *result;
  int64_t          result_size;
  int64_t          status;
  int64_t          cancelled;
  pthread_mutex_t  lock;
  pthread_cond_t   done_cond;
  mc_arena        *arena;
  void           (*entry)(struct mc_actor *);
  void            *closure_data;
  int64_t          closure_size;
  int64_t          reduction_count;
  int64_t          last_cycle;
  mc_channel      *channel;
  jmp_buf          trap;
} mc_actor;

enum {
  MC_ACTOR_PENDING   = 0,
  MC_ACTOR_RUNNING   = 1,
  MC_ACTOR_COMPLETED = 2,
  MC_ACTOR_CANCELLED = 3,
  MC_ACTOR_KILLED    = 4,
  MC_ACTOR_ZOMBIE    = 5
};

typedef struct {
  mc_actor      **buffer;
  int64_t         head;
  int64_t         tail;
  int64_t         cap;
  pthread_mutex_t lock;
} mc_deque;

/* ── Functions under test ─────────────────────────────────────────────── */

mc_arena *mc_arena_new(int64_t max_limit);
void     *mc_arena_alloc(mc_arena *a, int64_t size);
void      mc_arena_destroy(mc_arena *a);

mc_actor *mc_actor_new(void (*entry)(mc_actor *), void *closure_data,
                       int64_t closure_size, int64_t arena_max);
void      mc_actor_retain(mc_actor *a);
void      mc_actor_release(mc_actor *a);

void      mc_deque_push(mc_deque *d, mc_actor *actor);
mc_actor *mc_deque_pop(mc_deque *d);
mc_actor *mc_deque_steal(mc_deque *d);
void      mc_deque_drain(mc_deque *src, mc_deque *dst);

void      mc_executor_init(int64_t num_workers);
mc_actor *mc_executor_spawn(void (*entry)(mc_actor *), void *closure_data,
                            int64_t closure_size, int64_t arena_max);
void      mc_executor_shutdown(void);
void      mc_executor_replace_worker(int64_t worker_id);

} // extern "C"

/* ── Helpers ───────────────────────────────────────────────────────────── */

/* Deque init/destroy are static in runtime.c, so we replicate init here
   for standalone deque tests. */
static void test_deque_init(mc_deque *d) {
  d->buffer = (mc_actor **)calloc(256, sizeof(mc_actor *));
  d->head = 0;
  d->tail = 0;
  d->cap  = 256;
  pthread_mutex_init(&d->lock, NULL);
}

static void test_deque_destroy(mc_deque *d) {
  free(d->buffer);
  pthread_mutex_destroy(&d->lock);
}

/* A trivial actor entry that does nothing. */
static void noop_entry(mc_actor *) {}

/* An entry that writes a marker value into its result so we can check it. */
static void marker_entry(mc_actor *a) {
  /* Store a 64-bit marker in result.  We malloc it here because the
     worker loop expects result to be heap-allocated (survives arena). */
  a->result = malloc(sizeof(int64_t));
  a->result_size = sizeof(int64_t);
  *(int64_t *)a->result = 0xCAFECAFE;
}

/* An entry that busy-waits until cancelled. */
static void wait_for_cancel_entry(mc_actor *a) {
  while (!__atomic_load_n(&a->cancelled, __ATOMIC_ACQUIRE)) {
    /* spin */
  }
  /* Exit gracefully — status will be set to COMPLETED by the worker. */
}

/* ═══════════════════════════════════════════════════════════════════════ */
/* Actor unit tests                                                       */
/* ═══════════════════════════════════════════════════════════════════════ */

TEST(ActorTest, NewAndRelease) {
  mc_actor *a = mc_actor_new(noop_entry, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);
  EXPECT_EQ(a->refcount, 2);
  EXPECT_EQ(a->status, MC_ACTOR_PENDING);
  EXPECT_NE(a->arena, nullptr);
  EXPECT_EQ(a->closure_data, nullptr);

  /* Simulate the two owners dropping. */
  mc_actor_release(a); /* refcount → 1 */
  mc_actor_release(a); /* refcount → 0 → freed */
  /* No crash = success. */
}

TEST(ActorTest, ClosureDataCopiedIntoArena) {
  int64_t payload = 42;
  mc_actor *a = mc_actor_new(noop_entry, &payload, sizeof(payload), 0);
  ASSERT_NE(a, nullptr);
  ASSERT_NE(a->closure_data, nullptr);

  /* The data should be inside the arena region. */
  auto addr = reinterpret_cast<uintptr_t>(a->closure_data);
  auto base = reinterpret_cast<uintptr_t>(a->arena->base);
  EXPECT_GE(addr, base);
  EXPECT_LT(addr, base + (uintptr_t)a->arena->reserved);

  /* Verify the copied value. */
  EXPECT_EQ(*(int64_t *)a->closure_data, 42);

  mc_actor_release(a);
  mc_actor_release(a);
}

TEST(ActorTest, RetainIncrementsRefcount) {
  mc_actor *a = mc_actor_new(noop_entry, nullptr, 0, 0);
  EXPECT_EQ(a->refcount, 2);
  mc_actor_retain(a);
  EXPECT_EQ(a->refcount, 3);
  mc_actor_release(a);
  mc_actor_release(a);
  mc_actor_release(a);
}

/* ═══════════════════════════════════════════════════════════════════════ */
/* Deque unit tests                                                       */
/* ═══════════════════════════════════════════════════════════════════════ */

TEST(DequeTest, PushAndPop) {
  mc_deque d;
  test_deque_init(&d);

  mc_actor *a1 = mc_actor_new(noop_entry, nullptr, 0, 0);
  mc_actor *a2 = mc_actor_new(noop_entry, nullptr, 0, 0);

  mc_deque_push(&d, a1);
  mc_deque_push(&d, a2);

  /* Pop is LIFO — should get a2 first. */
  mc_actor *got = mc_deque_pop(&d);
  EXPECT_EQ(got, a2);
  got = mc_deque_pop(&d);
  EXPECT_EQ(got, a1);
  got = mc_deque_pop(&d);
  EXPECT_EQ(got, nullptr); /* empty */

  test_deque_destroy(&d);
  mc_actor_release(a1); mc_actor_release(a1);
  mc_actor_release(a2); mc_actor_release(a2);
}

TEST(DequeTest, StealIsFIFO) {
  mc_deque d;
  test_deque_init(&d);

  mc_actor *a1 = mc_actor_new(noop_entry, nullptr, 0, 0);
  mc_actor *a2 = mc_actor_new(noop_entry, nullptr, 0, 0);
  mc_actor *a3 = mc_actor_new(noop_entry, nullptr, 0, 0);

  mc_deque_push(&d, a1);
  mc_deque_push(&d, a2);
  mc_deque_push(&d, a3);

  /* Steal is FIFO — should get a1 first. */
  EXPECT_EQ(mc_deque_steal(&d), a1);
  EXPECT_EQ(mc_deque_steal(&d), a2);
  EXPECT_EQ(mc_deque_steal(&d), a3);
  EXPECT_EQ(mc_deque_steal(&d), nullptr);

  test_deque_destroy(&d);
  mc_actor_release(a1); mc_actor_release(a1);
  mc_actor_release(a2); mc_actor_release(a2);
  mc_actor_release(a3); mc_actor_release(a3);
}

TEST(DequeTest, PopEmptyReturnsNull) {
  mc_deque d;
  test_deque_init(&d);
  EXPECT_EQ(mc_deque_pop(&d), nullptr);
  test_deque_destroy(&d);
}

TEST(DequeTest, StealEmptyReturnsNull) {
  mc_deque d;
  test_deque_init(&d);
  EXPECT_EQ(mc_deque_steal(&d), nullptr);
  test_deque_destroy(&d);
}

TEST(DequeTest, Drain) {
  mc_deque src, dst;
  test_deque_init(&src);
  test_deque_init(&dst);

  mc_actor *a1 = mc_actor_new(noop_entry, nullptr, 0, 0);
  mc_actor *a2 = mc_actor_new(noop_entry, nullptr, 0, 0);
  mc_deque_push(&src, a1);
  mc_deque_push(&src, a2);

  mc_deque_drain(&src, &dst);

  /* src should be empty. */
  EXPECT_EQ(mc_deque_pop(&src), nullptr);

  /* dst should have both actors (drain steals FIFO, push LIFO — order may
     differ, but both should be there). */
  mc_actor *g1 = mc_deque_pop(&dst);
  mc_actor *g2 = mc_deque_pop(&dst);
  ASSERT_NE(g1, nullptr);
  ASSERT_NE(g2, nullptr);
  EXPECT_NE(g1, g2);
  EXPECT_EQ(mc_deque_pop(&dst), nullptr);

  test_deque_destroy(&src);
  test_deque_destroy(&dst);
  mc_actor_release(a1); mc_actor_release(a1);
  mc_actor_release(a2); mc_actor_release(a2);
}

/* ── Wait helper ───────────────────────────────────────────────────────── */
/* The worker publishes the final status under actor->lock, so we must     */
/* check it there.  This ensures arena cleanup is complete before return.  */
static void wait_for_actor(mc_actor *a) {
  pthread_mutex_lock(&a->lock);
  while (a->status < MC_ACTOR_COMPLETED)
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
    mc_executor_init(2); /* 2 workers for determinism */
  }
  void TearDown() override {
    mc_executor_shutdown();
  }
};

TEST_F(ExecutorTest, SpawnNoopCompletes) {
  mc_actor *a = mc_executor_spawn(noop_entry, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);

  wait_for_actor(a);

  EXPECT_EQ(a->status, MC_ACTOR_COMPLETED);
  EXPECT_EQ(a->arena, nullptr); /* arena destroyed by worker */

  mc_actor_release(a); /* drop parent ref */
}

TEST_F(ExecutorTest, SpawnWithMarker) {
  mc_actor *a = mc_executor_spawn(marker_entry, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);

  wait_for_actor(a);

  EXPECT_EQ(a->status, MC_ACTOR_COMPLETED);
  ASSERT_NE(a->result, nullptr);
  EXPECT_EQ(*(int64_t *)a->result, (int64_t)0xCAFECAFE);

  mc_actor_release(a);
}

TEST_F(ExecutorTest, SpawnMultiple) {
  const int N = 50;
  std::vector<mc_actor *> actors(N);

  for (int i = 0; i < N; i++) {
    actors[i] = mc_executor_spawn(noop_entry, nullptr, 0, 0);
    ASSERT_NE(actors[i], nullptr);
  }

  for (int i = 0; i < N; i++) {
    wait_for_actor(actors[i]);
    EXPECT_EQ(actors[i]->status, MC_ACTOR_COMPLETED);
    mc_actor_release(actors[i]);
  }
}

TEST_F(ExecutorTest, SpawnWithClosureData) {
  int64_t payload = 99;
  mc_actor *a = mc_executor_spawn(noop_entry, &payload, sizeof(payload), 0);
  ASSERT_NE(a, nullptr);

  wait_for_actor(a);

  EXPECT_EQ(a->status, MC_ACTOR_COMPLETED);
  mc_actor_release(a);
}

/* ── Cancellation ──────────────────────────────────────────────────────── */

TEST_F(ExecutorTest, CancelFlag) {
  mc_actor *a = mc_executor_spawn(wait_for_cancel_entry, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);

  /* Give the actor a moment to start. */
  std::this_thread::sleep_for(std::chrono::milliseconds(10));

  /* Set the cancel flag. */
  __atomic_store_n(&a->cancelled, 1, __ATOMIC_RELEASE);

  wait_for_actor(a);

  EXPECT_EQ(a->status, MC_ACTOR_COMPLETED);
  mc_actor_release(a);
}

/* ── setjmp/longjmp kill path ──────────────────────────────────────────── */

static void self_kill_entry(mc_actor *a) {
  /* Set the desired terminal status, then longjmp.  The worker loop
     captures this via final_status and publishes it under the lock
     after arena cleanup. */
  a->status = MC_ACTOR_KILLED;
  longjmp(a->trap, 1);
}

TEST_F(ExecutorTest, LongjmpKillPath) {
  mc_actor *a = mc_executor_spawn(self_kill_entry, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);

  wait_for_actor(a);

  EXPECT_EQ(a->status, MC_ACTOR_KILLED);
  EXPECT_EQ(a->arena, nullptr); /* arena destroyed after longjmp */
  mc_actor_release(a);
}

/* ── Worker replacement ────────────────────────────────────────────────── */

TEST_F(ExecutorTest, ReplaceWorkerAndContinue) {
  /* Replace worker 0 — should not crash or deadlock. */
  mc_executor_replace_worker(0);

  /* Spawn work after the replacement and verify it completes. */
  mc_actor *a = mc_executor_spawn(noop_entry, nullptr, 0, 0);
  ASSERT_NE(a, nullptr);

  wait_for_actor(a);

  EXPECT_EQ(a->status, MC_ACTOR_COMPLETED);
  mc_actor_release(a);
}
