/* Shared type declarations for runtime tests.
 *
 * These must exactly mirror the struct layouts in src/runtime/runtime.c.
 * Keep this file in sync whenever the runtime structs change.
 */

#ifndef SAGA_RUNTIME_TEST_TYPES_H
#define SAGA_RUNTIME_TEST_TYPES_H

#include <csetjmp>
#include <cstdint>
#include <pthread.h>

extern "C" {

/* ── Arena ─────────────────────────────────────────────────────────────── */

typedef struct {
  char    *base;
  int64_t  offset;
  int64_t  committed;
  int64_t  reserved;
  int64_t  max_limit;
} mc_arena;

/* ── Channel (opaque in tests unless testing channel internals) ────────── */

typedef struct mc_channel mc_channel;

/* ── Actor status ──────────────────────────────────────────────────────── */

enum {
  MC_ACTOR_PENDING   = 0,
  MC_ACTOR_RUNNING   = 1,
  MC_ACTOR_COMPLETED = 2,
  MC_ACTOR_CANCELLED = 3,
  MC_ACTOR_KILLED    = 4,
  MC_ACTOR_ZOMBIE    = 5
};

/* ── Actor ─────────────────────────────────────────────────────────────── */

typedef struct mc_actor {
  /* -- Stable fields (malloc'd, outlive the arena) ---------------------- */
  int64_t          refcount;
  void            *result;
  int64_t          result_size;
  int64_t          status;
  int64_t          cancelled;
  pthread_mutex_t  lock;
  pthread_cond_t   done_cond;

  /* -- Arena-lifetime fields -------------------------------------------- */
  mc_arena        *arena;
  void           (*entry)(struct mc_actor *);
  void            *closure_data;
  int64_t          closure_size;
  int64_t          reduction_count;
  int64_t          last_cycle;
  mc_channel      *channel;
  void            *result_in_arena;
  jmp_buf          trap;
} mc_actor;

/* ── Deque ─────────────────────────────────────────────────────────────── */

typedef struct {
  mc_actor      **buffer;
  int64_t         head;
  int64_t         tail;
  int64_t         cap;
  pthread_mutex_t lock;
} mc_deque;

/* ── Arena API ─────────────────────────────────────────────────────────── */

mc_arena   *mc_arena_new(int64_t max_limit);
void       *mc_arena_alloc(mc_arena *a, int64_t size);
void        mc_arena_destroy(mc_arena *a);

typedef struct {
  const char *data;
  int64_t len;
  int64_t refcount;
} mc_string;

typedef struct {
  void *data;
  int64_t len;
  int64_t cap;
  int64_t elem_size;
  int64_t refcount;
} mc_array;

mc_string  *mc_arena_alloc_string(mc_arena *a, const char *buf, int64_t len);
mc_array   *mc_arena_alloc_array(mc_arena *a, int64_t elem_size,
                                 int64_t initial_cap);

/* ── COW helpers ───────────────────────────────────────────────────────── */

mc_string  *mc_cow_copy_string(mc_arena *a, mc_string *src);
mc_array   *mc_cow_copy_array(mc_arena *a, mc_array *src);

/* ── Refcount helpers (for COW tests) ──────────────────────────────────── */

void        mc_retain_string(mc_string *s);
void        mc_release_string(mc_string *s);
void        mc_retain_array(mc_array *arr);
void        mc_release_array(mc_array *arr);

/* ── Actor API ─────────────────────────────────────────────────────────── */

mc_actor   *mc_actor_new(void (*entry)(mc_actor *), void *closure_data,
                         int64_t closure_size, int64_t arena_max);
void        mc_actor_retain(mc_actor *a);
void        mc_actor_release(mc_actor *a);
void        mc_reduction_tick(mc_actor *a);

/* ── Deque API ─────────────────────────────────────────────────────────── */

void        mc_deque_push(mc_deque *d, mc_actor *actor);
mc_actor   *mc_deque_pop(mc_deque *d);
mc_actor   *mc_deque_steal(mc_deque *d);
void        mc_deque_drain(mc_deque *src, mc_deque *dst);

/* ── Executor API ──────────────────────────────────────────────────────── */

void        mc_executor_init(int64_t num_workers);
mc_actor   *mc_executor_spawn(void (*entry)(mc_actor *), void *closure_data,
                              int64_t closure_size, int64_t arena_max);
void        mc_executor_schedule(mc_actor *actor);
void        mc_executor_shutdown(void);
void        mc_executor_replace_worker(int64_t worker_id);

/* ── Task API ──────────────────────────────────────────────────────────── */

int64_t     mc_task_alive(mc_actor *a);
void        mc_task_cancel(mc_actor *a);
void        mc_task_term(mc_actor *a);
void       *mc_task_wait(mc_actor *a, int64_t *out_status);
void        mc_task_drop(mc_actor *a);

/* ── Context API ───────────────────────────────────────────────────────── */

int64_t     mc_context_cancelled(mc_actor *a);
void        mc_context_exit(mc_actor *a, void *value, int64_t size);
int         mc_context_send(mc_actor *a, const void *data);

/* ── Channel API ───────────────────────────────────────────────────────── */

mc_channel *mc_channel_new(int64_t elem_size, int64_t capacity);
int         mc_channel_send(mc_channel *ch, const void *data, mc_actor *actor);
int         mc_channel_recv(mc_channel *ch, void *out_buf);
void        mc_channel_close(mc_channel *ch);
void        mc_channel_destroy(mc_channel *ch);

} // extern "C"

#endif // SAGA_RUNTIME_TEST_TYPES_H
