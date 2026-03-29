/* Copyright 2026 Rob Thornton
 * SPDX-License-Identifier: MIT
 *
 * Minimal runtime for the Saga language.
 * Compiled into a static library and linked into every Saga binary.
 */

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

#ifndef MC_MAX_ARENA_SIZE
#define MC_MAX_ARENA_SIZE (64 * 1024 * 1024) /* 64 MB default */
#endif

#ifndef MC_MAX_REDUCTIONS
#define MC_MAX_REDUCTIONS 1000000
#endif

#ifndef MC_HEARTBEAT_TIMEOUT_MS
#define MC_HEARTBEAT_TIMEOUT_MS 5000
#endif

#ifndef MC_ARENA_INITIAL_COMMIT
#define MC_ARENA_INITIAL_COMMIT (64 * 1024) /* 64 KB initial commit */
#endif

#define MC_ARENA_ALIGN 16

typedef struct {
  char    *base;      /* mmap'd region — never moves                       */
  int64_t  offset;    /* next free byte (bump pointer)                     */
  int64_t  committed; /* bytes currently backed by physical pages          */
  int64_t  reserved;  /* total virtual reservation                         */
  int64_t  max_limit; /* hard quota — allocation fails if exceeded         */
} mc_arena;

/* Round `n` up to the nearest multiple of `align` (must be power of 2). */
static inline int64_t mc_align_up(int64_t n, int64_t align) {
  return (n + align - 1) & ~(align - 1);
}

/* Return the system page size, cached after first call. */
static inline int64_t mc_page_size(void) {
  static int64_t ps = 0;
  if (ps == 0) ps = (int64_t)sysconf(_SC_PAGESIZE);
  return ps;
}

/*
 * mc_arena_new  —  reserve virtual address space and commit an initial chunk.
 *
 * `max_limit` is the hard memory quota for this arena.  If 0 the compile-time
 * default MC_MAX_ARENA_SIZE is used.  The virtual reservation is the larger of
 * max_limit and MC_MAX_ARENA_SIZE (virtual space is free on 64-bit).
 *
 * Returns NULL if the mmap or initial commit fails.
 */
