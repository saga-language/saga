/* Copyright 2026 Rob Thornton
 * SPDX-License-Identifier: MIT
 *
 * Minimal runtime for the Saga language.
 * Compiled into a static library and linked into every Saga binary.
 */

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>
#include <setjmp.h>
#include <time.h>

/* ───────────────────────────────────────────────────────────────────────── */
/* Arena allocator                                                          */
/*                                                                          */
/* Virtual-memory-reservation based bump allocator.  A large virtual region */
/* is reserved up front with mmap(PROT_NONE).  Physical pages are committed */
/* on demand via mprotect as the bump pointer advances.  The base address   */
/* never moves, so all pointers into the arena remain valid for its entire  */
/* lifetime.  Deallocation is a single munmap of the whole region.          */
/*                                                                          */
/* Layout: { char *base, i64 offset, i64 committed, i64 reserved,          */
/*           i64 max_limit }                                                */
/* ───────────────────────────────────────────────────────────────────────── */

#ifndef SAGA_RUNTIME_MAX_ARENA_SIZE
#define SAGA_RUNTIME_MAX_ARENA_SIZE (64 * 1024 * 1024) /* 64 MB default */
#endif

#ifndef SAGA_RUNTIME_MAX_REDUCTIONS
#define SAGA_RUNTIME_MAX_REDUCTIONS 1000000
#endif

#ifndef SAGA_RUNTIME_HEARTBEAT_TIMEOUT_MS
#define SAGA_RUNTIME_HEARTBEAT_TIMEOUT_MS 5000
#endif

#ifndef SAGA_RUNTIME_ARENA_INITIAL_COMMIT
#define SAGA_RUNTIME_ARENA_INITIAL_COMMIT (64 * 1024) /* 64 KB initial commit */
#endif

#define SAGA_RUNTIME_ARENA_ALIGN 16

typedef struct {
  char    *base;      /* mmap'd region — never moves                       */
  int64_t  offset;    /* next free byte (bump pointer)                     */
  int64_t  committed; /* bytes currently backed by physical pages          */
  int64_t  reserved;  /* total virtual reservation                         */
  int64_t  max_limit; /* hard quota — allocation fails if exceeded         */
} saga_runtime_arena;

/* Round `n` up to the nearest multiple of `align` (must be power of 2). */
static inline int64_t saga_runtime_align_up(int64_t n, int64_t align) {
  return (n + align - 1) & ~(align - 1);
}

/* Return the system page size, cached after first call. */
static inline int64_t saga_runtime_page_size(void) {
  static int64_t ps = 0;
  if (ps == 0) ps = (int64_t)sysconf(_SC_PAGESIZE);
  return ps;
}

/*
 * saga_runtime_arena_new  —  reserve virtual address space and commit an initial chunk.
 *
 * `max_limit` is the hard memory quota for this arena.  If 0 the compile-time
 * default SAGA_RUNTIME_MAX_ARENA_SIZE is used.  The virtual reservation is the larger of
 * max_limit and SAGA_RUNTIME_MAX_ARENA_SIZE (virtual space is free on 64-bit).
 *
 * Returns NULL if the mmap or initial commit fails.
 */
saga_runtime_arena *saga_runtime_arena_new(int64_t max_limit) {
  if (max_limit <= 0) max_limit = SAGA_RUNTIME_MAX_ARENA_SIZE;

  int64_t page = saga_runtime_page_size();
  int64_t reserved = max_limit;
  if (reserved < SAGA_RUNTIME_MAX_ARENA_SIZE) reserved = SAGA_RUNTIME_MAX_ARENA_SIZE;
  reserved = saga_runtime_align_up(reserved, page);

  /* Reserve the entire virtual region with no access permissions. */
  void *base = mmap(NULL, (size_t)reserved,
                    PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                    -1, 0);
  if (base == MAP_FAILED) return NULL;

  /* Commit the initial chunk so early allocations don't page-fault. */
  int64_t initial = SAGA_RUNTIME_ARENA_INITIAL_COMMIT;
  if (initial > reserved) initial = reserved;
  initial = saga_runtime_align_up(initial, page);
  if (mprotect(base, (size_t)initial, PROT_READ | PROT_WRITE) != 0) {
    munmap(base, (size_t)reserved);
    return NULL;
  }

  saga_runtime_arena *a = (saga_runtime_arena *)malloc(sizeof(saga_runtime_arena));
  if (!a) {
    munmap(base, (size_t)reserved);
    return NULL;
  }
  a->base      = (char *)base;
  a->offset    = 0;
  a->committed = initial;
  a->reserved  = reserved;
  a->max_limit = max_limit;
  return a;
}

/*
 * saga_runtime_arena_alloc  —  bump-allocate `size` bytes from the arena.
 *
 * Returns a 16-byte-aligned pointer, or NULL if the allocation would exceed
 * the arena's max_limit (the caller is expected to kill the actor).
 */
void *saga_runtime_arena_alloc(saga_runtime_arena *a, int64_t size) {
  if (!a || size <= 0) return NULL;

  int64_t aligned = saga_runtime_align_up(size, SAGA_RUNTIME_ARENA_ALIGN);
  int64_t new_offset = a->offset + aligned;

  /* Hard quota check. */
  if (new_offset > a->max_limit) return NULL;

  /* Commit more pages if needed. */
  if (new_offset > a->committed) {
    int64_t page = saga_runtime_page_size();
    /* Commit in chunks: at least double current committed or enough for the
     * request, whichever is larger, capped at reserved. */
    int64_t want = a->committed * 2;
    if (want < new_offset) want = saga_runtime_align_up(new_offset, page);
    if (want > a->reserved) want = a->reserved;
    want = saga_runtime_align_up(want, page);

    if (want > a->committed) {
      if (mprotect(a->base, (size_t)want, PROT_READ | PROT_WRITE) != 0)
        return NULL;
      a->committed = want;
    }

    /* Even after committing as much as possible, not enough room. */
    if (new_offset > a->committed) return NULL;
  }

  void *ptr = a->base + a->offset;
  a->offset = new_offset;
  return ptr;
}

/*
 * saga_runtime_arena_destroy  —  release the entire virtual region in one call.
 *
 * After this call every pointer into the arena is invalid.
 */
void saga_runtime_arena_destroy(saga_runtime_arena *a) {
  if (!a) return;
  if (a->base) munmap(a->base, (size_t)a->reserved);
  a->base = NULL;
  a->offset = 0;
  a->committed = 0;
  a->reserved = 0;
  free(a);
}

/* ───────────────────────────────────────────────────────────────────────── */
/* Actor                                                                    */
/*                                                                          */
/* An actor is the unit of concurrent execution.  It owns an arena for its  */
/* working memory and is scheduled by the executor on a pool of OS threads. */
/*                                                                          */
/* The saga_runtime_actor struct itself is heap-allocated (malloc) and refcounted so   */
/* it outlives the arena.  The worker holds one ref, the parent's Task      */
/* handle holds another.  The struct is freed when refcount reaches 0.      */
/*                                                                          */
/* Lifecycle: PENDING → RUNNING → COMPLETED | KILLED | ZOMBIE              */
/* ───────────────────────────────────────────────────────────────────────── */

enum {
  SAGA_RUNTIME_ACTOR_PENDING   = 0,
  SAGA_RUNTIME_ACTOR_RUNNING   = 1,
  SAGA_RUNTIME_ACTOR_COMPLETED = 2,
  SAGA_RUNTIME_ACTOR_CANCELLED = 3,
  SAGA_RUNTIME_ACTOR_KILLED    = 4,
  SAGA_RUNTIME_ACTOR_ZOMBIE    = 5
};

/* Forward declarations — full definitions follow below. */
typedef struct saga_runtime_actor   saga_runtime_actor;
typedef struct saga_runtime_channel saga_runtime_channel;
void saga_channel_close(saga_runtime_channel *ch);
int  saga_runtime_channel_send(saga_runtime_channel *ch, const void *data, saga_runtime_actor *actor);

typedef struct saga_runtime_actor {
  /* -- Stable fields (malloc'd, outlive the arena) ---------------------- */
  int64_t          refcount;       /* 1 worker + 1 parent Task handle      */
  void            *result;         /* exit value, copied out before arena   */
  int64_t          result_size;    /*   dies — always malloc'd             */
  int64_t          status;         /* SAGA_RUNTIME_ACTOR_{PENDING,RUNNING,...}        */
  int64_t          cancelled;      /* set by parent Cancel(), read by actor */
  pthread_mutex_t  lock;           /* guards status transitions             */
  pthread_cond_t   done_cond;      /* signalled on completion (for Wait())  */

  /* -- Arena-lifetime fields -------------------------------------------- */
  saga_runtime_arena        *arena;
  void           (*entry)(struct saga_runtime_actor *);
  void            *closure_data;   /* captured vars packed by codegen       */
  int64_t          closure_size;
  int64_t          reduction_count;
  int64_t          last_cycle;     /* monotonic ns timestamp                */
  saga_runtime_channel      *channel;        /* typed channel for Send() / iteration  */
  void            *result_in_arena;/* set by context_exit, lives in arena   */
  jmp_buf          trap;           /* setjmp target for panic / quota       */
} saga_runtime_actor;

/* Monotonic clock helper (nanoseconds). */
static int64_t saga_runtime_monotonic_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

saga_runtime_actor *saga_runtime_actor_new(void (*entry)(saga_runtime_actor *), void *closure_data,
                       int64_t closure_size, int64_t arena_max) {
  saga_runtime_actor *a = (saga_runtime_actor *)calloc(1, sizeof(saga_runtime_actor));
  if (!a) return NULL;

  a->refcount      = 2; /* one for worker, one for parent Task handle */
  a->status        = SAGA_RUNTIME_ACTOR_PENDING;
  a->cancelled     = 0;
  a->result        = NULL;
  a->result_size   = 0;
  a->entry         = entry;
  a->closure_size  = closure_size;
  a->reduction_count = 0;
  a->last_cycle    = saga_runtime_monotonic_now();
  a->channel       = NULL;

  pthread_mutex_init(&a->lock, NULL);
  pthread_cond_init(&a->done_cond, NULL);

  /* Create the actor's arena. */
  a->arena = saga_runtime_arena_new(arena_max);
  if (!a->arena) {
    pthread_mutex_destroy(&a->lock);
    pthread_cond_destroy(&a->done_cond);
    free(a);
    return NULL;
  }

  /* Copy closure data into the actor's arena so it's fully owned. */
  if (closure_data && closure_size > 0) {
    a->closure_data = saga_runtime_arena_alloc(a->arena, closure_size);
    if (!a->closure_data) {
      saga_runtime_arena_destroy(a->arena);
      pthread_mutex_destroy(&a->lock);
      pthread_cond_destroy(&a->done_cond);
      free(a);
      return NULL;
    }
    memcpy(a->closure_data, closure_data, (size_t)closure_size);
  } else {
    a->closure_data = NULL;
  }

  return a;
}

/* Runtime accessor for closure_data — used by codegen to unpack captures
   without hardcoding struct offsets in LLVM IR. */
void *saga_runtime_actor_get_closure(saga_runtime_actor *a) {
  return a ? a->closure_data : NULL;
}

/* Runtime setter for channel — used by codegen to attach a channel after
   saga_executor_spawn() returns the actor. */
void saga_runtime_actor_set_channel(saga_runtime_actor *a, saga_runtime_channel *ch) {
  if (a) a->channel = ch;
}

void saga_runtime_actor_retain(saga_runtime_actor *a) {
  if (!a) return;
  __atomic_add_fetch(&a->refcount, 1, __ATOMIC_SEQ_CST);
}

void saga_runtime_actor_release(saga_runtime_actor *a) {
  if (!a) return;
  int64_t rc = __atomic_sub_fetch(&a->refcount, 1, __ATOMIC_SEQ_CST);
  if (rc <= 0) {
    /* Last ref — free the actor struct itself. */
    if (a->result) free(a->result);
    pthread_mutex_destroy(&a->lock);
    pthread_cond_destroy(&a->done_cond);
    /* Arena should already be destroyed by the worker. */
    free(a);
  }
}

/*
 * saga_reduction_tick  —  called at the top of every loop iteration inside
 *                       a spawned actor.  Checks status poisoning and
 *                       enforces the CPU quota.
 *
 * If the actor has been killed (e.g. by saga_task_term or heartbeat monitor),
 * it longjmps out immediately.  If the reduction counter exceeds the
 * compile-time limit, the actor is killed.
 */
void saga_reduction_tick(saga_runtime_actor *a) {
  if (!a) return;

  /* Status poisoning — killed externally? */
  if (__atomic_load_n(&a->status, __ATOMIC_ACQUIRE) == SAGA_RUNTIME_ACTOR_KILLED)
    longjmp(a->trap, 1);

  a->reduction_count++;
  if (a->reduction_count > SAGA_RUNTIME_MAX_REDUCTIONS) {
    __atomic_store_n(&a->status, SAGA_RUNTIME_ACTOR_KILLED, __ATOMIC_RELEASE);
    longjmp(a->trap, 1);
  }

  a->last_cycle = saga_runtime_monotonic_now();
}

