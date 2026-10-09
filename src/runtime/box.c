/* Copyright 2026 Rob Thornton
 * SPDX-License-Identifier: MIT
 *
 * The interface box, a counted, copy-on-write heap copy of the value an
 * interface holds; and the shared box, a counted heap copy of a value nothing
 * writes through. The layouts are in runtime_internal.h.
 */

#include <stdlib.h>
#include <string.h>

#include "runtime_internal.h"

static void *box_value(saga_runtime_box *b) {
  return (char *)b + SAGA_RUNTIME_BOX_VALUE_OFFSET;
}

/* The value is left for the caller to place, and its references with it. */
saga_runtime_box *saga_box_new(const saga_runtime_vtable *vt) {
  saga_runtime_box *b = (saga_runtime_box *)malloc(
      (size_t)(SAGA_RUNTIME_BOX_VALUE_OFFSET + vt->size));
  b->refcount = 1;
  b->vtable = vt;
  return b;
}

void saga_box_retain(saga_runtime_box *b) {
  if (b && b->refcount > 0)
    b->refcount++;
}

void saga_box_release(saga_runtime_box *b) {
  if (!b || b->refcount < 0) return;
  if (--b->refcount > 0) return;
  if (b->vtable->ops)
    b->vtable->ops->release(box_value(b));
  free(b);
}

static saga_runtime_box *box_clone(saga_runtime_box *b) {
  saga_runtime_box *copy = saga_box_new(b->vtable);
  memcpy(box_value(copy), box_value(b), (size_t)b->vtable->size);
  if (b->vtable->ops)
    b->vtable->ops->retain(box_value(copy));
  return copy;
}

/* Ready a box for a call through its vtable, handing back a +1 reference */
/* for the caller. Method `method` writing through a box something else   */
/* shares gets a copy of its own; any other call keeps the box.           */
saga_runtime_box *saga_box_unique_for(saga_runtime_box *b, int64_t method) {
  if (!b) return NULL;
  int writes = (b->vtable->writes >> method) & 1;
  if (!writes || b->refcount == 1) {
    saga_box_retain(b);
    return b;
  }
  return box_clone(b);
}

static saga_runtime_shared *shared_header(void *value) {
  return (saga_runtime_shared *)((char *)value -
                                 SAGA_RUNTIME_SHARED_VALUE_OFFSET);
}

/* Zeroed for the caller to fill; the box takes over what it is given. */
void *saga_shared_new(int64_t size, const saga_runtime_elem_ops *ops) {
  saga_runtime_shared *h = (saga_runtime_shared *)calloc(
      1, (size_t)(SAGA_RUNTIME_SHARED_VALUE_OFFSET + size));
  h->refcount = 1;
  h->ops = ops;
  return (char *)h + SAGA_RUNTIME_SHARED_VALUE_OFFSET;
}

void saga_shared_retain(void *value) {
  if (!value) return;
  saga_runtime_shared *h = shared_header(value);
  if (h->refcount > 0)
    h->refcount++;
}

/* Freeing a box releases what it holds, which may free the next box, so a
 * long list would free itself one stack frame per node. A box whose count
 * reaches zero while another is being freed is queued instead, and the
 * outermost release frees the queue: the stack stays flat however deep the
 * value goes. */
static __thread struct {
  void **boxes;
  size_t len, cap;
  int freeing;
} pending;

static void queue_free(void *value) {
  if (pending.len == pending.cap) {
    pending.cap = pending.cap ? pending.cap * 2 : 64;
    pending.boxes = (void **)realloc(pending.boxes,
                                     pending.cap * sizeof(void *));
  }
  pending.boxes[pending.len++] = value;
}

static void shared_free(void *value) {
  saga_runtime_shared *h = shared_header(value);
  if (h->ops)
    h->ops->release(value);
  free(h);
}

void saga_shared_release(void *value) {
  if (!value) return;
  saga_runtime_shared *h = shared_header(value);
  if (h->refcount < 0 || --h->refcount > 0) return;
  if (pending.freeing) {
    queue_free(value);
    return;
  }
  pending.freeing = 1;
  shared_free(value);
  while (pending.len > 0)
    shared_free(pending.boxes[--pending.len]);
  pending.freeing = 0;
}
