/* Copyright 2026 Rob Thornton
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>

#include "runtime_internal.h"

/* ───────────────────────────────────────────────────────────────────────── */
/* Array                                                                    */
/*                                                                          */
/* A counted, copy-on-write buffer of fixed-size elements. The layout is in */
/* runtime_internal.h.                                                      */
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
saga_runtime_array *saga_array_new_internal(int64_t elem_size, int64_t initial_cap) {
  if (initial_cap < 4) initial_cap = 4;
  saga_runtime_array *arr = (saga_runtime_array *)malloc(sizeof(saga_runtime_array));
  arr->data = malloc((size_t)(elem_size * initial_cap));
  arr->len = 0;
  arr->cap = initial_cap;
  arr->elem_size = elem_size;
  arr->refcount = 1;
  return arr;
}

void saga_array_push_internal(saga_runtime_array *arr, const void *elem) {
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

// Construction-only append: the caller has just allocated `arr` and holds the
// sole reference, so it is safe to grow in place with no copy-on-write.
void saga_array_builder_push(saga_runtime_array *arr, const void *elem) {
  saga_array_push_internal(arr, elem);
}

void *saga_array_at(saga_runtime_array *arr, int64_t index) {
  if (!arr || index < 0 || index >= arr->len) return NULL;
  return (char *)arr->data + arr->elem_size * index;
}

int64_t saga_array_size(saga_runtime_array *arr) {
  return arr ? arr->len : 0;
}

void saga_array_find(saga_union_8 *out, saga_runtime_array *arr,
                     const void *elem) {
  if (arr && elem) {
    for (int64_t i = 0; i < arr->len; i++) {
      void *cur = (char *)arr->data + arr->elem_size * i;
      if (memcmp(cur, elem, (size_t)arr->elem_size) == 0) {
        saga_union_8_set_i64(out, 0, i);
        return;
      }
    }
  }
  static const char msg[] = "element not found";
  saga_union_8_set_ptr(out, 1,
      saga_missing_new(msg, (int64_t)(sizeof msg - 1)));
}

saga_runtime_array *saga_array_insert(saga_runtime_array *arr,
                                      const void *elem, int64_t index) {
  arr = saga_array_make_unique(arr);
  if (!arr || !elem) return arr;
  if (index < 0) index = 0;
  if (index > arr->len) index = arr->len;
  if (arr->len >= arr->cap) {
    arr->cap = arr->cap > 0 ? arr->cap * 2 : 4;
    arr->data = realloc(arr->data, (size_t)(arr->elem_size * arr->cap));
  }
  char *base = (char *)arr->data;
  int64_t es = arr->elem_size;
  if (index < arr->len) {
    memmove(base + es * (index + 1), base + es * index,
            (size_t)(es * (arr->len - index)));
  }
  memcpy(base + es * index, elem, (size_t)es);
  arr->len++;
  return arr;
}

void *saga_array_pop(saga_runtime_array *arr) {
  if (!arr || arr->len == 0) return NULL;
  arr->len--;
  return (char *)arr->data + arr->elem_size * arr->len;
}

saga_runtime_array *saga_array_set(saga_runtime_array *arr, int64_t index,
                                   const void *elem) {
  arr = saga_array_make_unique(arr);
  if (!arr || !elem || index < 0 || index >= arr->len) return arr;
  memcpy((char *)arr->data + arr->elem_size * index, elem,
         (size_t)arr->elem_size);
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

/* Copy-on-write: hand back a uniquely-owned array carrying a +1 reference   */
/* for the caller.  Uniquely owned (refcount == 1) is retained in place;     */
/* shared (> 1) or static (-1, a rodata const) is cloned.                    */
saga_runtime_array *saga_array_make_unique(saga_runtime_array *arr) {
  if (!arr) return NULL;
  if (arr->refcount == 1) {
    saga_retain_array(arr);
    return arr;
  }
  return saga_array_clone(arr);
}

saga_runtime_array *saga_array_append(saga_runtime_array *arr,
                                      const void *elem) {
  arr = saga_array_make_unique(arr);
  if (!arr || !elem) return arr;
  if (arr->len >= arr->cap) {
    arr->cap = arr->cap > 0 ? arr->cap * 2 : 4;
    arr->data = realloc(arr->data, (size_t)(arr->elem_size * arr->cap));
  }
  memcpy((char *)arr->data + arr->elem_size * arr->len, elem,
         (size_t)arr->elem_size);
  arr->len++;
  return arr;
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