/* ───────────────────────────────────────────────────────────────────────── */
/* Work-stealing deque                                                      */
/*                                                                          */
/* Mutex-protected double-ended queue.  The owning worker pushes/pops from  */
/* the tail (LIFO for cache locality); thieves steal from the head (FIFO).  */
/* ───────────────────────────────────────────────────────────────────────── */

#ifndef SAGA_RUNTIME_DEQUE_INITIAL_CAP
#define SAGA_RUNTIME_DEQUE_INITIAL_CAP 256
#endif

typedef struct {
  saga_runtime_actor      **buffer;
  int64_t         head;  /* steal from here (FIFO end) */
  int64_t         tail;  /* push/pop here  (LIFO end)  */
  int64_t         cap;
  pthread_mutex_t lock;
} saga_runtime_deque;

static void saga_runtime_deque_init(saga_runtime_deque *d) {
  d->buffer = (saga_runtime_actor **)calloc((size_t)SAGA_RUNTIME_DEQUE_INITIAL_CAP,
                                  sizeof(saga_runtime_actor *));
  d->head = 0;
  d->tail = 0;
  d->cap  = SAGA_RUNTIME_DEQUE_INITIAL_CAP;
  pthread_mutex_init(&d->lock, NULL);
}

static void saga_runtime_deque_destroy(saga_runtime_deque *d) {
  if (!d) return;
  free(d->buffer);
  d->buffer = NULL;
  pthread_mutex_destroy(&d->lock);
}

static int64_t saga_runtime_deque_size_locked(saga_runtime_deque *d) {
  return d->tail - d->head;
}

/* Grow the ring buffer (caller must hold the lock). */
static void saga_runtime_deque_grow(saga_runtime_deque *d) {
  int64_t new_cap = d->cap * 2;
  saga_runtime_actor **new_buf = (saga_runtime_actor **)calloc((size_t)new_cap,
                                           sizeof(saga_runtime_actor *));
  int64_t n = saga_runtime_deque_size_locked(d);
  for (int64_t i = 0; i < n; i++)
    new_buf[i] = d->buffer[(d->head + i) % d->cap];
  free(d->buffer);
  d->buffer = new_buf;
  d->head   = 0;
  d->tail   = n;
  d->cap    = new_cap;
}

/* Push an actor onto the tail (owner side). */
void saga_runtime_deque_push(saga_runtime_deque *d, saga_runtime_actor *actor) {
  pthread_mutex_lock(&d->lock);
  if (saga_runtime_deque_size_locked(d) >= d->cap)
    saga_runtime_deque_grow(d);
  d->buffer[d->tail % d->cap] = actor;
  d->tail++;
  pthread_mutex_unlock(&d->lock);
}

/* Pop from the tail (owner side, LIFO). Returns NULL if empty. */
saga_runtime_actor *saga_runtime_deque_pop(saga_runtime_deque *d) {
  pthread_mutex_lock(&d->lock);
  if (saga_runtime_deque_size_locked(d) <= 0) {
    pthread_mutex_unlock(&d->lock);
    return NULL;
  }
  d->tail--;
  saga_runtime_actor *a = d->buffer[d->tail % d->cap];
  pthread_mutex_unlock(&d->lock);
  return a;
}

/* Steal from the head (thief side, FIFO). Returns NULL if empty. */
saga_runtime_actor *saga_runtime_deque_steal(saga_runtime_deque *d) {
  pthread_mutex_lock(&d->lock);
  if (saga_runtime_deque_size_locked(d) <= 0) {
    pthread_mutex_unlock(&d->lock);
    return NULL;
  }
  saga_runtime_actor *a = d->buffer[d->head % d->cap];
  d->head++;
  pthread_mutex_unlock(&d->lock);
  return a;
}

/* Drain all actors from `src` into `dst`.  Used during worker replacement  */
/* to rescue stranded actors before detaching a stuck thread.               */
void saga_runtime_deque_drain(saga_runtime_deque *src, saga_runtime_deque *dst) {
  pthread_mutex_lock(&src->lock);
  while (saga_runtime_deque_size_locked(src) > 0) {
    saga_runtime_actor *a = src->buffer[src->head % src->cap];
    src->head++;
    /* Push into dst (acquires dst lock internally). */
    pthread_mutex_unlock(&src->lock);
    saga_runtime_deque_push(dst, a);
    pthread_mutex_lock(&src->lock);
  }
  pthread_mutex_unlock(&src->lock);
}

/* ───────────────────────────────────────────────────────────────────────── */
/* Current-actor thread-local                                               */
/*                                                                          */
/* Each worker thread publishes the saga_runtime_actor* it is currently executing     */
/* here so that intrinsics (trap, yield) can locate it from any call depth  */
/* without a compile-time reference to the actor pointer.                   */
/* ───────────────────────────────────────────────────────────────────────── */

static __thread saga_runtime_actor *saga_runtime_current_actor = NULL;

saga_runtime_actor *saga_runtime_get_current_actor(void) { return saga_runtime_current_actor; }

/* Test hook — lets unit tests bypass the worker loop and exercise
   intrinsics that depend on the thread-local current actor. */
void saga_runtime_set_current_actor_for_test(saga_runtime_actor *a) { saga_runtime_current_actor = a; }

/* ───────────────────────────────────────────────────────────────────────── */
/* Executor                                                                 */
/*                                                                          */
/* A fixed-size thread pool with per-worker deques and a global inject      */
/* queue.  Workers sleep on a condition variable when idle and are woken     */
/* instantly when work is submitted.                                        */
/* ───────────────────────────────────────────────────────────────────────── */

typedef struct {
  int64_t          id;       /* index into executor arrays                  */
  void            *executor; /* back-pointer (cast to saga_runtime_executor*)         */
} saga_runtime_worker_ctx;

/* ── Active actor list (for heartbeat monitor) ─────────────────────────── */
/* A simple array-based list of actors currently being executed.  Protected */
/* by its own mutex so the monitor thread can scan without contending on    */
/* the work-stealing deques.                                                */

#ifndef SAGA_RUNTIME_ACTIVE_LIST_CAP
#define SAGA_RUNTIME_ACTIVE_LIST_CAP 1024
#endif

typedef struct {
  saga_runtime_actor       **actors;
  int64_t          len;
  int64_t          cap;
  pthread_mutex_t  lock;
} saga_runtime_active_list;

static void saga_runtime_active_list_init(saga_runtime_active_list *l) {
  l->actors = (saga_runtime_actor **)calloc(SAGA_RUNTIME_ACTIVE_LIST_CAP, sizeof(saga_runtime_actor *));
  l->len    = 0;
  l->cap    = SAGA_RUNTIME_ACTIVE_LIST_CAP;
  pthread_mutex_init(&l->lock, NULL);
}

static void saga_runtime_active_list_destroy(saga_runtime_active_list *l) {
  free(l->actors);
  pthread_mutex_destroy(&l->lock);
}

static void saga_runtime_active_list_add(saga_runtime_active_list *l, saga_runtime_actor *a) {
  pthread_mutex_lock(&l->lock);
  if (l->len >= l->cap) {
    l->cap *= 2;
    l->actors = (saga_runtime_actor **)realloc(l->actors,
                                     (size_t)l->cap * sizeof(saga_runtime_actor *));
  }
  l->actors[l->len++] = a;
  pthread_mutex_unlock(&l->lock);
}

static void saga_runtime_active_list_remove(saga_runtime_active_list *l, saga_runtime_actor *a) {
  pthread_mutex_lock(&l->lock);
  for (int64_t i = 0; i < l->len; i++) {
    if (l->actors[i] == a) {
      l->actors[i] = l->actors[l->len - 1];
      l->len--;
      break;
    }
  }
  pthread_mutex_unlock(&l->lock);
}

typedef struct saga_runtime_executor {
  pthread_t       *threads;
  saga_runtime_worker_ctx   *worker_ctxs;
  saga_runtime_deque        *deques;          /* one per worker slot                  */
  int64_t          num_workers;
  saga_runtime_deque         inject_queue;    /* global queue for newly spawned actors*/
  pthread_mutex_t  work_avail_lock;
  pthread_cond_t   work_avail_cond; /* workers sleep here when idle         */
  int64_t          shutdown;        /* flag, checked by workers             */
  saga_runtime_active_list   active;          /* actors currently executing           */
  pthread_t        monitor_thread;  /* heartbeat monitor                    */
  pthread_mutex_t  monitor_lock;    /* protects monitor condvar             */
  pthread_cond_t   monitor_cond;    /* woken on shutdown for fast exit      */
} saga_runtime_executor;

/* Singleton executor — initialised by saga_executor_init(). */
static saga_runtime_executor *g_executor = NULL;