mc_arena *mc_arena_new(int64_t max_limit) {
  if (max_limit <= 0) max_limit = MC_MAX_ARENA_SIZE;

  int64_t page = mc_page_size();
  int64_t reserved = max_limit;
  if (reserved < MC_MAX_ARENA_SIZE) reserved = MC_MAX_ARENA_SIZE;
  reserved = mc_align_up(reserved, page);

  /* Reserve the entire virtual region with no access permissions. */
  void *base = mmap(NULL, (size_t)reserved,
                    PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                    -1, 0);
  if (base == MAP_FAILED) return NULL;

  /* Commit the initial chunk so early allocations don't page-fault. */
  int64_t initial = MC_ARENA_INITIAL_COMMIT;
  if (initial > reserved) initial = reserved;
  initial = mc_align_up(initial, page);
  if (mprotect(base, (size_t)initial, PROT_READ | PROT_WRITE) != 0) {
    munmap(base, (size_t)reserved);
    return NULL;
  }

  mc_arena *a = (mc_arena *)malloc(sizeof(mc_arena));
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
 * mc_arena_alloc  —  bump-allocate `size` bytes from the arena.
 *
 * Returns a 16-byte-aligned pointer, or NULL if the allocation would exceed
 * the arena's max_limit (the caller is expected to kill the actor).
 */
void *mc_arena_alloc(mc_arena *a, int64_t size) {
  if (!a || size <= 0) return NULL;

  int64_t aligned = mc_align_up(size, MC_ARENA_ALIGN);
  int64_t new_offset = a->offset + aligned;

  /* Hard quota check. */
  if (new_offset > a->max_limit) return NULL;

  /* Commit more pages if needed. */
  if (new_offset > a->committed) {
    int64_t page = mc_page_size();
    /* Commit in chunks: at least double current committed or enough for the
     * request, whichever is larger, capped at reserved. */
    int64_t want = a->committed * 2;
    if (want < new_offset) want = mc_align_up(new_offset, page);
    if (want > a->reserved) want = a->reserved;
    want = mc_align_up(want, page);

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
 * mc_arena_destroy  —  release the entire virtual region in one call.
 *
 * After this call every pointer into the arena is invalid.
 */
void mc_arena_destroy(mc_arena *a) {
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
/* The mc_actor struct itself is heap-allocated (malloc) and refcounted so   */
/* it outlives the arena.  The worker holds one ref, the parent's Task      */
/* handle holds another.  The struct is freed when refcount reaches 0.      */
/*                                                                          */
/* Lifecycle: PENDING → RUNNING → COMPLETED | KILLED | ZOMBIE              */
/* ───────────────────────────────────────────────────────────────────────── */

enum {
  MC_ACTOR_PENDING   = 0,
  MC_ACTOR_RUNNING   = 1,
  MC_ACTOR_COMPLETED = 2,
  MC_ACTOR_CANCELLED = 3,
  MC_ACTOR_KILLED    = 4,
  MC_ACTOR_ZOMBIE    = 5
};

/* Forward declarations — full definitions follow below. */
typedef struct mc_actor   mc_actor;
typedef struct mc_channel mc_channel;
void mc_channel_close(mc_channel *ch);
int  mc_channel_send(mc_channel *ch, const void *data, mc_actor *actor);

typedef struct mc_actor {
  /* -- Stable fields (malloc'd, outlive the arena) ---------------------- */
  int64_t          refcount;       /* 1 worker + 1 parent Task handle      */
  void            *result;         /* exit value, copied out before arena   */
  int64_t          result_size;    /*   dies — always malloc'd             */
  int64_t          status;         /* MC_ACTOR_{PENDING,RUNNING,...}        */
  int64_t          cancelled;      /* set by parent Cancel(), read by actor */
  pthread_mutex_t  lock;           /* guards status transitions             */
  pthread_cond_t   done_cond;      /* signalled on completion (for Wait())  */

  /* -- Arena-lifetime fields -------------------------------------------- */
  mc_arena        *arena;
  void           (*entry)(struct mc_actor *);
  void            *closure_data;   /* captured vars packed by codegen       */
  int64_t          closure_size;
  int64_t          reduction_count;
  int64_t          last_cycle;     /* monotonic ns timestamp                */
  mc_channel      *channel;        /* typed channel for Send() / iteration  */
  void            *result_in_arena;/* set by context_exit, lives in arena   */
  jmp_buf          trap;           /* setjmp target for panic / quota       */
} mc_actor;

/* Monotonic clock helper (nanoseconds). */
static int64_t mc_monotonic_now(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec;
}

mc_actor *mc_actor_new(void (*entry)(mc_actor *), void *closure_data,
                       int64_t closure_size, int64_t arena_max) {
  mc_actor *a = (mc_actor *)calloc(1, sizeof(mc_actor));
  if (!a) return NULL;

  a->refcount      = 2; /* one for worker, one for parent Task handle */
  a->status        = MC_ACTOR_PENDING;
  a->cancelled     = 0;
  a->result        = NULL;
  a->result_size   = 0;
  a->entry         = entry;
  a->closure_size  = closure_size;
  a->reduction_count = 0;
  a->last_cycle    = mc_monotonic_now();
  a->channel       = NULL;

  pthread_mutex_init(&a->lock, NULL);
  pthread_cond_init(&a->done_cond, NULL);

  /* Create the actor's arena. */
  a->arena = mc_arena_new(arena_max);
  if (!a->arena) {
    pthread_mutex_destroy(&a->lock);
    pthread_cond_destroy(&a->done_cond);
    free(a);
    return NULL;
  }

  /* Copy closure data into the actor's arena so it's fully owned. */
  if (closure_data && closure_size > 0) {
    a->closure_data = mc_arena_alloc(a->arena, closure_size);
    if (!a->closure_data) {
      mc_arena_destroy(a->arena);
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

void mc_actor_retain(mc_actor *a) {
  if (!a) return;
  __atomic_add_fetch(&a->refcount, 1, __ATOMIC_SEQ_CST);
}

void mc_actor_release(mc_actor *a) {
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
 * mc_reduction_tick  —  called at the top of every loop iteration inside
 *                       a spawned actor.  Checks status poisoning and
 *                       enforces the CPU quota.
 *
 * If the actor has been killed (e.g. by mc_task_term or heartbeat monitor),
 * it longjmps out immediately.  If the reduction counter exceeds the
 * compile-time limit, the actor is killed.
 */
void mc_reduction_tick(mc_actor *a) {
  if (!a) return;

  /* Status poisoning — killed externally? */
  if (__atomic_load_n(&a->status, __ATOMIC_ACQUIRE) == MC_ACTOR_KILLED)
    longjmp(a->trap, 1);

  a->reduction_count++;
  if (a->reduction_count > MC_MAX_REDUCTIONS) {
    __atomic_store_n(&a->status, MC_ACTOR_KILLED, __ATOMIC_RELEASE);
    longjmp(a->trap, 1);
  }

  a->last_cycle = mc_monotonic_now();
}

/* ───────────────────────────────────────────────────────────────────────── */
/* Work-stealing deque                                                      */
/*                                                                          */
/* Mutex-protected double-ended queue.  The owning worker pushes/pops from  */
/* the tail (LIFO for cache locality); thieves steal from the head (FIFO).  */
/* ───────────────────────────────────────────────────────────────────────── */

#ifndef MC_DEQUE_INITIAL_CAP
#define MC_DEQUE_INITIAL_CAP 256
#endif

typedef struct {
  mc_actor      **buffer;
  int64_t         head;  /* steal from here (FIFO end) */
  int64_t         tail;  /* push/pop here  (LIFO end)  */
  int64_t         cap;
  pthread_mutex_t lock;
} mc_deque;

static void mc_deque_init(mc_deque *d) {
  d->buffer = (mc_actor **)calloc((size_t)MC_DEQUE_INITIAL_CAP,
                                  sizeof(mc_actor *));
  d->head = 0;
  d->tail = 0;
  d->cap  = MC_DEQUE_INITIAL_CAP;
  pthread_mutex_init(&d->lock, NULL);
}

static void mc_deque_destroy(mc_deque *d) {
  if (!d) return;
  free(d->buffer);
  d->buffer = NULL;
  pthread_mutex_destroy(&d->lock);
}

static int64_t mc_deque_size_locked(mc_deque *d) {
  return d->tail - d->head;
}

/* Grow the ring buffer (caller must hold the lock). */
static void mc_deque_grow(mc_deque *d) {
  int64_t new_cap = d->cap * 2;
  mc_actor **new_buf = (mc_actor **)calloc((size_t)new_cap,
                                           sizeof(mc_actor *));
  int64_t n = mc_deque_size_locked(d);
  for (int64_t i = 0; i < n; i++)
    new_buf[i] = d->buffer[(d->head + i) % d->cap];
  free(d->buffer);
  d->buffer = new_buf;
  d->head   = 0;
  d->tail   = n;
  d->cap    = new_cap;
}

/* Push an actor onto the tail (owner side). */
void mc_deque_push(mc_deque *d, mc_actor *actor) {
  pthread_mutex_lock(&d->lock);
  if (mc_deque_size_locked(d) >= d->cap)
    mc_deque_grow(d);
  d->buffer[d->tail % d->cap] = actor;
  d->tail++;
  pthread_mutex_unlock(&d->lock);
}

/* Pop from the tail (owner side, LIFO). Returns NULL if empty. */
mc_actor *mc_deque_pop(mc_deque *d) {
  pthread_mutex_lock(&d->lock);
  if (mc_deque_size_locked(d) <= 0) {
    pthread_mutex_unlock(&d->lock);
    return NULL;
  }
  d->tail--;
  mc_actor *a = d->buffer[d->tail % d->cap];
  pthread_mutex_unlock(&d->lock);
  return a;
}

/* Steal from the head (thief side, FIFO). Returns NULL if empty. */
mc_actor *mc_deque_steal(mc_deque *d) {
  pthread_mutex_lock(&d->lock);
  if (mc_deque_size_locked(d) <= 0) {
    pthread_mutex_unlock(&d->lock);
    return NULL;
  }
  mc_actor *a = d->buffer[d->head % d->cap];
  d->head++;
  pthread_mutex_unlock(&d->lock);
  return a;
}

/* Drain all actors from `src` into `dst`.  Used during worker replacement  */
/* to rescue stranded actors before detaching a stuck thread.               */
void mc_deque_drain(mc_deque *src, mc_deque *dst) {
  pthread_mutex_lock(&src->lock);
  while (mc_deque_size_locked(src) > 0) {
    mc_actor *a = src->buffer[src->head % src->cap];
    src->head++;
    /* Push into dst (acquires dst lock internally). */
    pthread_mutex_unlock(&src->lock);
    mc_deque_push(dst, a);
    pthread_mutex_lock(&src->lock);
  }
  pthread_mutex_unlock(&src->lock);
}

/* ───────────────────────────────────────────────────────────────────────── */
/* Executor                                                                 */
/*                                                                          */
/* A fixed-size thread pool with per-worker deques and a global inject      */
/* queue.  Workers sleep on a condition variable when idle and are woken     */
/* instantly when work is submitted.                                        */
/* ───────────────────────────────────────────────────────────────────────── */

typedef struct {
  int64_t          id;       /* index into executor arrays                  */
  void            *executor; /* back-pointer (cast to mc_executor*)         */
} mc_worker_ctx;

/* ── Active actor list (for heartbeat monitor) ─────────────────────────── */
/* A simple array-based list of actors currently being executed.  Protected */
/* by its own mutex so the monitor thread can scan without contending on    */
/* the work-stealing deques.                                                */

#ifndef MC_ACTIVE_LIST_CAP
#define MC_ACTIVE_LIST_CAP 1024
#endif

typedef struct {
  mc_actor       **actors;
  int64_t          len;
  int64_t          cap;
  pthread_mutex_t  lock;
} mc_active_list;

static void mc_active_list_init(mc_active_list *l) {
  l->actors = (mc_actor **)calloc(MC_ACTIVE_LIST_CAP, sizeof(mc_actor *));
  l->len    = 0;
  l->cap    = MC_ACTIVE_LIST_CAP;
  pthread_mutex_init(&l->lock, NULL);
}

static void mc_active_list_destroy(mc_active_list *l) {
  free(l->actors);
  pthread_mutex_destroy(&l->lock);
}

static void mc_active_list_add(mc_active_list *l, mc_actor *a) {
  pthread_mutex_lock(&l->lock);
  if (l->len >= l->cap) {
    l->cap *= 2;
    l->actors = (mc_actor **)realloc(l->actors,
                                     (size_t)l->cap * sizeof(mc_actor *));
  }
  l->actors[l->len++] = a;
  pthread_mutex_unlock(&l->lock);
}

static void mc_active_list_remove(mc_active_list *l, mc_actor *a) {
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

typedef struct mc_executor {
  pthread_t       *threads;
  mc_worker_ctx   *worker_ctxs;
  mc_deque        *deques;          /* one per worker slot                  */
  int64_t          num_workers;
  mc_deque         inject_queue;    /* global queue for newly spawned actors*/
  pthread_mutex_t  work_avail_lock;
  pthread_cond_t   work_avail_cond; /* workers sleep here when idle         */
  int64_t          shutdown;        /* flag, checked by workers             */
  mc_active_list   active;          /* actors currently executing           */
  pthread_t        monitor_thread;  /* heartbeat monitor                    */
  pthread_mutex_t  monitor_lock;    /* protects monitor condvar             */
  pthread_cond_t   monitor_cond;    /* woken on shutdown for fast exit      */
} mc_executor;

/* Singleton executor — initialised by mc_executor_init(). */
static mc_executor *g_executor = NULL;

/* Random peer selection for stealing. */
static uint32_t mc_xorshift32(uint32_t *state) {
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  *state = x;
  return x;
}

/* ── Worker thread entry point ─────────────────────────────────────────── */

static void *mc_worker_loop(void *arg) {
  mc_worker_ctx *ctx = (mc_worker_ctx *)arg;
  mc_executor *ex = (mc_executor *)ctx->executor;
  int64_t my_id = ctx->id;
  mc_deque *my_deque = &ex->deques[my_id];

  /* Seed the PRNG with something unique per worker. */
  uint32_t rng = (uint32_t)(my_id + 1) * 2654435761u;

  while (1) {
    /* Check shutdown. */
    if (__atomic_load_n(&ex->shutdown, __ATOMIC_ACQUIRE))
      break;

    /* 1. Try own deque. */
    mc_actor *actor = mc_deque_pop(my_deque);

    /* 2. Try the global inject queue. */
    if (!actor)
      actor = mc_deque_steal(&ex->inject_queue);

    /* 3. Try stealing from a random peer. */
    if (!actor && ex->num_workers > 1) {
      int64_t victim = (int64_t)(mc_xorshift32(&rng) % (uint32_t)ex->num_workers);
      if (victim == my_id)
        victim = (victim + 1) % ex->num_workers;
      actor = mc_deque_steal(&ex->deques[victim]);
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
      actor = mc_deque_steal(&ex->inject_queue);
      if (actor) {
        pthread_mutex_unlock(&ex->work_avail_lock);
      } else {
        pthread_cond_wait(&ex->work_avail_cond, &ex->work_avail_lock);
        pthread_mutex_unlock(&ex->work_avail_lock);
        continue; /* loop back and try again */
      }
    }

    /* ── Execute the actor ──────────────────────────────────────────── */
    actor->last_cycle = mc_monotonic_now();
    actor->reduction_count = 0;
    mc_active_list_add(&ex->active, actor);

    /* Track the final status locally — only published under the lock
       after cleanup so that waiters never see a terminal status while
       the arena is still alive. */
    int64_t final_status;

    /* Set status to RUNNING *before* setjmp so the actor can read it. */
    __atomic_store_n(&actor->status, MC_ACTOR_RUNNING, __ATOMIC_RELEASE);

    if (setjmp(actor->trap) == 0) {
      /* Normal path: run the actor's entry function. */
      actor->entry(actor);
      /* Default to COMPLETED.  context_exit may have set a different
         status — we capture whatever is current. */
      final_status = __atomic_load_n(&actor->status, __ATOMIC_ACQUIRE);
      if (final_status == MC_ACTOR_RUNNING)
        final_status = MC_ACTOR_COMPLETED;
    } else {
      /* longjmp path — actor was killed, trapped, or exited. */
      final_status = __atomic_load_n(&actor->status, __ATOMIC_ACQUIRE);
      if (final_status == MC_ACTOR_RUNNING) {
        /* If result_in_arena is set, context_exit was called — treat as
           completed.  Otherwise default to killed. */
        final_status = actor->result_in_arena ? MC_ACTOR_COMPLETED
                                              : MC_ACTOR_KILLED;
      }
    }

    /* Copy result out of arena BEFORE destroying it. */
    if (actor->result_in_arena && actor->result_size > 0
        && actor->result == NULL) {
      actor->result = malloc((size_t)actor->result_size);
      if (actor->result)
        memcpy(actor->result, actor->result_in_arena,
               (size_t)actor->result_size);
    }

    /* Tear down the arena. */
    mc_arena_destroy(actor->arena);
    actor->arena = NULL;

    /* Remove from active list before signalling waiters. */
    mc_active_list_remove(&ex->active, actor);

    /* Close the channel so any parent iterating via recv sees EOF. */
    if (actor->channel)
      mc_channel_close(actor->channel);

    /* Publish the final status and signal waiters under the lock.
       This guarantees that anyone waking from mc_task_wait() sees
       arena == NULL and result fully populated. */
    pthread_mutex_lock(&actor->lock);
    actor->status = final_status;
    pthread_cond_signal(&actor->done_cond);
    pthread_mutex_unlock(&actor->lock);

    /* Drop the worker's reference. */
    mc_actor_release(actor);
  }

  return NULL;
}

/* ── Public executor API ───────────────────────────────────────────────── */

/*
 * mc_executor_init  —  create the global executor and spawn worker threads.
 *
 * `num_workers` = 0 means auto-detect (hardware concurrency).
 * Must be called once before any mc_executor_spawn().
 */
/* ── Heartbeat monitor ──────────────────────────────────────────────────── */
/*                                                                          */
/* A dedicated thread that periodically scans all active actors.  If an     */
/* actor's last_cycle timestamp is older than MC_HEARTBEAT_TIMEOUT_MS, the  */
/* monitor sets its status to KILLED (status poisoning).  The actor will    */
/* exit on its next reduction tick or blocking channel operation.            */
/*                                                                          */
/* If the actor is STILL running after a second timeout period, the monitor */
/* assumes it is stuck in an FFI/syscall and logs a warning.  The worker    */
/* replacement mechanism (mc_executor_replace_worker) could be triggered    */
/* here in the future; for now we just poison and warn.                     */

static void *mc_heartbeat_monitor(void *arg) {
  mc_executor *ex = (mc_executor *)arg;
  int64_t timeout_ns = (int64_t)MC_HEARTBEAT_TIMEOUT_MS * 1000000LL;
  int64_t check_interval_ms = MC_HEARTBEAT_TIMEOUT_MS / 2;
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

    int64_t now = mc_monotonic_now();

    pthread_mutex_lock(&ex->active.lock);
    for (int64_t i = 0; i < ex->active.len; i++) {
      mc_actor *a = ex->active.actors[i];
      int64_t st = __atomic_load_n(&a->status, __ATOMIC_ACQUIRE);
      if (st != MC_ACTOR_RUNNING) continue;

      int64_t last = __atomic_load_n(&a->last_cycle, __ATOMIC_ACQUIRE);
      int64_t elapsed = now - last;

      if (elapsed > timeout_ns) {
        /* First timeout: poison the actor. */
        __atomic_store_n(&a->status, MC_ACTOR_KILLED, __ATOMIC_RELEASE);
        /* Also close the channel to unblock any waiting send. */
        if (a->channel)
          mc_channel_close(a->channel);
      }
    }
    pthread_mutex_unlock(&ex->active.lock);
  }

  return NULL;
}

void mc_executor_init(int64_t num_workers) {
  if (g_executor) return; /* already initialised */

  if (num_workers <= 0) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    num_workers = (n > 0) ? (int64_t)n : 4;
  }

  mc_executor *ex = (mc_executor *)calloc(1, sizeof(mc_executor));
  ex->num_workers = num_workers;
  ex->shutdown    = 0;

  pthread_mutex_init(&ex->work_avail_lock, NULL);
  pthread_cond_init(&ex->work_avail_cond, NULL);

  mc_deque_init(&ex->inject_queue);

  ex->deques = (mc_deque *)calloc((size_t)num_workers, sizeof(mc_deque));
  for (int64_t i = 0; i < num_workers; i++)
    mc_deque_init(&ex->deques[i]);

  ex->worker_ctxs = (mc_worker_ctx *)calloc((size_t)num_workers,
                                             sizeof(mc_worker_ctx));
  ex->threads = (pthread_t *)calloc((size_t)num_workers, sizeof(pthread_t));

  mc_active_list_init(&ex->active);
  pthread_mutex_init(&ex->monitor_lock, NULL);
  pthread_cond_init(&ex->monitor_cond, NULL);

  g_executor = ex;

  for (int64_t i = 0; i < num_workers; i++) {
    ex->worker_ctxs[i].id = i;
    ex->worker_ctxs[i].executor = ex;
    pthread_create(&ex->threads[i], NULL, mc_worker_loop,
                   &ex->worker_ctxs[i]);
  }

  /* Start the heartbeat monitor. */
  pthread_create(&ex->monitor_thread, NULL, mc_heartbeat_monitor, ex);
}

/*
 * mc_executor_spawn  —  create a new actor and schedule it for execution.
 *
 * Returns a pointer to the actor (the parent's Task handle).  The caller
 * owns one reference; the worker holds the other.
 */
mc_actor *mc_executor_spawn(void (*entry)(mc_actor *), void *closure_data,
                            int64_t closure_size, int64_t arena_max) {
  if (!g_executor) return NULL;

  mc_actor *actor = mc_actor_new(entry, closure_data, closure_size, arena_max);
  if (!actor) return NULL;

  mc_deque_push(&g_executor->inject_queue, actor);

  /* Wake one idle worker. */
  pthread_mutex_lock(&g_executor->work_avail_lock);
  pthread_cond_signal(&g_executor->work_avail_cond);
  pthread_mutex_unlock(&g_executor->work_avail_lock);

  return actor;
}

/*
 * mc_executor_schedule  —  push an already-created actor into the executor.
 *
 * Use this when you need to configure the actor (e.g. attach a channel)
 * between creation and scheduling.  The actor must have been created with
 * mc_actor_new().
 */
void mc_executor_schedule(mc_actor *actor) {
  if (!g_executor || !actor) return;
  mc_deque_push(&g_executor->inject_queue, actor);
  pthread_mutex_lock(&g_executor->work_avail_lock);
  pthread_cond_signal(&g_executor->work_avail_cond);
  pthread_mutex_unlock(&g_executor->work_avail_lock);
}

/*
 * mc_executor_shutdown  —  signal all workers to stop and join them.
 */
void mc_executor_shutdown(void) {
  if (!g_executor) return;
  mc_executor *ex = g_executor;

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
    mc_deque_destroy(&ex->deques[i]);
  mc_deque_destroy(&ex->inject_queue);
  mc_active_list_destroy(&ex->active);

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
 * mc_executor_replace_worker  —  abandon a stuck worker and spawn a
 * replacement.  The old thread is detached (leaked); its deque is drained
 * into the inject queue first, and a fresh deque is allocated for the
 * replacement so there is no contention.
 */
void mc_executor_replace_worker(int64_t worker_id) {
  if (!g_executor) return;
  mc_executor *ex = g_executor;
  if (worker_id < 0 || worker_id >= ex->num_workers) return;

  /* Drain stranded actors into the global inject queue. */
  mc_deque_drain(&ex->deques[worker_id], &ex->inject_queue);

  /* Destroy the old deque and create a fresh one. */
  mc_deque_destroy(&ex->deques[worker_id]);
  mc_deque_init(&ex->deques[worker_id]);

  /* Detach the stuck thread — it keeps running but we no longer join it. */
  pthread_detach(ex->threads[worker_id]);

  /* Spawn a replacement. */
  ex->worker_ctxs[worker_id].id = worker_id;
  ex->worker_ctxs[worker_id].executor = ex;
  pthread_create(&ex->threads[worker_id], NULL, mc_worker_loop,
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
/* mc_actor* that mc_executor_spawn() returned (the "Task handle").         */
/* ───────────────────────────────────────────────────────────────────────── */

/*
 * mc_task_alive  —  returns 1 if the actor is still running or pending.
 */
int64_t mc_task_alive(mc_actor *a) {
  if (!a) return 0;
  pthread_mutex_lock(&a->lock);
  int alive = (a->status == MC_ACTOR_PENDING ||
               a->status == MC_ACTOR_RUNNING);
  pthread_mutex_unlock(&a->lock);
  return alive ? 1 : 0;
}

/*
 * mc_task_cancel  —  ask the actor to stop.  The actor must poll
 *                    mc_context_cancelled() to honour this.
 */
void mc_task_cancel(mc_actor *a) {
  if (!a) return;
  __atomic_store_n(&a->cancelled, 1, __ATOMIC_RELEASE);
}

/*
 * mc_task_term  —  immediately kill the actor.  If it is blocked on a
 *                  channel the channel is closed to unblock it; on its
 *                  next reduction tick or blocking call it will longjmp
 *                  out (status poisoning).
 */
void mc_task_term(mc_actor *a) {
  if (!a) return;
  pthread_mutex_lock(&a->lock);
  if (a->status == MC_ACTOR_PENDING || a->status == MC_ACTOR_RUNNING) {
    a->status = MC_ACTOR_KILLED;
    if (a->channel)
      mc_channel_close(a->channel);
  }
  /* If the actor already finished, term is a no-op. */
  pthread_cond_signal(&a->done_cond);
  pthread_mutex_unlock(&a->lock);
}

/*
 * mc_task_wait  —  block until the actor reaches a terminal status.
 *                  Returns a pointer to the result (malloc'd, outlives
 *                  the arena) or NULL if no result was set.
 *
 *                  `out_status` is set to the terminal status if non-NULL.
 */
void *mc_task_wait(mc_actor *a, int64_t *out_status) {
  if (!a) {
    if (out_status) *out_status = MC_ACTOR_KILLED;
    return NULL;
  }
  pthread_mutex_lock(&a->lock);
  while (a->status < MC_ACTOR_COMPLETED)
    pthread_cond_wait(&a->done_cond, &a->lock);
  int64_t st = a->status;
  pthread_mutex_unlock(&a->lock);

  if (out_status) *out_status = st;
  return a->result;
}

/*
 * mc_task_drop  —  release the parent's reference.  Called when the Task
 *                  handle goes out of scope in generated code.
 */
void mc_task_drop(mc_actor *a) {
  mc_actor_release(a);
}

/* ───────────────────────────────────────────────────────────────────────── */
/* Context API                                                              */
/*                                                                          */
/* Called from inside the spawned actor.  The actor receives a pointer to   */
/* its own mc_actor as the "context".                                       */
/* ───────────────────────────────────────────────────────────────────────── */

/*
 * mc_context_cancelled  —  returns 1 if the parent called mc_task_cancel.
 */
int64_t mc_context_cancelled(mc_actor *a) {
  if (!a) return 0;
  return __atomic_load_n(&a->cancelled, __ATOMIC_ACQUIRE) ? 1 : 0;
}

/*
 * mc_context_exit  —  terminate the actor with a return value.
 *
 * `value` must point to data inside the actor's arena (or stack).
 * The worker loop copies it to a malloc'd buffer before destroying
 * the arena.
 *
 * Does not return — calls longjmp.
 */
void mc_context_exit(mc_actor *a, void *value, int64_t size) {
  if (!a) return;
  a->result_in_arena = value;
  a->result_size     = size;
  /* Do NOT set status here — the worker loop publishes the final status
     under the lock after arena cleanup.  We stash the desired status in
     a field the worker reads.  For context_exit the desired status is
     always COMPLETED; the worker detects this via result_in_arena being
     set and final_status still being RUNNING → it promotes to COMPLETED. */
  longjmp(a->trap, 1);
}

/*
 * mc_context_send  —  push a value into the actor's channel.
 *
 * Non-blocking from the actor's perspective unless the channel buffer is
 * full, in which case it blocks until a consumer drains an element.
 * Resets the reduction counter (I/O counts as yielding).
 */
int mc_context_send(mc_actor *a, const void *data) {
  if (!a || !a->channel) return -1;
  return mc_channel_send(a->channel, data, a);
}

/* ───────────────────────────────────────────────────────────────────────── */
/* Channel                                                                  */
/*                                                                          */
/* Fixed-size ring buffer for transferring data between actors.  Data is    */
/* copied (ownership transfer) — the sender retains no reference.           */
/*                                                                          */
/* mc_channel_send blocks if the buffer is full.                            */
/* mc_channel_recv blocks if the buffer is empty.                           */
/* mc_channel_close wakes all waiters; subsequent recv returns -1 once the  */
/* buffer drains.                                                           */
/* ───────────────────────────────────────────────────────────────────────── */

#ifndef MC_CHANNEL_DEFAULT_CAP
#define MC_CHANNEL_DEFAULT_CAP 64
#endif

struct mc_channel {
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
 * mc_channel_new  —  create a channel with a fixed ring buffer.
 *
 * `elem_size` is the byte size of each element.
 * `capacity`  is the max number of elements; 0 → MC_CHANNEL_DEFAULT_CAP.
 */
mc_channel *mc_channel_new(int64_t elem_size, int64_t capacity) {
  if (elem_size <= 0) return NULL;
  if (capacity <= 0) capacity = MC_CHANNEL_DEFAULT_CAP;

  mc_channel *ch = (mc_channel *)calloc(1, sizeof(mc_channel));
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
 * mc_channel_send  —  copy `data` into the channel (blocking if full).
 *
 * `actor` may be NULL (for sends from the main thread).  When non-NULL the
 * function checks status poisoning so a killed actor unblocks immediately.
 *
 * Returns  0 on success, -1 if the channel is closed.
 */
int mc_channel_send(mc_channel *ch, const void *data, mc_actor *actor) {
  if (!ch || !data) return -1;

  pthread_mutex_lock(&ch->lock);

  /* Block while full — respect close and actor kill. */
  while (ch->count == ch->capacity && !ch->closed) {
    if (actor && __atomic_load_n(&actor->status, __ATOMIC_ACQUIRE)
                    == MC_ACTOR_KILLED) {
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
 * mc_channel_recv  —  read the next element into `out_buf` (blocking).
 *
 * Returns  0 on success.
 * Returns -1 if the channel is closed AND the buffer is drained (EOF).
 */
int mc_channel_recv(mc_channel *ch, void *out_buf) {
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
 * mc_channel_close  —  mark the channel closed, wake all waiters.
 *
 * Safe to call multiple times.  After close, send returns -1.  recv
 * continues to drain buffered data, then returns -1.
 */
void mc_channel_close(mc_channel *ch) {
  if (!ch) return;

  pthread_mutex_lock(&ch->lock);
  ch->closed = 1;
  pthread_cond_broadcast(&ch->not_empty);
  pthread_cond_broadcast(&ch->not_full);
  pthread_mutex_unlock(&ch->lock);
}

/*
 * mc_channel_destroy  —  free all channel resources.
 *
 * The channel must already be closed and no threads may be blocked on it.
 */
void mc_channel_destroy(mc_channel *ch) {
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
} mc_string;

/* ───────────────────────────────────────────────────────────────────────── */
/* String refcounting                                                       */
/* ───────────────────────────────────────────────────────────────────────── */

void mc_retain_string(mc_string *s) {
  if (s && s->refcount > 0)
    s->refcount++;
}

void mc_release_string(mc_string *s) {
  if (!s || s->refcount < 0) return;   /* static — never free */
  s->refcount--;
  if (s->refcount <= 0) {
    free((void *)s->data);
    free(s);
  }
}

static mc_string *mc_alloc_string(const char *buf, int64_t len) {
  char *heap = (char *)malloc((size_t)len);
  if (len > 0) memcpy(heap, buf, (size_t)len);
  mc_string *s = (mc_string *)malloc(sizeof(mc_string));
  s->data = heap;
  s->len = len;
  s->refcount = 1;
  return s;
}

/* ───────────────────────────────────────────────────────────────────────── */
/* String helpers                                                           */
/* ───────────────────────────────────────────────────────────────────────── */

mc_string *mc_string_concat(const mc_string *a, const mc_string *b) {
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

  mc_string *result = (mc_string *)malloc(sizeof(mc_string));
  result->data = buf;
  result->len = new_len;
  result->refcount = 1;
  return result;
}

int64_t mc_string_compare(const mc_string *a, const mc_string *b) {
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

mc_string *mc_int_to_string(int64_t val) {
  char buf[32];
  int n = snprintf(buf, sizeof(buf), "%" PRId64, val);
  return mc_alloc_string(buf, n);
}

mc_string *mc_float_to_string(double val) {
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "%g", val);
  return mc_alloc_string(buf, n);
}

mc_string *mc_bool_to_string(int64_t val) {
  if (val) return mc_alloc_string("true", 4);
  return mc_alloc_string("false", 5);
}

/* ───────────────────────────────────────────────────────────────────────── */
/* Array                                                                    */
/*                                                                          */
/* Layout: { void *data, i64 len, i64 cap, i64 elem_size, i64 refcount }   */
/* ───────────────────────────────────────────────────────────────────────── */

typedef struct {
  void *data;
  int64_t len;
  int64_t cap;
  int64_t elem_size;
  int64_t refcount;
} mc_array;

/* ───────────────────────────────────────────────────────────────────────── */
/* Array refcounting                                                        */
/* ───────────────────────────────────────────────────────────────────────── */

void mc_retain_array(mc_array *arr) {
  if (arr && arr->refcount > 0)
    arr->refcount++;
}

void mc_release_array(mc_array *arr) {
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

mc_array *mc_array_new(int64_t elem_size, int64_t initial_cap) {
  if (initial_cap < 4) initial_cap = 4;
  mc_array *arr = (mc_array *)malloc(sizeof(mc_array));
  arr->data = malloc((size_t)(elem_size * initial_cap));
  arr->len = 0;
  arr->cap = initial_cap;
  arr->elem_size = elem_size;
  arr->refcount = 1;
  return arr;
}

void mc_array_push(mc_array *arr, const void *elem) {
  if (!arr) return;
  if (arr->len >= arr->cap) {
    arr->cap = arr->cap * 2;
    arr->data = realloc(arr->data, (size_t)(arr->elem_size * arr->cap));
  }
  memcpy((char *)arr->data + arr->elem_size * arr->len, elem,
         (size_t)arr->elem_size);
  arr->len++;
}

void *mc_array_at(mc_array *arr, int64_t index) {
  if (!arr || index < 0 || index >= arr->len) return NULL;
  return (char *)arr->data + arr->elem_size * index;
}

int64_t mc_array_size(mc_array *arr) {
  return arr ? arr->len : 0;
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
} mc_map_entry;

typedef struct {
  mc_map_entry *entries;    /* dense array, insertion order                 */
  int64_t *indices;         /* hash table: slot → index (-1 empty, -2 del) */
  int64_t len;              /* number of live entries                       */
  int64_t entries_cap;      /* capacity of entries[]                        */
  int64_t index_cap;        /* capacity of indices[] (always power of 2)   */
  int64_t key_size;
  int64_t val_size;
  int64_t refcount;
} mc_map;

#define MC_MAP_EMPTY    (-1)
#define MC_MAP_DELETED  (-2)

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

static uint64_t mc_sip_k0;
static uint64_t mc_sip_k1;

__attribute__((constructor))
static void mc_siphash_init_key(void) {
  uint8_t buf[16];
  /* getrandom() won't short-read for 16 bytes on Linux. */
  if (getrandom(buf, sizeof(buf), 0) != sizeof(buf)) {
    /* Fallback: mix address-space and time entropy.  Not great, but     */
    /* strictly better than a fixed constant and only hit if getrandom   */
    /* is somehow unavailable.                                           */
    mc_sip_k0 = (uint64_t)(uintptr_t)&mc_sip_k0 * 6364136223846793005ULL;
    mc_sip_k1 = (uint64_t)(uintptr_t)&mc_sip_k1 * 1442695040888963407ULL;
    return;
  }
  memcpy(&mc_sip_k0, buf,     8);
  memcpy(&mc_sip_k1, buf + 8, 8);
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

static uint64_t mc_siphash(const uint8_t *data, int64_t len) {
  uint64_t v0 = mc_sip_k0 ^ 0x736f6d6570736575ULL;
  uint64_t v1 = mc_sip_k1 ^ 0x646f72616e646f6dULL;
  uint64_t v2 = mc_sip_k0 ^ 0x6c7967656e657261ULL;
  uint64_t v3 = mc_sip_k1 ^ 0x7465646279746573ULL;

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

/* ── Key hashing / comparison helpers ──────────────────────────────────── */

static uint64_t mc_map_hash_key(const mc_map *m, const void *key,
                                int is_string_key) {
  if (is_string_key) {
    const mc_string *s = *(const mc_string *const *)key;
    if (!s || !s->data || s->len <= 0)
      return mc_siphash((const uint8_t *)"", 0);
    return mc_siphash((const uint8_t *)s->data, s->len);
  }
  return mc_siphash((const uint8_t *)key, m->key_size);
}

static int mc_map_keys_equal(const mc_map *m, const void *a, const void *b,
                             int is_string_key) {
  if (is_string_key) {
    const mc_string *sa = *(const mc_string *const *)a;
    const mc_string *sb = *(const mc_string *const *)b;
    if (sa == sb) return 1;
    if (!sa || !sb) return 0;
    if (sa->len != sb->len) return 0;
    if (sa->len == 0) return 1;
    return memcmp(sa->data, sb->data, (size_t)sa->len) == 0;
  }
  return memcmp(a, b, (size_t)m->key_size) == 0;
}

/* ── Internal: index table probing ─────────────────────────────────────── */

/* Find the index-table slot for `key`.                                     */
/* Returns the slot that is either empty, deleted, or holds a matching key. */
/* On match, *out_entry_idx is set to the entries[] index.                  */
/* On empty/deleted, *out_entry_idx is set to -1.                           */

static int64_t mc_map_probe(const mc_map *m, const void *key,
                            int is_string_key, int64_t *out_entry_idx) {
  uint64_t h = mc_map_hash_key(m, key, is_string_key);
  int64_t mask = m->index_cap - 1; /* index_cap is always a power of 2 */
  int64_t slot = (int64_t)(h & (uint64_t)mask);
  int64_t first_deleted = -1;

  for (int64_t i = 0; i < m->index_cap; i++) {
    int64_t s = (slot + i) & mask;
    int64_t eidx = m->indices[s];

    if (eidx == MC_MAP_EMPTY) {
      *out_entry_idx = -1;
      return (first_deleted >= 0) ? first_deleted : s;
    }
    if (eidx == MC_MAP_DELETED) {
      if (first_deleted < 0) first_deleted = s;
      continue;
    }
    /* Occupied — compare keys. */
    if (mc_map_keys_equal(m, m->entries[eidx].key, key, is_string_key)) {
      *out_entry_idx = eidx;
      return s;
    }
  }
  /* Table full (should never happen if load < 1). */
  *out_entry_idx = -1;
  return (first_deleted >= 0) ? first_deleted : -1;
}

/* ── Rebuild / resize ──────────────────────────────────────────────────── */

static void mc_map_rebuild_index(mc_map *m, int is_string_key) {
  /* Reset index table. */
  for (int64_t i = 0; i < m->index_cap; i++)
    m->indices[i] = MC_MAP_EMPTY;

  int64_t mask = m->index_cap - 1;
  for (int64_t ei = 0; ei < m->len; ei++) {
    uint64_t h = mc_map_hash_key(m, m->entries[ei].key, is_string_key);
    int64_t slot = (int64_t)(h & (uint64_t)mask);
    while (m->indices[slot] >= 0)
      slot = (slot + 1) & mask;
    m->indices[slot] = ei;
  }
}

static void mc_map_grow(mc_map *m, int is_string_key) {
  /* Double the entries capacity. */
  int64_t new_ecap = m->entries_cap * 2;
  if (new_ecap < 16) new_ecap = 16;
  m->entries = (mc_map_entry *)realloc(m->entries,
      (size_t)new_ecap * sizeof(mc_map_entry));
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
  mc_map_rebuild_index(m, is_string_key);
}

/* ── Refcounting ───────────────────────────────────────────────────────── */

void mc_retain_map(mc_map *m) {
  if (m && m->refcount > 0)
    m->refcount++;
}

void mc_release_map(mc_map *m) {
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

mc_map *mc_map_new(int64_t key_size, int64_t val_size) {
  int64_t initial_ecap = 8;
  int64_t initial_icap = 16; /* power of 2, ≥ 2 * initial_ecap */

  mc_map *m = (mc_map *)malloc(sizeof(mc_map));
  m->entries = (mc_map_entry *)malloc((size_t)initial_ecap * sizeof(mc_map_entry));
  m->indices = (int64_t *)malloc((size_t)initial_icap * sizeof(int64_t));
  for (int64_t i = 0; i < initial_icap; i++)
    m->indices[i] = MC_MAP_EMPTY;

  m->len = 0;
  m->entries_cap = initial_ecap;
  m->index_cap = initial_icap;
  m->key_size = (key_size < 0) ? (int64_t)sizeof(void *) : key_size;
  m->val_size = val_size;
  m->refcount = 1;
  return m;
}

void mc_map_set(mc_map *m, const void *key, const void *value,
                int64_t is_string_key) {
  if (!m) return;

  int64_t entry_idx;
  int64_t slot = mc_map_probe(m, key, (int)is_string_key, &entry_idx);

  if (entry_idx >= 0) {
    /* Key exists — update value in place (preserves insertion order). */
    memcpy(m->entries[entry_idx].value, value, (size_t)m->val_size);
    return;
  }

  /* New key — grow if entries array is full. */
  if (m->len >= m->entries_cap) {
    mc_map_grow(m, (int)is_string_key);
    /* Slot may have moved; re-probe. */
    slot = mc_map_probe(m, key, (int)is_string_key, &entry_idx);
  }
  /* Also check index load factor (> 2/3). */
  if ((m->len + 1) * 3 > m->index_cap * 2) {
    mc_map_grow(m, (int)is_string_key);
    slot = mc_map_probe(m, key, (int)is_string_key, &entry_idx);
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

void *mc_map_get(mc_map *m, const void *key, int64_t is_string_key) {
  if (!m || m->len == 0) return NULL;

  int64_t entry_idx;
  mc_map_probe(m, key, (int)is_string_key, &entry_idx);
  if (entry_idx < 0) return NULL;
  return m->entries[entry_idx].value;
}

int64_t mc_map_has(mc_map *m, const void *key, int64_t is_string_key) {
  return mc_map_get(m, key, is_string_key) != NULL ? 1 : 0;
}

void mc_map_remove(mc_map *m, const void *key, int64_t is_string_key) {
  if (!m || m->len == 0) return;

  int64_t entry_idx;
  int64_t slot = mc_map_probe(m, key, (int)is_string_key, &entry_idx);
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
    int64_t moved_slot = mc_map_probe(m, m->entries[entry_idx].key,
                                      (int)is_string_key, &moved_eidx);
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

  m->indices[slot] = MC_MAP_DELETED;
  m->len--;
}

int64_t mc_map_size(mc_map *m) {
  return m ? m->len : 0;
}

/* O(1) access by insertion index — the dense array IS the order. */
void *mc_map_key_at(mc_map *m, int64_t index) {
  if (!m || index < 0 || index >= m->len) return NULL;
  return m->entries[index].key;
}

void *mc_map_value_at(mc_map *m, int64_t index) {
  if (!m || index < 0 || index >= m->len) return NULL;
  return m->entries[index].value;
}

/* ───────────────────────────────────────────────────────────────────────── */
/* Intrinsics                                                               */
/* ───────────────────────────────────────────────────────────────────────── */

void mc_intrinsic_print(const mc_string *s) {
  if (s && s->data && s->len > 0) {
    fwrite(s->data, 1, (size_t)s->len, stdout);
  }
  fflush(stdout);
}

/* ───────────────────────────────────────────────────────────────────────── */
/* Arena-aware allocation helpers                                           */
/*                                                                          */
/* These mirror mc_alloc_string / mc_array_new / mc_map_new but allocate    */
/* from an arena instead of malloc.  The refcount is set to -1 (static /    */
/* never-freed) because the arena owns the memory — individual objects are  */
/* not freed; the entire arena is munmap'd at once.                         */
/* ───────────────────────────────────────────────────────────────────────── */

mc_string *mc_arena_alloc_string(mc_arena *a, const char *buf, int64_t len) {
  if (!a) return NULL;

  mc_string *s = (mc_string *)mc_arena_alloc(a, (int64_t)sizeof(mc_string));
  if (!s) return NULL;

  char *data = (char *)mc_arena_alloc(a, len > 0 ? len : 1);
  if (!data) return NULL;
  if (len > 0 && buf) memcpy(data, buf, (size_t)len);

  s->data     = data;
  s->len      = len;
  s->refcount = -1; /* arena-owned: never individually freed */
  return s;
}

mc_array *mc_arena_alloc_array(mc_arena *a, int64_t elem_size,
                               int64_t initial_cap) {
  if (!a) return NULL;
  if (initial_cap < 4) initial_cap = 4;

  mc_array *arr = (mc_array *)mc_arena_alloc(a, (int64_t)sizeof(mc_array));
  if (!arr) return NULL;

  void *data = mc_arena_alloc(a, elem_size * initial_cap);
  if (!data) return NULL;

  arr->data      = data;
  arr->len       = 0;
  arr->cap       = initial_cap;
  arr->elem_size = elem_size;
  arr->refcount  = -1; /* arena-owned */
  return arr;
}

mc_map *mc_arena_alloc_map(mc_arena *a, int64_t key_size, int64_t val_size) {
  if (!a) return NULL;

  int64_t initial_ecap = 8;
  int64_t initial_icap = 16;

  mc_map *m = (mc_map *)mc_arena_alloc(a, (int64_t)sizeof(mc_map));
  if (!m) return NULL;

  mc_map_entry *entries = (mc_map_entry *)mc_arena_alloc(
      a, initial_ecap * (int64_t)sizeof(mc_map_entry));
  if (!entries) return NULL;

  int64_t *indices = (int64_t *)mc_arena_alloc(
      a, initial_icap * (int64_t)sizeof(int64_t));
  if (!indices) return NULL;

  for (int64_t i = 0; i < initial_icap; i++)
    indices[i] = MC_MAP_EMPTY;

  m->entries     = entries;
  m->indices     = indices;
  m->len         = 0;
  m->entries_cap = initial_ecap;
  m->index_cap   = initial_icap;
  m->key_size    = (key_size < 0) ? (int64_t)sizeof(void *) : key_size;
  m->val_size    = val_size;
  m->refcount    = -1; /* arena-owned */
  return m;
}
