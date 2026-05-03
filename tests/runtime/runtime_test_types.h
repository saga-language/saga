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
} saga_runtime_arena;

/* ── Channel (opaque in tests unless testing channel internals) ────────── */

typedef struct saga_runtime_channel saga_runtime_channel;

/* ── Actor status ──────────────────────────────────────────────────────── */

enum {
  SAGA_RUNTIME_ACTOR_PENDING   = 0,
  SAGA_RUNTIME_ACTOR_RUNNING   = 1,
  SAGA_RUNTIME_ACTOR_COMPLETED = 2,
  SAGA_RUNTIME_ACTOR_CANCELLED = 3,
  SAGA_RUNTIME_ACTOR_KILLED    = 4,
  SAGA_RUNTIME_ACTOR_ZOMBIE    = 5
};

/* ── Actor ─────────────────────────────────────────────────────────────── */

typedef struct saga_runtime_actor {
  /* -- Stable fields (malloc'd, outlive the arena) ---------------------- */
  int64_t          refcount;
  void            *result;
  int64_t          result_size;
  int64_t          status;
  int64_t          cancelled;
  pthread_mutex_t  lock;
  pthread_cond_t   done_cond;

  /* -- Arena-lifetime fields -------------------------------------------- */
  saga_runtime_arena        *arena;
  void           (*entry)(struct saga_runtime_actor *);
  void            *closure_data;
  int64_t          closure_size;
  int64_t          reduction_count;
  int64_t          last_cycle;
  saga_runtime_channel      *channel;
  void            *result_in_arena;
  jmp_buf          trap;
} saga_runtime_actor;

/* ── Deque ─────────────────────────────────────────────────────────────── */

typedef struct {
  saga_runtime_actor      **buffer;
  int64_t         head;
  int64_t         tail;
  int64_t         cap;
  pthread_mutex_t lock;
} saga_runtime_deque;

/* ── Arena API ─────────────────────────────────────────────────────────── */

saga_runtime_arena   *saga_runtime_arena_new(int64_t max_limit);
void       *saga_runtime_arena_alloc(saga_runtime_arena *a, int64_t size);
void        saga_runtime_arena_destroy(saga_runtime_arena *a);

typedef struct {
  const char *data;
  int64_t len;
  int64_t refcount;
} saga_runtime_string;

typedef struct {
  void *data;
  int64_t len;
  int64_t cap;
  int64_t elem_size;
  int64_t refcount;
} saga_runtime_array;

saga_runtime_string  *saga_runtime_arena_alloc_string(saga_runtime_arena *a, const char *buf, int64_t len);
saga_runtime_array   *saga_runtime_arena_alloc_array(saga_runtime_arena *a, int64_t elem_size,
                                 int64_t initial_cap);

/* ── COW helpers ───────────────────────────────────────────────────────── */

saga_runtime_string  *saga_runtime_cow_copy_string(saga_runtime_arena *a, saga_runtime_string *src);
saga_runtime_array   *saga_runtime_cow_copy_array(saga_runtime_arena *a, saga_runtime_array *src);

/* ── Refcount helpers (for COW tests) ──────────────────────────────────── */

void        saga_retain_string(saga_runtime_string *s);
void        saga_release_string(saga_runtime_string *s);
void        saga_retain_array(saga_runtime_array *arr);
void        saga_release_array(saga_runtime_array *arr);

/* ── Actor API ─────────────────────────────────────────────────────────── */

saga_runtime_actor   *saga_runtime_actor_new(void (*entry)(saga_runtime_actor *), void *closure_data,
                         int64_t closure_size, int64_t arena_max);
void        saga_runtime_actor_retain(saga_runtime_actor *a);
void        saga_runtime_actor_release(saga_runtime_actor *a);
void        saga_reduction_tick(saga_runtime_actor *a);
void        saga_actor_yield(void);
void        saga_actor_trap(saga_runtime_string *reason);
saga_runtime_actor   *saga_runtime_get_current_actor(void);

/* Test-only: publish a thread's current actor so unit tests can exercise
   intrinsics that read it via the thread-local. The production runtime
   sets this internally from the worker loop. */
void        saga_runtime_set_current_actor_for_test(saga_runtime_actor *a);

/* ── Deque API ─────────────────────────────────────────────────────────── */

void        saga_runtime_deque_push(saga_runtime_deque *d, saga_runtime_actor *actor);
saga_runtime_actor   *saga_runtime_deque_pop(saga_runtime_deque *d);
saga_runtime_actor   *saga_runtime_deque_steal(saga_runtime_deque *d);
void        saga_runtime_deque_drain(saga_runtime_deque *src, saga_runtime_deque *dst);

/* ── Executor API ──────────────────────────────────────────────────────── */

void        saga_executor_init(int64_t num_workers);
saga_runtime_actor   *saga_executor_spawn(void (*entry)(saga_runtime_actor *), void *closure_data,
                              int64_t closure_size, int64_t arena_max);
void        saga_executor_schedule(saga_runtime_actor *actor);
void        saga_executor_shutdown(void);
void        saga_runtime_executor_replace_worker(int64_t worker_id);

/* ── Task API ──────────────────────────────────────────────────────────── */

int64_t     saga_task_alive(saga_runtime_actor *a);
void        saga_task_cancel(saga_runtime_actor *a);
void        saga_task_term(saga_runtime_actor *a);
void       *saga_task_wait(saga_runtime_actor *a, int64_t *out_status);
void        saga_task_drop(saga_runtime_actor *a);

/* ── Context API ───────────────────────────────────────────────────────── */

int64_t     saga_context_cancelled(saga_runtime_actor *a);
void        saga_context_exit(saga_runtime_actor *a, void *value, int64_t size);
int         saga_context_send(saga_runtime_actor *a, const void *data);

/* ── Error interface plumbing (used by Task.Wait()'s error branch) ─────── */

void       *saga_error_from_trap(saga_runtime_actor *a);

typedef struct {
  void *message_fn;
} saga_runtime_trap_error_vtable;

typedef struct {
  void *data;
  void *vtable;
} saga_runtime_iface_fat_ptr;

/* ── Channel API ───────────────────────────────────────────────────────── */

saga_runtime_channel *saga_channel_new(int64_t elem_size, int64_t capacity);
int         saga_runtime_channel_send(saga_runtime_channel *ch, const void *data, saga_runtime_actor *actor);
int         saga_channel_recv(saga_runtime_channel *ch, void *out_buf);
void        saga_channel_close(saga_runtime_channel *ch);
void        saga_channel_destroy(saga_runtime_channel *ch);

} // extern "C"

#endif // SAGA_RUNTIME_TEST_TYPES_H