/* Random peer selection for stealing. */
static uint32_t saga_runtime_xorshift32(uint32_t *state) {
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

/* ── Worker thread entry point ─────────────────────────────────────────── */

static void *saga_runtime_worker_loop(void *arg) {
  saga_runtime_worker_ctx *ctx = (saga_runtime_worker_ctx *)arg;
  saga_runtime_executor *ex = (saga_runtime_executor *)ctx->executor;
  int64_t my_id = ctx->id;
  saga_runtime_deque *my_deque = &ex->deques[my_id];

  /* Seed the PRNG with something unique per worker. */
  uint32_t rng = (uint32_t)(my_id + 1) * 2654435761u;

  while (1) {
    /* Check shutdown. */
    if (__atomic_load_n(&ex->shutdown, __ATOMIC_ACQUIRE))
      break;

    /* 1. Try own deque. */
    saga_runtime_actor *actor = saga_runtime_deque_pop(my_deque);

    /* 2. Try the global inject queue. */
    if (!actor)
      actor = saga_runtime_deque_steal(&ex->inject_queue);

    /* 3. Try stealing from a random peer. */
    if (!actor && ex->num_workers > 1) {
      int64_t victim = (int64_t)(saga_runtime_xorshift32(&rng) % (uint32_t)ex->num_workers);
      if (victim == my_id)
        victim = (victim + 1) % ex->num_workers;
      actor = saga_runtime_deque_steal(&ex->deques[victim]);
    }

    /* 4. No work — sleep until signalled. */
    if (!actor) {
      pthread_mutex_lock(&ex->work_avail_lock);
      /* Re-check shutdown and inject queue while holding the lock to
         avoid a missed-wakeup race. */
      if (__atomic_load_n(&ex->shutdown, __ATOMIC_ACQUIRE)) {
        pthread_mutex_unlock(&ex->work_avail_lock);
        break;
      }
      /* Quick non-blocking peek at the inject queue before sleeping. */
      actor = saga_runtime_deque_steal(&ex->inject_queue);
      if (actor) {
        pthread_mutex_unlock(&ex->work_avail_lock);
      } else {
        pthread_cond_wait(&ex->work_avail_cond, &ex->work_avail_lock);
        pthread_mutex_unlock(&ex->work_avail_lock);
        continue; /* loop back and try again */
      }
    }

    /* ── Execute the actor ──────────────────────────────────────────── */
    actor->last_cycle = saga_runtime_monotonic_now();
    actor->reduction_count = 0;
    saga_runtime_active_list_add(&ex->active, actor);

    /* Track the final status locally — only published under the lock
       after cleanup so that waiters never see a terminal status while
       the arena is still alive. */
    int64_t final_status;

    /* Set status to RUNNING *before* setjmp so the actor can read it. */
    __atomic_store_n(&actor->status, SAGA_RUNTIME_ACTOR_RUNNING, __ATOMIC_RELEASE);

    /* Publish the actor as the thread-local current actor so that
       intrinsics called from any depth can locate it. */
    saga_runtime_current_actor = actor;

    if (setjmp(actor->trap) == 0) {
      /* Normal path: run the actor's entry function. */
      actor->entry(actor);
      /* Default to COMPLETED.  context_exit may have set a different
         status — we capture whatever is current. */
      final_status = __atomic_load_n(&actor->status, __ATOMIC_ACQUIRE);
      if (final_status == SAGA_RUNTIME_ACTOR_RUNNING)
        final_status = SAGA_RUNTIME_ACTOR_COMPLETED;
    } else {
      /* longjmp path — actor was killed, trapped, or exited. */
      final_status = __atomic_load_n(&actor->status, __ATOMIC_ACQUIRE);
      if (final_status == SAGA_RUNTIME_ACTOR_RUNNING) {
        /* If result_in_arena is set, context_exit was called — treat as
           completed.  Otherwise default to killed. */
        final_status = actor->result_in_arena ? SAGA_RUNTIME_ACTOR_COMPLETED
                                              : SAGA_RUNTIME_ACTOR_KILLED;
      }
    }

    /* Clear the thread-local so stale reads from idle workers are a
       visible NULL deref rather than a use-after-free on the actor. */
    saga_runtime_current_actor = NULL;

    /* Copy result out of arena BEFORE destroying it. */
    if (actor->result_in_arena && actor->result_size > 0
        && actor->result == NULL) {
      actor->result = malloc((size_t)actor->result_size);
      if (actor->result)
        memcpy(actor->result, actor->result_in_arena,
               (size_t)actor->result_size);
    }

    /* Tear down the arena. */
    saga_runtime_arena_destroy(actor->arena);
    actor->arena = NULL;

    /* Remove from active list before signalling waiters. */
    saga_runtime_active_list_remove(&ex->active, actor);

    /* Close the channel so any parent iterating via recv sees EOF. */
    if (actor->channel)
      saga_channel_close(actor->channel);

    /* Publish the final status and signal waiters under the lock.
       This guarantees that anyone waking from saga_task_wait() sees
       arena == NULL and result fully populated. */
    pthread_mutex_lock(&actor->lock);
    actor->status = final_status;
    pthread_cond_signal(&actor->done_cond);
    pthread_mutex_unlock(&actor->lock);

    /* Drop the worker's reference. */
    saga_runtime_actor_release(actor);
  }

  return NULL;
}

/* ── Public executor API ───────────────────────────────────────────────── */

/*
 * saga_executor_init  —  create the global executor and spawn worker threads.
 *
 * `num_workers` = 0 means auto-detect (hardware concurrency).
 * Must be called once before any saga_executor_spawn().
 */
/* ── Heartbeat monitor ──────────────────────────────────────────────────── */
/*                                                                          */
/* A dedicated thread that periodically scans all active actors.  If an     */
/* actor's last_cycle timestamp is older than SAGA_RUNTIME_HEARTBEAT_TIMEOUT_MS, the  */
/* monitor sets its status to KILLED (status poisoning).  The actor will    */
/* exit on its next reduction tick or blocking channel operation.            */
/*                                                                          */
/* If the actor is STILL running after a second timeout period, the monitor */
/* assumes it is stuck in an FFI/syscall and logs a warning.  The worker    */
/* replacement mechanism (saga_runtime_executor_replace_worker) could be triggered    */
/* here in the future; for now we just poison and warn.                     */

static void *saga_runtime_heartbeat_monitor(void *arg) {
  saga_runtime_executor *ex = (saga_runtime_executor *)arg;
  int64_t timeout_ns = (int64_t)SAGA_RUNTIME_HEARTBEAT_TIMEOUT_MS * 1000000LL;
  int64_t check_interval_ms = SAGA_RUNTIME_HEARTBEAT_TIMEOUT_MS / 2;
  if (check_interval_ms < 100) check_interval_ms = 100;

  while (!__atomic_load_n(&ex->shutdown, __ATOMIC_ACQUIRE)) {
    /* Sleep for check_interval, but wake immediately on shutdown. */
    struct timespec abs_ts;
    clock_gettime(CLOCK_REALTIME, &abs_ts);
    abs_ts.tv_sec  += check_interval_ms / 1000;
    abs_ts.tv_nsec += (check_interval_ms % 1000) * 1000000L;
    if (abs_ts.tv_nsec >= 1000000000L) {
      abs_ts.tv_sec  += 1;
      abs_ts.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&ex->monitor_lock);
    pthread_cond_timedwait(&ex->monitor_cond, &ex->monitor_lock, &abs_ts);
    pthread_mutex_unlock(&ex->monitor_lock);

    if (__atomic_load_n(&ex->shutdown, __ATOMIC_ACQUIRE))
      break;

    int64_t now = saga_runtime_monotonic_now();

    pthread_mutex_lock(&ex->active.lock);
    for (int64_t i = 0; i < ex->active.len; i++) {
      saga_runtime_actor *a = ex->active.actors[i];
      int64_t st = __atomic_load_n(&a->status, __ATOMIC_ACQUIRE);
      if (st != SAGA_RUNTIME_ACTOR_RUNNING) continue;

      int64_t last = __atomic_load_n(&a->last_cycle, __ATOMIC_ACQUIRE);
      int64_t elapsed = now - last;

      if (elapsed > timeout_ns) {
        /* First timeout: poison the actor. */
        __atomic_store_n(&a->status, SAGA_RUNTIME_ACTOR_KILLED, __ATOMIC_RELEASE);
        /* Also close the channel to unblock any waiting send. */
        if (a->channel)
          saga_channel_close(a->channel);
      }
    }
    pthread_mutex_unlock(&ex->active.lock);
  }

  return NULL;
}

void saga_executor_init(int64_t num_workers) {
  if (g_executor) return; /* already initialised */

  if (num_workers <= 0) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    num_workers = (n > 0) ? (int64_t)n : 4;
  }

  saga_runtime_executor *ex = (saga_runtime_executor *)calloc(1, sizeof(saga_runtime_executor));
  ex->num_workers = num_workers;
  ex->shutdown    = 0;

  pthread_mutex_init(&ex->work_avail_lock, NULL);
  pthread_cond_init(&ex->work_avail_cond, NULL);

  saga_runtime_deque_init(&ex->inject_queue);

  ex->deques = (saga_runtime_deque *)calloc((size_t)num_workers, sizeof(saga_runtime_deque));
  for (int64_t i = 0; i < num_workers; i++)
    saga_runtime_deque_init(&ex->deques[i]);

  ex->worker_ctxs = (saga_runtime_worker_ctx *)calloc((size_t)num_workers,
                                             sizeof(saga_runtime_worker_ctx));
  ex->threads = (pthread_t *)calloc((size_t)num_workers, sizeof(pthread_t));

  saga_runtime_active_list_init(&ex->active);
  pthread_mutex_init(&ex->monitor_lock, NULL);
  pthread_cond_init(&ex->monitor_cond, NULL);

  g_executor = ex;

  for (int64_t i = 0; i < num_workers; i++) {
    ex->worker_ctxs[i].id = i;
    ex->worker_ctxs[i].executor = ex;
    pthread_create(&ex->threads[i], NULL, saga_runtime_worker_loop,
                   &ex->worker_ctxs[i]);
  }

  /* Start the heartbeat monitor. */
  pthread_create(&ex->monitor_thread, NULL, saga_runtime_heartbeat_monitor, ex);
}

/*
 * saga_executor_spawn  —  create a new actor and schedule it for execution.
 *
 * Returns a pointer to the actor (the parent's Task handle).  The caller
 * owns one reference; the worker holds the other.
 */
saga_runtime_actor *saga_executor_spawn(void (*entry)(saga_runtime_actor *), void *closure_data,
                            int64_t closure_size, int64_t arena_max) {
  if (!g_executor) return NULL;

  saga_runtime_actor *actor = saga_runtime_actor_new(entry, closure_data, closure_size, arena_max);
  if (!actor) return NULL;

  saga_runtime_deque_push(&g_executor->inject_queue, actor);

  /* Wake one idle worker. */
  pthread_mutex_lock(&g_executor->work_avail_lock);
  pthread_cond_signal(&g_executor->work_avail_cond);
  pthread_mutex_unlock(&g_executor->work_avail_lock);

  return actor;
}

/*
 * saga_executor_schedule  —  push an already-created actor into the executor.
 *
 * Use this when you need to configure the actor (e.g. attach a channel)
 * between creation and scheduling.  The actor must have been created with
 * saga_runtime_actor_new().
 */
void saga_executor_schedule(saga_runtime_actor *actor) {
  if (!g_executor || !actor) return;
  saga_runtime_deque_push(&g_executor->inject_queue, actor);
  pthread_mutex_lock(&g_executor->work_avail_lock);
  pthread_cond_signal(&g_executor->work_avail_cond);
  pthread_mutex_unlock(&g_executor->work_avail_lock);
}

/*
 * saga_executor_shutdown  —  signal all workers to stop and join them.
 */
void saga_executor_shutdown(void) {
  if (!g_executor) return;
  saga_runtime_executor *ex = g_executor;

  __atomic_store_n(&ex->shutdown, 1, __ATOMIC_RELEASE);

  /* Wake all workers so they see the shutdown flag. */
  pthread_mutex_lock(&ex->work_avail_lock);
  pthread_cond_broadcast(&ex->work_avail_cond);
  pthread_mutex_unlock(&ex->work_avail_lock);

  for (int64_t i = 0; i < ex->num_workers; i++)
    pthread_join(ex->threads[i], NULL);

  /* Wake and join the heartbeat monitor. */
  pthread_mutex_lock(&ex->monitor_lock);
  pthread_cond_signal(&ex->monitor_cond);
  pthread_mutex_unlock(&ex->monitor_lock);
  pthread_join(ex->monitor_thread, NULL);

  /* Clean up. */
  for (int64_t i = 0; i < ex->num_workers; i++)
    saga_runtime_deque_destroy(&ex->deques[i]);
  saga_runtime_deque_destroy(&ex->inject_queue);
  saga_runtime_active_list_destroy(&ex->active);

  pthread_mutex_destroy(&ex->monitor_lock);
  pthread_cond_destroy(&ex->monitor_cond);
  pthread_mutex_destroy(&ex->work_avail_lock);
  pthread_cond_destroy(&ex->work_avail_cond);

  free(ex->threads);
  free(ex->worker_ctxs);
  free(ex->deques);
  free(ex);
  g_executor = NULL;
}

/*
 * saga_runtime_executor_replace_worker  —  abandon a stuck worker and spawn a
 * replacement.  The old thread is detached (leaked); its deque is drained
 * into the inject queue first, and a fresh deque is allocated for the
 * replacement so there is no contention.
 */
void saga_runtime_executor_replace_worker(int64_t worker_id) {
  if (!g_executor) return;
  saga_runtime_executor *ex = g_executor;
  if (worker_id < 0 || worker_id >= ex->num_workers) return;

  /* Drain stranded actors into the global inject queue. */
  saga_runtime_deque_drain(&ex->deques[worker_id], &ex->inject_queue);

  /* Destroy the old deque and create a fresh one. */
  saga_runtime_deque_destroy(&ex->deques[worker_id]);
  saga_runtime_deque_init(&ex->deques[worker_id]);

  /* Detach the stuck thread — it keeps running but we no longer join it. */
  pthread_detach(ex->threads[worker_id]);

  /* Spawn a replacement. */
  ex->worker_ctxs[worker_id].id = worker_id;
  ex->worker_ctxs[worker_id].executor = ex;
  pthread_create(&ex->threads[worker_id], NULL, saga_runtime_worker_loop,
                 &ex->worker_ctxs[worker_id]);

  /* Wake the new worker in case there's work. */
  pthread_mutex_lock(&ex->work_avail_lock);
  pthread_cond_signal(&ex->work_avail_cond);
  pthread_mutex_unlock(&ex->work_avail_lock);
}

/* ───────────────────────────────────────────────────────────────────────── */
/* Task API                                                                 */
/*                                                                          */
/* Called from the parent / spawning thread.  These operate on the          */
/* saga_runtime_actor* that saga_executor_spawn() returned (the "Task handle").         */
/* ───────────────────────────────────────────────────────────────────────── */

/*
 * saga_task_alive  —  returns 1 if the actor is still running or pending.
 */
int64_t saga_task_alive(saga_runtime_actor *a) {
  if (!a) return 0;
  pthread_mutex_lock(&a->lock);
  int alive = (a->status == SAGA_RUNTIME_ACTOR_PENDING ||
               a->status == SAGA_RUNTIME_ACTOR_RUNNING);
  pthread_mutex_unlock(&a->lock);
  return alive ? 1 : 0;
}

/*
 * saga_task_cancel  —  ask the actor to stop.  The actor must poll
 *                    saga_context_cancelled() to honour this.
 */
void saga_task_cancel(saga_runtime_actor *a) {
  if (!a) return;
  __atomic_store_n(&a->cancelled, 1, __ATOMIC_RELEASE);
}

/*
 * saga_task_term  —  immediately kill the actor.  If it is blocked on a
 *                  channel the channel is closed to unblock it; on its
 *                  next reduction tick or blocking call it will longjmp
 *                  out (status poisoning).
 */
void saga_task_term(saga_runtime_actor *a) {
  if (!a) return;
  pthread_mutex_lock(&a->lock);
  if (a->status == SAGA_RUNTIME_ACTOR_PENDING || a->status == SAGA_RUNTIME_ACTOR_RUNNING) {
    a->status = SAGA_RUNTIME_ACTOR_KILLED;
    if (a->channel)
      saga_channel_close(a->channel);
  }
  /* If the actor already finished, term is a no-op. */
  pthread_cond_signal(&a->done_cond);
  pthread_mutex_unlock(&a->lock);
}

/*
 * saga_task_wait  —  block until the actor reaches a terminal status.
 *                  Returns a pointer to the result (malloc'd, outlives
 *                  the arena) or NULL if no result was set.
 *
 *                  `out_status` is set to the terminal status if non-NULL.
 */
void *saga_task_wait(saga_runtime_actor *a, int64_t *out_status) {
  if (!a) {
    if (out_status) *out_status = SAGA_RUNTIME_ACTOR_KILLED;
    return NULL;
  }
  pthread_mutex_lock(&a->lock);
  while (a->status < SAGA_RUNTIME_ACTOR_COMPLETED)
    pthread_cond_wait(&a->done_cond, &a->lock);
  int64_t st = a->status;
  pthread_mutex_unlock(&a->lock);

  if (out_status) *out_status = st;
  return a->result;
}

/*
 * saga_task_drop  —  release the parent's reference.  Called when the Task
 *                  handle goes out of scope in generated code.
 */
void saga_task_drop(saga_runtime_actor *a) {
  saga_runtime_actor_release(a);
}

/* ───────────────────────────────────────────────────────────────────────── */
/* Context API                                                              */
/*                                                                          */
/* Called from inside the spawned actor.  The actor receives a pointer to   */
/* its own saga_runtime_actor as the "context".                                       */
/* ───────────────────────────────────────────────────────────────────────── */

/*
 * saga_context_cancelled  —  returns 1 if the parent called saga_task_cancel.
 */
int64_t saga_context_cancelled(saga_runtime_actor *a) {
  if (!a) return 0;
  return __atomic_load_n(&a->cancelled, __ATOMIC_ACQUIRE) ? 1 : 0;
}

/*
 * saga_context_exit  —  terminate the actor with a return value.
 *
 * `value` must point to data inside the actor's arena (or stack).
 * The worker loop copies it to a malloc'd buffer before destroying
 * the arena.
 *
 * Does not return — calls longjmp.
 */
void saga_context_exit(saga_runtime_actor *a, void *value, int64_t size) {
  if (!a) return;
  /* Copy the bytes onto the heap BEFORE longjmp.  `value` typically points
     into the actor's stack frame, which is abandoned after longjmp; the
     worker loop runs intervening code (malloc, etc.) that can clobber it
     before a later memcpy would have read it.  Copying here keeps the
     bytes live.  We also set result_in_arena as a signal to the worker
     loop that context_exit was called (so it promotes final_status to
     COMPLETED on the longjmp path). */
  if (value && size > 0) {
    void *copy = malloc((size_t)size);
    if (copy) {
      memcpy(copy, value, (size_t)size);
      a->result = copy;
    }
  }
  a->result_in_arena = value;
  a->result_size     = size;
  longjmp(a->trap, 1);
}

/*
 * saga_context_send  —  push a value into the actor's channel.
 *
 * Non-blocking from the actor's perspective unless the channel buffer is
 * full, in which case it blocks until a consumer drains an element.
 * Resets the reduction counter (I/O counts as yielding).
 */
int saga_context_send(saga_runtime_actor *a, const void *data) {
  if (!a || !a->channel) return -1;
  return saga_runtime_channel_send(a->channel, data, a);
}

/*
 * saga_actor_yield  —  voluntarily yield execution and reset the reduction
 *                    counter.  Called by the yield intrinsic.
 */
#include <sched.h>

void saga_actor_yield(void) {
  saga_runtime_actor *a = saga_runtime_current_actor;
  if (!a) return;
  sched_yield();
  a->reduction_count = 0;
  a->last_cycle = saga_runtime_monotonic_now();
}

/* ───────────────────────────────────────────────────────────────────────── */
/* Channel                                                                  */
/*                                                                          */
/* Fixed-size ring buffer for transferring data between actors.  Data is    */
/* copied (ownership transfer) — the sender retains no reference.           */
/*                                                                          */
/* saga_runtime_channel_send blocks if the buffer is full.                            */
/* saga_channel_recv blocks if the buffer is empty.                           */
/* saga_channel_close wakes all waiters; subsequent recv returns -1 once the  */
/* buffer drains.                                                           */
/* ───────────────────────────────────────────────────────────────────────── */

#ifndef SAGA_RUNTIME_CHANNEL_DEFAULT_CAP
#define SAGA_RUNTIME_CHANNEL_DEFAULT_CAP 64
#endif

struct saga_runtime_channel {
  char           *buffer;      /* ring buffer: capacity * elem_size bytes  */
  int64_t         elem_size;
  int64_t         capacity;    /* fixed at creation                        */
  int64_t         head;        /* read position                            */
  int64_t         tail;        /* write position                           */
  int64_t         count;       /* items currently in the buffer            */
  int             closed;
  pthread_mutex_t lock;
  pthread_cond_t  not_empty;   /* recv waits here                         */
  pthread_cond_t  not_full;    /* send waits here                         */
};

/*
 * saga_channel_new  —  create a channel with a fixed ring buffer.
 *
 * `elem_size` is the byte size of each element.
 * `capacity`  is the max number of elements; 0 → SAGA_RUNTIME_CHANNEL_DEFAULT_CAP.
 */
saga_runtime_channel *saga_channel_new(int64_t elem_size, int64_t capacity) {
  if (elem_size <= 0) return NULL;
  if (capacity <= 0) capacity = SAGA_RUNTIME_CHANNEL_DEFAULT_CAP;

  saga_runtime_channel *ch = (saga_runtime_channel *)calloc(1, sizeof(saga_runtime_channel));
  if (!ch) return NULL;

  ch->buffer    = (char *)calloc((size_t)capacity, (size_t)elem_size);
  if (!ch->buffer) { free(ch); return NULL; }

  ch->elem_size = elem_size;
  ch->capacity  = capacity;
  ch->head      = 0;
  ch->tail      = 0;
  ch->count     = 0;
  ch->closed    = 0;

  pthread_mutex_init(&ch->lock, NULL);
  pthread_cond_init(&ch->not_empty, NULL);
  pthread_cond_init(&ch->not_full, NULL);
  return ch;
}

/*
 * saga_runtime_channel_send  —  copy `data` into the channel (blocking if full).
 *
 * `actor` may be NULL (for sends from the main thread).  When non-NULL the
 * function checks status poisoning so a killed actor unblocks immediately.
 *
 * Returns  0 on success, -1 if the channel is closed.
 */
int saga_runtime_channel_send(saga_runtime_channel *ch, const void *data, saga_runtime_actor *actor) {
  if (!ch || !data) return -1;

  pthread_mutex_lock(&ch->lock);

  /* Block while full — respect close and actor kill. */
  while (ch->count == ch->capacity && !ch->closed) {
    if (actor && __atomic_load_n(&actor->status, __ATOMIC_ACQUIRE)
                    == SAGA_RUNTIME_ACTOR_KILLED) {
      pthread_mutex_unlock(&ch->lock);
      longjmp(actor->trap, 1);
    }
    pthread_cond_wait(&ch->not_full, &ch->lock);
  }

  if (ch->closed) {
    pthread_mutex_unlock(&ch->lock);
    return -1;
  }

  /* Copy element into the ring buffer. */
  memcpy(ch->buffer + (ch->tail * ch->elem_size), data,
         (size_t)ch->elem_size);
  ch->tail = (ch->tail + 1) % ch->capacity;
  ch->count++;

  /* Reset the actor's reduction counter (I/O counts as yielding). */
  if (actor)
    actor->reduction_count = 0;

  pthread_cond_signal(&ch->not_empty);
  pthread_mutex_unlock(&ch->lock);
  return 0;
}

/*
 * saga_channel_recv  —  read the next element into `out_buf` (blocking).
 *
 * Returns  0 on success.
 * Returns -1 if the channel is closed AND the buffer is drained (EOF).
 */
int saga_channel_recv(saga_runtime_channel *ch, void *out_buf) {
  if (!ch || !out_buf) return -1;

  pthread_mutex_lock(&ch->lock);

  /* Block while empty — break out when data arrives or channel closes. */
  while (ch->count == 0 && !ch->closed)
    pthread_cond_wait(&ch->not_empty, &ch->lock);

  /* Closed with nothing left → EOF. */
  if (ch->count == 0 && ch->closed) {
    pthread_mutex_unlock(&ch->lock);
    return -1;
  }

  /* Copy element out of the ring buffer. */
  memcpy(out_buf, ch->buffer + (ch->head * ch->elem_size),
         (size_t)ch->elem_size);
  ch->head = (ch->head + 1) % ch->capacity;
  ch->count--;

  pthread_cond_signal(&ch->not_full);
  pthread_mutex_unlock(&ch->lock);
  return 0;
}

/*
 * saga_channel_close  —  mark the channel closed, wake all waiters.
 *
 * Safe to call multiple times.  After close, send returns -1.  recv
 * continues to drain buffered data, then returns -1.
 */
void saga_channel_close(saga_runtime_channel *ch) {
  if (!ch) return;

  pthread_mutex_lock(&ch->lock);
  ch->closed = 1;
  pthread_cond_broadcast(&ch->not_empty);
  pthread_cond_broadcast(&ch->not_full);
  pthread_mutex_unlock(&ch->lock);
}

/*
 * saga_channel_destroy  —  free all channel resources.
 *
 * The channel must already be closed and no threads may be blocked on it.
 */
void saga_channel_destroy(saga_runtime_channel *ch) {
  if (!ch) return;
  free(ch->buffer);
  pthread_mutex_destroy(&ch->lock);
  pthread_cond_destroy(&ch->not_empty);
  pthread_cond_destroy(&ch->not_full);
  free(ch);
}

/* ───────────────────────────────────────────────────────────────────────── */
/* String representation                                                    */
/*                                                                          */
/* Layout: { const char *data, int64_t len, int64_t refcount }              */
/*   refcount == -1  →  static / global constant, never freed               */
/*   refcount >=  1  →  heap-allocated, freed when it reaches 0             */
/* ───────────────────────────────────────────────────────────────────────── */

typedef struct {
  const char *data;
  int64_t len;
  int64_t refcount;
} saga_runtime_string;

/* ───────────────────────────────────────────────────────────────────────── */
/* saga_actor_trap  —  transition actor to ZOMBIE state                       */
/*                                                                          */
/* The actor's arena is NOT destroyed so a supervisor can inspect it.        */
/* Called by the intrinsic_trap() Saga intrinsic from inside a spawn block.  */
/* ───────────────────────────────────────────────────────────────────────── */

void saga_actor_trap(saga_runtime_string *reason) {
  saga_runtime_actor *a = saga_runtime_current_actor;
  if (!a) return;

  __atomic_store_n(&a->status, SAGA_RUNTIME_ACTOR_ZOMBIE, __ATOMIC_RELEASE);

  /* Store a copy of the reason string in heap memory (outlives arena). */
  if (reason) {
    saga_runtime_string *copy = (saga_runtime_string *)malloc(sizeof(saga_runtime_string) + reason->len + 1);
    if (copy) {
      char *buf = (char *)copy + sizeof(saga_runtime_string);
      memcpy(buf, reason->data, reason->len);
      buf[reason->len] = '\0';
      copy->data = buf;
      copy->len = reason->len;
      copy->refcount = 1;
      a->result = copy;
      a->result_size = sizeof(saga_runtime_string);
    }
  }

  longjmp(a->trap, 1);
}

/* ───────────────────────────────────────────────────────────────────────── */
/* TrapError — concrete type returned from Task.Wait()'s Error branch.      */
/*                                                                          */
/* Layout mirrors the compiler's iface_fat_ptr layout { data*, vtable* }.   */
/* The vtable has one slot (Message) matching the Error interface.          */
/* ───────────────────────────────────────────────────────────────────────── */

typedef struct {
  saga_runtime_string *reason;
} saga_runtime_trap_error;

typedef struct {
  void *message_fn;
} saga_runtime_trap_error_vtable;

typedef struct {
  void *data;
  void *vtable;
} saga_runtime_iface_fat_ptr;

/* Forward declaration for the vtable initializer. */
static saga_runtime_string *saga_trap_error_message(void *self);

static const saga_runtime_trap_error_vtable saga_trap_error_vtable_instance = {
    .message_fn = (void *)saga_trap_error_message,
};

static saga_runtime_string *saga_trap_error_message(void *self) {
  saga_runtime_trap_error *e = (saga_runtime_trap_error *)self;
  if (e && e->reason) {
    /* Hand out a retained reference — the caller owns it. */
    if (e->reason->refcount > 0) e->reason->refcount++;
    return e->reason;
  }
  /* Fall back to a fresh "killed" string. */
  static const char kFallback[] = "killed";
  char *heap = (char *)malloc(sizeof(kFallback));
  memcpy(heap, kFallback, sizeof(kFallback));
  saga_runtime_string *s = (saga_runtime_string *)malloc(sizeof(saga_runtime_string));
  s->data = heap;
  s->len = sizeof(kFallback) - 1;
  s->refcount = 1;
  return s;
}

/* ───────────────────────────────────────────────────────────────────────── */
/* Missing — concrete error returned by index/map lookups and the failure   */
/* path of intrinsic_runtime_try.                                           */
/*                                                                          */
/* Uses the same saga_runtime_iface_fat_ptr shape as TrapError, with a      */
/* one-slot vtable (Message) so callers can dispatch through the Error      */
/* interface uniformly.                                                     */
/* ───────────────────────────────────────────────────────────────────────── */

/* saga_runtime_alloc_string is defined further below; forward-declare it
 * here so the Missing constructor can reach it. */
static saga_runtime_string *saga_runtime_alloc_string(const char *buf,
                                                      int64_t len);

typedef struct {
  saga_runtime_string *message;
} saga_runtime_missing;

static saga_runtime_string *saga_missing_message(void *self) {
  saga_runtime_missing *m = (saga_runtime_missing *)self;
  if (m && m->message) {
    if (m->message->refcount > 0) m->message->refcount++;
    return m->message;
  }
  return saga_runtime_alloc_string("", 0);
}

static const saga_runtime_trap_error_vtable saga_missing_vtable_instance = {
    .message_fn = (void *)saga_missing_message,
};

void *saga_missing_new(const char *msg, int64_t len) {
  saga_runtime_missing *m =
      (saga_runtime_missing *)malloc(sizeof(saga_runtime_missing));
  if (!m) return NULL;
  m->message = saga_runtime_alloc_string(msg ? msg : "", msg ? len : 0);

  saga_runtime_iface_fat_ptr *fat =
      (saga_runtime_iface_fat_ptr *)malloc(sizeof(saga_runtime_iface_fat_ptr));
  if (!fat) { free(m); return NULL; }
  fat->data = m;
  fat->vtable = (void *)&saga_missing_vtable_instance;
  return fat;
}

/*
 * saga_error_from_trap — build an Error interface fat pointer from the
 * trapped actor's stashed reason string.  Returns a heap-allocated
 * saga_runtime_iface_fat_ptr whose data points to a saga_runtime_trap_error and whose vtable
 * points to saga_trap_error_vtable_instance.
 *
 * Called by the Task.Wait() lowering on the error path.
 */
void *saga_error_from_trap(saga_runtime_actor *a) {
  saga_runtime_trap_error *e = (saga_runtime_trap_error *)malloc(sizeof(saga_runtime_trap_error));
  if (!e) return NULL;
  saga_runtime_string *reason = (a && a->result) ? (saga_runtime_string *)a->result : NULL;
  if (reason && reason->refcount > 0) reason->refcount++;
  e->reason = reason;

  saga_runtime_iface_fat_ptr *fat = (saga_runtime_iface_fat_ptr *)malloc(sizeof(saga_runtime_iface_fat_ptr));
  if (!fat) { free(e); return NULL; }
  fat->data = e;
  fat->vtable = (void *)&saga_trap_error_vtable_instance;
  return fat;
}

/* ───────────────────────────────────────────────────────────────────────── */
/* String refcounting                                                       */
/* ───────────────────────────────────────────────────────────────────────── */

void saga_retain_string(saga_runtime_string *s) {
  if (s && s->refcount > 0)
    s->refcount++;
}

void saga_release_string(saga_runtime_string *s) {
  if (!s || s->refcount < 0) return;   /* static — never free */
  s->refcount--;
  if (s->refcount <= 0) {
    free((void *)s->data);
    free(s);
  }
}

static saga_runtime_string *saga_runtime_alloc_string(const char *buf, int64_t len) {
  char *heap = (char *)malloc((size_t)len);
  if (len > 0) memcpy(heap, buf, (size_t)len);
  saga_runtime_string *s = (saga_runtime_string *)malloc(sizeof(saga_runtime_string));
  s->data = heap;
  s->len = len;
  s->refcount = 1;
  return s;
}

/* ───────────────────────────────────────────────────────────────────────── */
/* String access (indexing and slicing)                                     */
/*                                                                          */
/* Indexing and slicing are byte-indexed.  This matches the spec's          */
/* one-character examples ("hello"[1] => "e") for ASCII; the runes/bytes    */
/* split (saga_string_runes / saga_string_bytes) is the path that exposes   */
/* codepoint-aware iteration when needed.                                   */
/* ───────────────────────────────────────────────────────────────────────── */

saga_runtime_string *saga_string_at(const saga_runtime_string *s,
                                    int64_t index) {
  if (!s || index < 0 || index >= s->len)
    return saga_runtime_alloc_string("", 0);
  return saga_runtime_alloc_string(s->data + index, 1);
}

/* Sentinel high-bound: callers pass INT64_MIN when the slice's high end is */
/* omitted (str[a..]).  Likewise INT64_MIN signals an omitted low (str[..b]) */
/* — chosen because no valid index can collide with it.  Out-of-range       */
/* values are clamped to [0, len], which mirrors how Go and Rust treat      */
/* slice bounds: `str[..]` == `str[0..len]`.                                */
saga_runtime_string *saga_string_slice(const saga_runtime_string *s,
                                       int64_t low, int64_t high) {
  if (!s) return saga_runtime_alloc_string("", 0);
  if (low == INT64_MIN || low < 0) low = 0;
  if (low > s->len) low = s->len;
  if (high == INT64_MIN || high > s->len) high = s->len;
  if (high < low) high = low;
  return saga_runtime_alloc_string(s->data + low, high - low);
}

/* ───────────────────────────────────────────────────────────────────────── */
/* String helpers                                                           */
/* ───────────────────────────────────────────────────────────────────────── */

saga_runtime_string *saga_string_concat(const saga_runtime_string *a, const saga_runtime_string *b) {
  int64_t new_len = 0;
  if (a) new_len += a->len;
  if (b) new_len += b->len;

  char *buf = (char *)malloc((size_t)new_len);
  int64_t off = 0;
  if (a && a->data && a->len > 0) {
    memcpy(buf + off, a->data, (size_t)a->len);
    off += a->len;
  }
  if (b && b->data && b->len > 0) {
    memcpy(buf + off, b->data, (size_t)b->len);
  }

  saga_runtime_string *result = (saga_runtime_string *)malloc(sizeof(saga_runtime_string));
  result->data = buf;
  result->len = new_len;
  result->refcount = 1;
  return result;
}

int64_t saga_string_compare(const saga_runtime_string *a, const saga_runtime_string *b) {
  if (!a || !a->data) return (b && b->data && b->len > 0) ? -1 : 0;
  if (!b || !b->data) return (a->len > 0) ? 1 : 0;

  int64_t min_len = a->len < b->len ? a->len : b->len;
  int cmp = memcmp(a->data, b->data, (size_t)min_len);
  if (cmp != 0) return cmp;
  if (a->len < b->len) return -1;
  if (a->len > b->len) return 1;
  return 0;
}

/* ───────────────────────────────────────────────────────────────────────── */
/* Type-to-string conversions                                               */
/* ───────────────────────────────────────────────────────────────────────── */

saga_runtime_string *saga_int_to_string(int64_t val) {
  char buf[32];
  int n = snprintf(buf, sizeof(buf), "%" PRId64, val);
  return saga_runtime_alloc_string(buf, n);
}

saga_runtime_string *saga_float_to_string(double val) {
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "%g", val);
  return saga_runtime_alloc_string(buf, n);
}

saga_runtime_string *saga_bool_to_string(int64_t val) {
  if (val) return saga_runtime_alloc_string("true", 4);
  return saga_runtime_alloc_string("false", 5);
}

saga_runtime_string *saga_string_lower(const saga_runtime_string *s) {
  if (!s || s->len == 0) return saga_runtime_alloc_string("", 0);
  char *buf = (char *)malloc((size_t)s->len);
  if (!buf) return saga_runtime_alloc_string("", 0);
  for (int64_t i = 0; i < s->len; i++) {
    unsigned char c = (unsigned char)s->data[i];
    buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
  }
  saga_runtime_string *result = saga_runtime_alloc_string(buf, s->len);
  free(buf);
  return result;
}

saga_runtime_string *saga_string_upper(const saga_runtime_string *s) {
  if (!s || s->len == 0) return saga_runtime_alloc_string("", 0);
  char *buf = (char *)malloc((size_t)s->len);
  if (!buf) return saga_runtime_alloc_string("", 0);
  for (int64_t i = 0; i < s->len; i++) {
    unsigned char c = (unsigned char)s->data[i];
    buf[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : (char)c;
  }
  saga_runtime_string *result = saga_runtime_alloc_string(buf, s->len);
  free(buf);
  return result;
}

/* ───────────────────────────────────────────────────────────────────────── */
/* String: Bytes, Count, Runes                                              */
/* ───────────────────────────────────────────────────────────────────────── */

/* saga_runtime_array typedef needed here for forward declarations below. */
typedef struct {
  void *data;
  int64_t len;
  int64_t cap;
  int64_t elem_size;
  int64_t refcount;
} saga_runtime_array;

/* Forward declarations for array ops (defined below). */
static saga_runtime_array *saga_array_new_internal(int64_t elem_size, int64_t initial_cap);
static void saga_array_push_internal(saga_runtime_array *arr, const void *elem);

saga_runtime_array *saga_string_bytes(const saga_runtime_string *s) {
  int64_t len = (s && s->data) ? s->len : 0;
  saga_runtime_array *arr = saga_array_new_internal(1, len > 4 ? len : 4);
  for (int64_t i = 0; i < len; i++) {
    uint8_t b = (uint8_t)s->data[i];
    saga_array_push_internal(arr, &b);
  }
  return arr;
}

int64_t saga_string_count(const saga_runtime_string *s) {
  if (!s || !s->data || s->len == 0) return 0;
  int64_t count = 0;
  for (int64_t i = 0; i < s->len; ) {
    unsigned char c = (unsigned char)s->data[i];
    if      (c < 0x80) i += 1;
    else if (c < 0xE0) i += 2;
    else if (c < 0xF0) i += 3;
    else               i += 4;
    count++;
  }
  return count;
}

saga_runtime_array *saga_string_runes(const saga_runtime_string *s) {
  int64_t len = (s && s->data) ? s->len : 0;
  saga_runtime_array *arr = saga_array_new_internal(4, len > 4 ? len : 4);
  for (int64_t i = 0; i < len; ) {
    unsigned char c = (unsigned char)s->data[i];
    int32_t cp = 0;
    if (c < 0x80) {
      cp = c; i += 1;
    } else if (c < 0xE0 && i + 1 < len) {
      cp = ((c & 0x1F) << 6) | (s->data[i+1] & 0x3F);
      i += 2;
    } else if (c < 0xF0 && i + 2 < len) {
      cp = ((c & 0x0F) << 12) | ((s->data[i+1] & 0x3F) << 6)
           | (s->data[i+2] & 0x3F);
      i += 3;
    } else if (i + 3 < len) {
      cp = ((c & 0x07) << 18) | ((s->data[i+1] & 0x3F) << 12)
           | ((s->data[i+2] & 0x3F) << 6) | (s->data[i+3] & 0x3F);
      i += 4;
    } else {
      cp = 0xFFFD; /* replacement character */
      i++;
    }
    saga_array_push_internal(arr, &cp);
  }
  return arr;
}

/* ───────────────────────────────────────────────────────────────────────── */
/* String: predicates and substring search                                  */
/* ───────────────────────────────────────────────────────────────────────── */

int64_t saga_string_has_prefix(const saga_runtime_string *s,
                               const saga_runtime_string *prefix) {
  if (!prefix || prefix->len == 0) return 1;
  if (!s || s->len < prefix->len) return 0;
  return memcmp(s->data, prefix->data, (size_t)prefix->len) == 0 ? 1 : 0;
}

int64_t saga_string_has_suffix(const saga_runtime_string *s,
                               const saga_runtime_string *suffix) {
  if (!suffix || suffix->len == 0) return 1;
  if (!s || s->len < suffix->len) return 0;
  return memcmp(s->data + (s->len - suffix->len), suffix->data,
                (size_t)suffix->len) == 0 ? 1 : 0;
}

/* Naive O(n*m) substring search.  Returns the byte offset of the first       */
/* occurrence, or -1 if absent.                                               */
static int64_t saga_runtime_string_find(const saga_runtime_string *s,
                                        const saga_runtime_string *needle) {
  if (!needle || needle->len == 0) return 0;
  if (!s || s->len < needle->len) return -1;
  int64_t last = s->len - needle->len;
  for (int64_t i = 0; i <= last; i++) {
    if (memcmp(s->data + i, needle->data, (size_t)needle->len) == 0)
      return i;
  }
  return -1;
}

int64_t saga_string_contains(const saga_runtime_string *s,
                             const saga_runtime_string *needle) {
  return saga_runtime_string_find(s, needle) >= 0 ? 1 : 0;
}

/* ───────────────────────────────────────────────────────────────────────── */
/* String: trimming and casing                                              */
/* ───────────────────────────────────────────────────────────────────────── */

static int saga_runtime_is_ascii_space(unsigned char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' ||
         c == '\f';
}

saga_runtime_string *saga_string_trim(const saga_runtime_string *s) {
  if (!s || s->len == 0) return saga_runtime_alloc_string("", 0);
  int64_t start = 0;
  int64_t end = s->len;
  while (start < end && saga_runtime_is_ascii_space((unsigned char)s->data[start]))
    start++;
  while (end > start && saga_runtime_is_ascii_space((unsigned char)s->data[end - 1]))
    end--;
  return saga_runtime_alloc_string(s->data + start, end - start);
}

saga_runtime_string *saga_string_capitalize(const saga_runtime_string *s) {
  if (!s || s->len == 0) return saga_runtime_alloc_string("", 0);
  char *buf = (char *)malloc((size_t)s->len);
  unsigned char c0 = (unsigned char)s->data[0];
  buf[0] = (c0 >= 'a' && c0 <= 'z') ? (char)(c0 - 32) : (char)c0;
  for (int64_t i = 1; i < s->len; i++) {
    unsigned char c = (unsigned char)s->data[i];
    buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
  }
  saga_runtime_string *result = saga_runtime_alloc_string(buf, s->len);
  free(buf);
  return result;
}

saga_runtime_string *saga_string_title(const saga_runtime_string *s) {
  if (!s || s->len == 0) return saga_runtime_alloc_string("", 0);
  char *buf = (char *)malloc((size_t)s->len);
  int at_word_start = 1;
  for (int64_t i = 0; i < s->len; i++) {
    unsigned char c = (unsigned char)s->data[i];
    if (saga_runtime_is_ascii_space(c)) {
      buf[i] = (char)c;
      at_word_start = 1;
    } else if (at_word_start) {
      buf[i] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : (char)c;
      at_word_start = 0;
    } else {
      buf[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
    }
  }
  saga_runtime_string *result = saga_runtime_alloc_string(buf, s->len);
  free(buf);
  return result;
}

/* ───────────────────────────────────────────────────────────────────────── */
/* String: split                                                            */
/* ───────────────────────────────────────────────────────────────────────── */

/* Split returns an array of saga_runtime_string* fragments.  An empty       */
/* separator returns a single-element array containing the input string.     */
saga_runtime_array *saga_string_split(const saga_runtime_string *s,
                                      const saga_runtime_string *sep) {
  saga_runtime_array *arr = saga_array_new_internal(
      (int64_t)sizeof(saga_runtime_string *), 4);
  if (!s || s->len == 0) {
    saga_runtime_string *empty = saga_runtime_alloc_string("", 0);
    saga_array_push_internal(arr, &empty);
    return arr;
  }
  if (!sep || sep->len == 0) {
    saga_runtime_string *copy = saga_runtime_alloc_string(s->data, s->len);
    saga_array_push_internal(arr, &copy);
    return arr;
  }
  /* Back-to-back separators produce empty fragments (matches Go's          */
  /* strings.Split).                                                         */
  int64_t start = 0;
  int64_t i = 0;
  while (i <= s->len - sep->len) {
    if (memcmp(s->data + i, sep->data, (size_t)sep->len) == 0) {
      saga_runtime_string *frag =
          saga_runtime_alloc_string(s->data + start, i - start);
      saga_array_push_internal(arr, &frag);
      i += sep->len;
      start = i;
    } else {
      i++;
    }
  }
  saga_runtime_string *tail =
      saga_runtime_alloc_string(s->data + start, s->len - start);
  saga_array_push_internal(arr, &tail);
  return arr;
}

/* ───────────────────────────────────────────────────────────────────────── */
/* String: parsing (try-style: returns 0 on success, non-zero on failure)   */
/* ───────────────────────────────────────────────────────────────────────── */

int64_t saga_string_to_int(const saga_runtime_string *s, int64_t *out) {
  if (!s || !s->data || s->len == 0) return 1;
  /* Copy to null-terminated buffer for strtoll. */
  char buf[64];
  int64_t copy_len = s->len < 63 ? s->len : 63;
  memcpy(buf, s->data, (size_t)copy_len);
  buf[copy_len] = '\0';
  char *end = NULL;
  errno = 0;
  long long v = strtoll(buf, &end, 10);
  if (errno != 0 || end == buf || *end != '\0') return 1;
  *out = (int64_t)v;
  return 0;
}

int64_t saga_string_to_float(const saga_runtime_string *s, double *out) {
  if (!s || !s->data || s->len == 0) return 1;
  char buf[256];
  int64_t copy_len = s->len < 255 ? s->len : 255;
  memcpy(buf, s->data, (size_t)copy_len);
  buf[copy_len] = '\0';
  char *end = NULL;
  errno = 0;
  double v = strtod(buf, &end);
  if (errno != 0 || end == buf || *end != '\0') return 1;
  *out = v;
  return 0;
}

/* ───────────────────────────────────────────────────────────────────────── */
/* Formatting helpers                                                       */
/*                                                                          */
/* Format specifier: [sign][fill][width][.precision][e]                     */
/*   sign:  '+'  — always show sign                                        */
/*   fill:  '0' or ' '  — padding character (default space)                */
/*   width: integer  — minimum width                                        */
/*   .precision: integer  — decimal places (floats) or ignored (ints)       */
/*   e:  — scientific notation (floats only)                                */
/* ───────────────────────────────────────────────────────────────────────── */

typedef struct {
  int show_sign;   /* 1 if '+' present */
  char fill;       /* '0' or ' ' */
  int width;       /* minimum width, 0 if unset */
  int precision;   /* -1 if unset */
  int scientific;  /* 1 if 'e' present */
} fmt_spec;

static fmt_spec parse_fmt(const saga_runtime_string *fmt) {
  fmt_spec f = {0, ' ', 0, -1, 0};
  if (!fmt || !fmt->data || fmt->len == 0) return f;
  const char *p = fmt->data;
  const char *end = p + fmt->len;

  if (p < end && *p == '+') { f.show_sign = 1; p++; }
  if (p < end && (*p == '0' || *p == ' ')) { f.fill = *p; p++; }
  /* width */
  while (p < end && *p >= '0' && *p <= '9') {
    f.width = f.width * 10 + (*p - '0');
    p++;
  }
  /* .precision */
  if (p < end && *p == '.') {
    p++;
    f.precision = 0;
    while (p < end && *p >= '0' && *p <= '9') {
      f.precision = f.precision * 10 + (*p - '0');
      p++;
    }
  }
  if (p < end && (*p == 'e' || *p == 'E')) { f.scientific = 1; }
  return f;
}

static saga_runtime_string *apply_padding(const char *raw, int raw_len, fmt_spec *f) {
  if (raw_len >= f->width) return saga_runtime_alloc_string(raw, raw_len);
  int pad = f->width - raw_len;
  char *buf = (char *)malloc((size_t)f->width);
  if (f->fill == '0' && (raw[0] == '+' || raw[0] == '-')) {
    /* sign before zero-fill: +000042 */
    buf[0] = raw[0];
    memset(buf + 1, '0', (size_t)pad);
    memcpy(buf + 1 + pad, raw + 1, (size_t)(raw_len - 1));
  } else {
    memset(buf, f->fill, (size_t)pad);
    memcpy(buf + pad, raw, (size_t)raw_len);
  }
  saga_runtime_string *result = saga_runtime_alloc_string(buf, f->width);
  free(buf);
  return result;
}

saga_runtime_string *saga_int_format(int64_t val, const saga_runtime_string *fmt) {
  fmt_spec f = parse_fmt(fmt);
  char raw[64];
  int n;
  if (f.show_sign)
    n = snprintf(raw, sizeof(raw), "%+" PRId64, val);
  else
    n = snprintf(raw, sizeof(raw), "%" PRId64, val);
  return apply_padding(raw, n, &f);
}

saga_runtime_string *saga_float_format(double val, const saga_runtime_string *fmt) {
  fmt_spec f = parse_fmt(fmt);
  char raw[128];
  int n;
  if (f.scientific) {
    if (f.precision >= 0) {
      if (f.show_sign) n = snprintf(raw, sizeof(raw), "%+.*e", f.precision, val);
      else             n = snprintf(raw, sizeof(raw), "%.*e", f.precision, val);
    } else {
      if (f.show_sign) n = snprintf(raw, sizeof(raw), "%+e", val);
      else             n = snprintf(raw, sizeof(raw), "%e", val);
    }
  } else if (f.precision >= 0) {
    if (f.show_sign) n = snprintf(raw, sizeof(raw), "%+.*f", f.precision, val);
    else             n = snprintf(raw, sizeof(raw), "%.*f", f.precision, val);
  } else {
    if (f.show_sign) n = snprintf(raw, sizeof(raw), "%+g", val);
    else             n = snprintf(raw, sizeof(raw), "%g", val);
  }
  return apply_padding(raw, n, &f);
}

saga_runtime_string *saga_string_format(const saga_runtime_string *self, const saga_runtime_string *fmt) {
  fmt_spec f = parse_fmt(fmt);
  int slen = self ? (int)self->len : 0;
  const char *sdata = (self && self->data) ? self->data : "";
  if (slen >= f.width) return saga_runtime_alloc_string(sdata, slen);
  int pad = f.width - slen;
  char *buf = (char *)malloc((size_t)f.width);
  /* Strings are right-aligned with padding on the left by default. */
  memset(buf, f.fill, (size_t)pad);
  memcpy(buf + pad, sdata, (size_t)slen);
  saga_runtime_string *result = saga_runtime_alloc_string(buf, f.width);
  free(buf);
  return result;
}

/* ───────────────────────────────────────────────────────────────────────── */
/* Array                                                                    */
/*                                                                          */
/* Layout: { void *data, i64 len, i64 cap, i64 elem_size, i64 refcount }   */
/* (saga_runtime_array typedef is above, before string helpers that use it.)          */
/* ───────────────────────────────────────────────────────────────────────── */

/* ───────────────────────────────────────────────────────────────────────── */
/* Array refcounting                                                        */
/* ───────────────────────────────────────────────────────────────────────── */

void saga_retain_array(saga_runtime_array *arr) {
  if (arr && arr->refcount > 0)
    arr->refcount++;
}

void saga_release_array(saga_runtime_array *arr) {
  if (!arr || arr->refcount < 0) return;
  arr->refcount--;
  if (arr->refcount <= 0) {
    free(arr->data);
    free(arr);
  }
}

/* ───────────────────────────────────────────────────────────────────────── */
/* Array operations                                                         */
/* ───────────────────────────────────────────────────────────────────────── */

/* Internal versions used by string helpers (defined above via forward decl). */
static saga_runtime_array *saga_array_new_internal(int64_t elem_size, int64_t initial_cap) {
  if (initial_cap < 4) initial_cap = 4;
  saga_runtime_array *arr = (saga_runtime_array *)malloc(sizeof(saga_runtime_array));
  arr->data = malloc((size_t)(elem_size * initial_cap));
  arr->len = 0;
  arr->cap = initial_cap;
  arr->elem_size = elem_size;
  arr->refcount = 1;
  return arr;
}

static void saga_array_push_internal(saga_runtime_array *arr, const void *elem) {
  if (!arr) return;
  if (arr->len >= arr->cap) {
    arr->cap = arr->cap * 2;
    arr->data = realloc(arr->data, (size_t)(arr->elem_size * arr->cap));
  }
  memcpy((char *)arr->data + arr->elem_size * arr->len, elem,
         (size_t)arr->elem_size);
  arr->len++;
}

saga_runtime_array *saga_array_new(int64_t elem_size, int64_t initial_cap) {
  if (initial_cap < 4) initial_cap = 4;
  saga_runtime_array *arr = (saga_runtime_array *)malloc(sizeof(saga_runtime_array));
  arr->data = malloc((size_t)(elem_size * initial_cap));
  arr->len = 0;
  arr->cap = initial_cap;
  arr->elem_size = elem_size;
  arr->refcount = 1;
  return arr;
}

void saga_array_push(saga_runtime_array *arr, const void *elem) {
  if (!arr) return;
  if (arr->len >= arr->cap) {
    arr->cap = arr->cap * 2;
    arr->data = realloc(arr->data, (size_t)(arr->elem_size * arr->cap));
  }
  memcpy((char *)arr->data + arr->elem_size * arr->len, elem,
         (size_t)arr->elem_size);
  arr->len++;
}

void *saga_array_at(saga_runtime_array *arr, int64_t index) {
  if (!arr || index < 0 || index >= arr->len) return NULL;
  return (char *)arr->data + arr->elem_size * index;
}

int64_t saga_array_size(saga_runtime_array *arr) {
  return arr ? arr->len : 0;
}

int64_t saga_array_find(saga_runtime_array *arr, const void *elem, int64_t *out) {
  if (!arr || !elem) return 1;
  for (int64_t i = 0; i < arr->len; i++) {
    void *cur = (char *)arr->data + arr->elem_size * i;
    if (memcmp(cur, elem, (size_t)arr->elem_size) == 0) {
      *out = i;
      return 0;
    }
  }
  return 1; /* not found */
}

void saga_array_insert(saga_runtime_array *arr, const void *elem, int64_t index) {
  if (!arr || !elem) return;
  if (index < 0) index = 0;
  if (index > arr->len) index = arr->len;
  /* Ensure capacity. */
  if (arr->len >= arr->cap) {
    arr->cap = arr->cap * 2;
    arr->data = realloc(arr->data, (size_t)(arr->elem_size * arr->cap));
  }
  /* Shift elements right. */
  char *base = (char *)arr->data;
  int64_t es = arr->elem_size;
  if (index < arr->len) {
    memmove(base + es * (index + 1), base + es * index,
            (size_t)(es * (arr->len - index)));
  }
  memcpy(base + es * index, elem, (size_t)es);
  arr->len++;
}

void *saga_array_pop(saga_runtime_array *arr) {
  if (!arr || arr->len == 0) return NULL;
  arr->len--;
  return (char *)arr->data + arr->elem_size * arr->len;
}

void saga_array_set(saga_runtime_array *arr, int64_t index, const void *elem) {
  if (!arr || !elem || index < 0 || index >= arr->len) return;
  memcpy((char *)arr->data + arr->elem_size * index, elem,
         (size_t)arr->elem_size);
}

/* Materialize a [low, high) range into a fresh i64-element array.  Element  */
/* widths narrower than i64 (Char, Int8/16/32) share the same in-memory      */
/* representation by ABI, so a single i64 path covers all integer ranges.    */
saga_runtime_array *saga_range_to_array(int64_t low, int64_t high) {
  int64_t len = high > low ? high - low : 0;
  int64_t cap = len > 4 ? len : 4;
  saga_runtime_array *arr =
      (saga_runtime_array *)malloc(sizeof(saga_runtime_array));
  arr->data = malloc((size_t)(8 * cap));
  arr->len = len;
  arr->cap = cap;
  arr->elem_size = 8;
  arr->refcount = 1;
  int64_t *out = (int64_t *)arr->data;
  for (int64_t i = 0; i < len; i++)
    out[i] = low + i;
  return arr;
}

/* Shallow clone: new struct + new data buffer, contents memcpy'd.            */
/* Matches saga_array_equals: elements (pointer or aggregate value) are       */
/* shared by byte-copy, not deeply duplicated.                                */
saga_runtime_array *saga_array_clone(const saga_runtime_array *src) {
  if (!src) return NULL;
  saga_runtime_array *dst = (saga_runtime_array *)malloc(sizeof(*dst));
  dst->elem_size = src->elem_size;
  dst->len = src->len;
  dst->cap = src->cap > 0 ? src->cap : (src->len > 0 ? src->len : 4);
  dst->refcount = 1;
  dst->data = malloc((size_t)(dst->elem_size * dst->cap));
  if (src->len > 0)
    memcpy(dst->data, src->data, (size_t)(src->elem_size * src->len));
  return dst;
}

/* Element-wise byte comparison.  Matches saga_array_find semantics: arrays  */
/* of strings/structs compare by stored bytes (pointer or aggregate value),  */
/* not by deep content.                                                      */
int64_t saga_array_equals(const saga_runtime_array *a,
                          const saga_runtime_array *b) {
  if (a == b) return 1;
  if (!a || !b) return 0;
  if (a->len != b->len) return 0;
  if (a->elem_size != b->elem_size) return 0;
  if (a->len == 0) return 1;
  return memcmp(a->data, b->data,
                (size_t)(a->len * a->elem_size)) == 0 ? 1 : 0;
}

/* ───────────────────────────────────────────────────────────────────────── */
/* Map (ordered hash table)                                                 */
/*                                                                          */
/* Insertion-ordered map using a dense entry array + separate hash index.   */
/* Design follows CPython 3.7+ compact dict:                                */
/*   - entries[]: dense array of {key, value} in insertion order             */
/*   - indices[]: hash table mapping slot → index into entries[]             */
/*     sentinel values: -1 = empty, -2 = tombstone                          */
/*                                                                          */
/* Iteration is O(n) over the dense array; lookup is O(1) amortised.        */
/* Hash function: SipHash-2-4 (resistant to hash-flooding DoS).             */
/* ───────────────────────────────────────────────────────────────────────── */

typedef struct {
  void *key;
  void *value;
} saga_runtime_map_entry;

/* Key-kind tag.  Tells the runtime how to hash/compare keys without an     */
/* indirect call when the type is a known primitive.  USER keys go through */
/* the ops callback table.                                                  */
typedef enum {
  SAGA_KEY_KIND_USER   = 0, /* dispatch via ops                             */
  SAGA_KEY_KIND_INT64  = 1,
  SAGA_KEY_KIND_INT32  = 2,
  SAGA_KEY_KIND_INT16  = 3,
  SAGA_KEY_KIND_INT8   = 4,
  SAGA_KEY_KIND_UINT64 = 5,
  SAGA_KEY_KIND_UINT32 = 6,
  SAGA_KEY_KIND_UINT16 = 7,
  SAGA_KEY_KIND_UINT8  = 8,
  SAGA_KEY_KIND_BOOL   = 9,
  SAGA_KEY_KIND_STRING = 10,
} saga_runtime_key_kind;

/* Callback table for user-defined keys.  Pointed at constant tables emitted */
/* by codegen alongside the user type's monomorphised methods.               */
typedef struct {
  uint64_t (*hash)(const void *key);
  int      (*equals)(const void *a, const void *b);
} saga_runtime_key_ops;

typedef struct {
  saga_runtime_map_entry *entries;    /* dense array, insertion order                 */
  int64_t *indices;         /* hash table: slot → index (-1 empty, -2 del) */
  int64_t len;              /* number of live entries                       */
  int64_t entries_cap;      /* capacity of entries[]                        */
  int64_t index_cap;        /* capacity of indices[] (always power of 2)   */
  int64_t key_size;
  int64_t val_size;
  int64_t refcount;
  int64_t key_kind;                       /* saga_runtime_key_kind                  */
  const saga_runtime_key_ops *ops;        /* non-NULL iff key_kind == USER          */
} saga_runtime_map;

#define SAGA_RUNTIME_MAP_EMPTY    (-1)
#define SAGA_RUNTIME_MAP_DELETED  (-2)

/* ───────────────────────────────────────────────────────────────────────── */
/* SipHash-1-3                                                              */
/*                                                                          */
/* Reference: Jean-Philippe Aumasson & Daniel J. Bernstein, 2012.          */
/* 1 compression round per block, 3 finalisation rounds.                    */
/* Recommended by the authors for hash-table use when the key is secret.    */
/*                                                                          */
/* The 128-bit key is randomly seeded once at process start via             */
/* getrandom(2), making hash output unpredictable to external observers     */
/* and preventing HashDoS collision attacks.                                */
/* ───────────────────────────────────────────────────────────────────────── */

#include <sys/random.h>

static uint64_t saga_runtime_sip_k0;
static uint64_t saga_runtime_sip_k1;

__attribute__((constructor))
static void saga_runtime_siphash_init_key(void) {
  uint8_t buf[16];
  /* getrandom() won't short-read for 16 bytes on Linux. */
  if (getrandom(buf, sizeof(buf), 0) != sizeof(buf)) {
    /* Fallback: mix address-space and time entropy.  Not great, but     */
    /* strictly better than a fixed constant and only hit if getrandom   */
    /* is somehow unavailable.                                           */
    saga_runtime_sip_k0 = (uint64_t)(uintptr_t)&saga_runtime_sip_k0 * 6364136223846793005ULL;
    saga_runtime_sip_k1 = (uint64_t)(uintptr_t)&saga_runtime_sip_k1 * 1442695040888963407ULL;
    return;
  }
  memcpy(&saga_runtime_sip_k0, buf,     8);
  memcpy(&saga_runtime_sip_k1, buf + 8, 8);
}

static inline uint64_t sip_rotl(uint64_t x, int b) {
  return (x << b) | (x >> (64 - b));
}

#define SIP_ROUND            \
  do {                       \
    v0 += v1;                \
    v1 = sip_rotl(v1, 13);  \
    v1 ^= v0;               \
    v0 = sip_rotl(v0, 32);  \
    v2 += v3;                \
    v3 = sip_rotl(v3, 16);  \
    v3 ^= v2;               \
    v0 += v3;                \
    v3 = sip_rotl(v3, 21);  \
    v3 ^= v0;               \
    v2 += v1;                \
    v1 = sip_rotl(v1, 17);  \
    v1 ^= v2;               \
    v2 = sip_rotl(v2, 32);  \
  } while (0)

static uint64_t saga_runtime_siphash(const uint8_t *data, int64_t len) {
  uint64_t v0 = saga_runtime_sip_k0 ^ 0x736f6d6570736575ULL;
  uint64_t v1 = saga_runtime_sip_k1 ^ 0x646f72616e646f6dULL;
  uint64_t v2 = saga_runtime_sip_k0 ^ 0x6c7967656e657261ULL;
  uint64_t v3 = saga_runtime_sip_k1 ^ 0x7465646279746573ULL;

  const uint8_t *end = data + (len - (len % 8));
  uint64_t m;
  for (const uint8_t *p = data; p < end; p += 8) {
    memcpy(&m, p, 8);
    v3 ^= m;
    SIP_ROUND;              /* 1 compression round */
    v0 ^= m;
  }

  /* Last block: remaining bytes + length tag in top byte. */
  const uint8_t *tail = end;
  m = (uint64_t)((uint8_t)len) << 56;
  switch (len & 7) {
    case 7: m |= (uint64_t)tail[6] << 48; /* fall through */
    case 6: m |= (uint64_t)tail[5] << 40; /* fall through */
    case 5: m |= (uint64_t)tail[4] << 32; /* fall through */
    case 4: m |= (uint64_t)tail[3] << 24; /* fall through */
    case 3: m |= (uint64_t)tail[2] << 16; /* fall through */
    case 2: m |= (uint64_t)tail[1] <<  8; /* fall through */
    case 1: m |= (uint64_t)tail[0];        break;
    case 0: break;
  }
  v3 ^= m;
  SIP_ROUND;                /* 1 compression round */
  v0 ^= m;

  /* Finalisation: 3 rounds. */
  v2 ^= 0xff;
  SIP_ROUND; SIP_ROUND; SIP_ROUND;

  return v0 ^ v1 ^ v2 ^ v3;
}

#undef SIP_ROUND

/* Public per-primitive hash functions exposed to stdlib via             */
/* intrinsic_runtime.  Each routes the concrete primitive payload        */
/* through SipHash so user-visible Hash() methods produce the same bits  */
/* the runtime uses internally for map keys.                             */
int64_t saga_int_hash(int64_t v) {
  return (int64_t)saga_runtime_siphash((const uint8_t *)&v, sizeof v);
}

int64_t saga_string_hash(const saga_runtime_string *s) {
  if (!s || !s->data || s->len <= 0)
    return (int64_t)saga_runtime_siphash((const uint8_t *)"", 0);
  return (int64_t)saga_runtime_siphash((const uint8_t *)s->data, s->len);
}

int64_t saga_bool_hash(int64_t v) {
  /* Storage is i64; only the low bit is meaningful. */
  uint8_t b = (uint8_t)(v & 1);
  return (int64_t)saga_runtime_siphash(&b, 1);
}

/* ── Key hashing / comparison helpers ──────────────────────────────────── */

static uint64_t saga_runtime_map_hash_string(const void *key) {
  const saga_runtime_string *s = *(const saga_runtime_string *const *)key;
  if (!s || !s->data || s->len <= 0)
    return saga_runtime_siphash((const uint8_t *)"", 0);
  return saga_runtime_siphash((const uint8_t *)s->data, s->len);
}

static uint64_t saga_runtime_map_hash_bytes(const void *key, int64_t key_size) {
  return saga_runtime_siphash((const uint8_t *)key, key_size);
}

static uint64_t saga_runtime_map_hash_key(const saga_runtime_map *m, const void *key) {
  switch ((saga_runtime_key_kind)m->key_kind) {
    case SAGA_KEY_KIND_USER:
      return m->ops->hash(key);
    case SAGA_KEY_KIND_STRING:
      return saga_runtime_map_hash_string(key);
    default:
      return saga_runtime_map_hash_bytes(key, m->key_size);
  }
}

static int saga_runtime_map_keys_equal_string(const void *a, const void *b) {
  const saga_runtime_string *sa = *(const saga_runtime_string *const *)a;
  const saga_runtime_string *sb = *(const saga_runtime_string *const *)b;
  if (sa == sb) return 1;
  if (!sa || !sb) return 0;
  if (sa->len != sb->len) return 0;
  if (sa->len == 0) return 1;
  return memcmp(sa->data, sb->data, (size_t)sa->len) == 0;
}

static int saga_runtime_map_keys_equal(const saga_runtime_map *m, const void *a, const void *b) {
  switch ((saga_runtime_key_kind)m->key_kind) {
    case SAGA_KEY_KIND_USER:
      return m->ops->equals(a, b);
    case SAGA_KEY_KIND_STRING:
      return saga_runtime_map_keys_equal_string(a, b);
    default:
      return memcmp(a, b, (size_t)m->key_size) == 0;
  }
}

/* ── Internal: index table probing ─────────────────────────────────────── */

/* Find the index-table slot for `key`.                                     */
/* Returns the slot that is either empty, deleted, or holds a matching key. */
/* On match, *out_entry_idx is set to the entries[] index.                  */
/* On empty/deleted, *out_entry_idx is set to -1.                           */

static int64_t saga_runtime_map_probe(const saga_runtime_map *m, const void *key,
                            int64_t *out_entry_idx) {
  uint64_t h = saga_runtime_map_hash_key(m, key);
  int64_t mask = m->index_cap - 1; /* index_cap is always a power of 2 */
  int64_t slot = (int64_t)(h & (uint64_t)mask);
  int64_t first_deleted = -1;

  for (int64_t i = 0; i < m->index_cap; i++) {
    int64_t s = (slot + i) & mask;
    int64_t eidx = m->indices[s];

    if (eidx == SAGA_RUNTIME_MAP_EMPTY) {
      *out_entry_idx = -1;
      return (first_deleted >= 0) ? first_deleted : s;
    }
    if (eidx == SAGA_RUNTIME_MAP_DELETED) {
      if (first_deleted < 0) first_deleted = s;
      continue;
    }
    /* Occupied — compare keys. */
    if (saga_runtime_map_keys_equal(m, m->entries[eidx].key, key)) {
      *out_entry_idx = eidx;
      return s;
    }
  }
  /* Table full (should never happen if load < 1). */
  *out_entry_idx = -1;
  return (first_deleted >= 0) ? first_deleted : -1;
}

/* ── Rebuild / resize ──────────────────────────────────────────────────── */

static void saga_runtime_map_rebuild_index(saga_runtime_map *m) {
  /* Reset index table. */
  for (int64_t i = 0; i < m->index_cap; i++)
    m->indices[i] = SAGA_RUNTIME_MAP_EMPTY;

  int64_t mask = m->index_cap - 1;
  for (int64_t ei = 0; ei < m->len; ei++) {
    uint64_t h = saga_runtime_map_hash_key(m, m->entries[ei].key);
    int64_t slot = (int64_t)(h & (uint64_t)mask);
    while (m->indices[slot] >= 0)
      slot = (slot + 1) & mask;
    m->indices[slot] = ei;
  }
}

static void saga_runtime_map_grow(saga_runtime_map *m) {
  /* Double the entries capacity. */
  int64_t new_ecap = m->entries_cap * 2;
  if (new_ecap < 16) new_ecap = 16;
  m->entries = (saga_runtime_map_entry *)realloc(m->entries,
      (size_t)new_ecap * sizeof(saga_runtime_map_entry));
  m->entries_cap = new_ecap;

  /* Index table should be ~2x the entry count for low load factor. */
  int64_t new_icap = m->index_cap;
  while (new_icap < new_ecap * 2)
    new_icap *= 2;
  if (new_icap != m->index_cap) {
    m->indices = (int64_t *)realloc(m->indices,
        (size_t)new_icap * sizeof(int64_t));
    m->index_cap = new_icap;
  }
  saga_runtime_map_rebuild_index(m);
}

/* ── Refcounting ───────────────────────────────────────────────────────── */

void saga_retain_map(saga_runtime_map *m) {
  if (m && m->refcount > 0)
    m->refcount++;
}

void saga_release_map(saga_runtime_map *m) {
  if (!m || m->refcount < 0) return;
  m->refcount--;
  if (m->refcount <= 0) {
    for (int64_t i = 0; i < m->len; i++) {
      free(m->entries[i].key);
      free(m->entries[i].value);
    }
    free(m->entries);
    free(m->indices);
    free(m);
  }
}

/* ── Public API ────────────────────────────────────────────────────────── */

saga_runtime_map *saga_map_new(int64_t key_size, int64_t val_size,
                     int64_t key_kind, const saga_runtime_key_ops *ops) {
  int64_t initial_ecap = 8;
  int64_t initial_icap = 16; /* power of 2, ≥ 2 * initial_ecap */

  saga_runtime_map *m = (saga_runtime_map *)malloc(sizeof(saga_runtime_map));
  m->entries = (saga_runtime_map_entry *)malloc((size_t)initial_ecap * sizeof(saga_runtime_map_entry));
  m->indices = (int64_t *)malloc((size_t)initial_icap * sizeof(int64_t));
  for (int64_t i = 0; i < initial_icap; i++)
    m->indices[i] = SAGA_RUNTIME_MAP_EMPTY;

  m->len = 0;
  m->entries_cap = initial_ecap;
  m->index_cap = initial_icap;
  m->key_size = key_size;
  m->val_size = val_size;
  m->refcount = 1;
  m->key_kind = key_kind;
  m->ops = ops;
  return m;
}

void saga_map_set(saga_runtime_map *m, const void *key, const void *value) {
  if (!m) return;

  int64_t entry_idx;
  int64_t slot = saga_runtime_map_probe(m, key, &entry_idx);

  if (entry_idx >= 0) {
    /* Key exists — update value in place (preserves insertion order). */
    memcpy(m->entries[entry_idx].value, value, (size_t)m->val_size);
    return;
  }

  /* New key — grow if entries array is full. */
  if (m->len >= m->entries_cap) {
    saga_runtime_map_grow(m);
    /* Slot may have moved; re-probe. */
    slot = saga_runtime_map_probe(m, key, &entry_idx);
  }
  /* Also check index load factor (> 2/3). */
  if ((m->len + 1) * 3 > m->index_cap * 2) {
    saga_runtime_map_grow(m);
    slot = saga_runtime_map_probe(m, key, &entry_idx);
  }

  /* Append to the dense entries array. */
  int64_t ei = m->len;
  m->entries[ei].key = malloc((size_t)m->key_size);
  memcpy(m->entries[ei].key, key, (size_t)m->key_size);
  m->entries[ei].value = malloc((size_t)m->val_size);
  memcpy(m->entries[ei].value, value, (size_t)m->val_size);

  m->indices[slot] = ei;
  m->len++;
}

void *saga_map_get(saga_runtime_map *m, const void *key) {
  if (!m || m->len == 0) return NULL;

  int64_t entry_idx;
  saga_runtime_map_probe(m, key, &entry_idx);
  if (entry_idx < 0) return NULL;
  return m->entries[entry_idx].value;
}

int64_t saga_map_has(saga_runtime_map *m, const void *key) {
  return saga_map_get(m, key) != NULL ? 1 : 0;
}

void saga_map_remove(saga_runtime_map *m, const void *key) {
  if (!m || m->len == 0) return;

  int64_t entry_idx;
  int64_t slot = saga_runtime_map_probe(m, key, &entry_idx);
  if (entry_idx < 0) return;

  /* Free the key/value being removed. */
  free(m->entries[entry_idx].key);
  free(m->entries[entry_idx].value);

  /* Swap the last entry into the hole to keep the dense array compact.    */
  /* This changes the *iteration* order slightly: the last-inserted entry  */
  /* moves into the deleted position.  This is the standard trade-off for  */
  /* O(1) delete in a dense-array ordered map.                             */
  int64_t last = m->len - 1;
  if (entry_idx != last) {
    m->entries[entry_idx] = m->entries[last];
    /* Update the index slot that pointed at `last` to point at the new    */
    /* position.                                                           */
    int64_t moved_eidx;
    int64_t moved_slot = saga_runtime_map_probe(m, m->entries[entry_idx].key,
                                      &moved_eidx);
    /* moved_eidx should still be `last`; fix it. */
    (void)moved_slot;
    /* Scan the index for the entry pointing at `last` and rewrite it. */
    for (int64_t i = 0; i < m->index_cap; i++) {
      if (m->indices[i] == last) {
        m->indices[i] = entry_idx;
        break;
      }
    }
  }

  m->indices[slot] = SAGA_RUNTIME_MAP_DELETED;
  m->len--;
}

int64_t saga_map_size(saga_runtime_map *m) {
  return m ? m->len : 0;
}

/* O(1) access by insertion index — the dense array IS the order. */
void *saga_map_key_at(saga_runtime_map *m, int64_t index) {
  if (!m || index < 0 || index >= m->len) return NULL;
  return m->entries[index].key;
}

void *saga_map_value_at(saga_runtime_map *m, int64_t index) {
  if (!m || index < 0 || index >= m->len) return NULL;
  return m->entries[index].value;
}

saga_runtime_array *saga_map_keys(saga_runtime_map *m) {
  int64_t key_size = m ? m->key_size : 8;
  int64_t len = m ? m->len : 0;
  saga_runtime_array *arr = saga_array_new_internal(key_size, len > 4 ? len : 4);
  for (int64_t i = 0; i < len; i++) {
    saga_array_push_internal(arr, m->entries[i].key);
  }
  return arr;
}

/* Order-independent equality: same size, same key/value layout, every key   */
/* in `a` exists in `b` with byte-equal values.  Keys are compared via the   */
/* same path saga_map_get uses, so string keys compare by content.  Values   */
/* compare by stored bytes — string-valued maps compare by pointer.          */
int64_t saga_map_equals(saga_runtime_map *a, saga_runtime_map *b) {
  if (a == b) return 1;
  if (!a || !b) return 0;
  if (a->len != b->len) return 0;
  if (a->key_size != b->key_size) return 0;
  if (a->val_size != b->val_size) return 0;
  if (a->key_kind != b->key_kind) return 0;
  if (a->key_kind == SAGA_KEY_KIND_USER && a->ops != b->ops) return 0;
  for (int64_t i = 0; i < a->len; i++) {
    void *bv = saga_map_get(b, a->entries[i].key);
    if (!bv) return 0;
    if (memcmp(a->entries[i].value, bv, (size_t)a->val_size) != 0) return 0;
  }
  return 1;
}

/* ───────────────────────────────────────────────────────────────────────── */
/* Intrinsics                                                               */
/* ───────────────────────────────────────────────────────────────────────── */

void saga_intrinsic_print(const saga_runtime_string *s) {
  if (s && s->data && s->len > 0) {
    fwrite(s->data, 1, (size_t)s->len, stdout);
  }
  fflush(stdout);
}

/* ───────────────────────────────────────────────────────────────────────── */
/* Arena-aware allocation helpers                                           */
/*                                                                          */
/* These mirror saga_runtime_alloc_string / saga_array_new / saga_runtime_map_new but allocate    */
/* from an arena instead of malloc.  The refcount is set to -1 (static /    */
/* never-freed) because the arena owns the memory — individual objects are  */
/* not freed; the entire arena is munmap'd at once.                         */
/* ───────────────────────────────────────────────────────────────────────── */

saga_runtime_string *saga_runtime_arena_alloc_string(saga_runtime_arena *a, const char *buf, int64_t len) {
  if (!a) return NULL;

  saga_runtime_string *s = (saga_runtime_string *)saga_runtime_arena_alloc(a, (int64_t)sizeof(saga_runtime_string));
  if (!s) return NULL;

  char *data = (char *)saga_runtime_arena_alloc(a, len > 0 ? len : 1);
  if (!data) return NULL;
  if (len > 0 && buf) memcpy(data, buf, (size_t)len);

  s->data     = data;
  s->len      = len;
  s->refcount = -1; /* arena-owned: never individually freed */
  return s;
}

saga_runtime_array *saga_runtime_arena_alloc_array(saga_runtime_arena *a, int64_t elem_size,
                               int64_t initial_cap) {
  if (!a) return NULL;
  if (initial_cap < 4) initial_cap = 4;

  saga_runtime_array *arr = (saga_runtime_array *)saga_runtime_arena_alloc(a, (int64_t)sizeof(saga_runtime_array));
  if (!arr) return NULL;

  void *data = saga_runtime_arena_alloc(a, elem_size * initial_cap);
  if (!data) return NULL;

  arr->data      = data;
  arr->len       = 0;
  arr->cap       = initial_cap;
  arr->elem_size = elem_size;
  arr->refcount  = -1; /* arena-owned */
  return arr;
}

saga_runtime_map *saga_runtime_arena_alloc_map(saga_runtime_arena *a, int64_t key_size, int64_t val_size,
                          int64_t key_kind, const saga_runtime_key_ops *ops) {
  if (!a) return NULL;

  int64_t initial_ecap = 8;
  int64_t initial_icap = 16;

  saga_runtime_map *m = (saga_runtime_map *)saga_runtime_arena_alloc(a, (int64_t)sizeof(saga_runtime_map));
  if (!m) return NULL;

  saga_runtime_map_entry *entries = (saga_runtime_map_entry *)saga_runtime_arena_alloc(
      a, initial_ecap * (int64_t)sizeof(saga_runtime_map_entry));
  if (!entries) return NULL;

  int64_t *indices = (int64_t *)saga_runtime_arena_alloc(
      a, initial_icap * (int64_t)sizeof(int64_t));
  if (!indices) return NULL;

  for (int64_t i = 0; i < initial_icap; i++)
    indices[i] = SAGA_RUNTIME_MAP_EMPTY;

  m->entries     = entries;
  m->indices     = indices;
  m->len         = 0;
  m->entries_cap = initial_ecap;
  m->index_cap   = initial_icap;
  m->key_size    = key_size;
  m->val_size    = val_size;
  m->refcount    = -1; /* arena-owned */
  m->key_kind    = key_kind;
  m->ops         = ops;
  return m;
}

/* ───────────────────────────────────────────────────────────────────────── */
/* Copy-on-Write helpers                                                    */
/*                                                                          */
/* These create a deep copy of a refcounted object into a target arena.     */
/* Called when a shared object is about to be mutated inside a spawn block.  */
/*                                                                          */
/* The COW barrier pattern (emitted by codegen):                            */
/*   if (obj->refcount > 1) obj = saga_runtime_cow_copy_*(arena, obj);               */
/* ───────────────────────────────────────────────────────────────────────── */

/*
 * saga_runtime_cow_copy_string  —  deep-copy a string into an arena.
 */
saga_runtime_string *saga_runtime_cow_copy_string(saga_runtime_arena *a, saga_runtime_string *src) {
  if (!a || !src) return src;
  saga_runtime_string *copy = saga_runtime_arena_alloc_string(a, src->data, src->len);
  if (copy) saga_release_string(src); /* drop the shared ref */
  return copy ? copy : src;         /* fallback to original on alloc failure */
}

/*
 * saga_runtime_cow_copy_array  —  deep-copy an array into an arena.
 *
 * Elements are shallow-copied (memcpy).  For arrays of refcounted types,
 * the caller (codegen) must recursively COW-copy each element.
 */
saga_runtime_array *saga_runtime_cow_copy_array(saga_runtime_arena *a, saga_runtime_array *src) {
  if (!a || !src) return src;
  saga_runtime_array *copy = saga_runtime_arena_alloc_array(a, src->elem_size, src->cap);
  if (!copy) return src;

  if (src->len > 0 && src->data)
    memcpy(copy->data, src->data, (size_t)(src->elem_size * src->len));
  copy->len = src->len;

  saga_release_array(src);
  return copy;
}
