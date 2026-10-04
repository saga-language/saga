/* Copyright 2026 Rob Thornton
 * SPDX-License-Identifier: MIT
 *
 * Layouts and helpers the runtime's source files share. Each layout is also
 * the one codegen emits, so a field moves on both sides at once.
 */

#ifndef SAGA_RUNTIME_INTERNAL_H
#define SAGA_RUNTIME_INTERNAL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

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
} saga_runtime_array;

/* ───────────────────────────────────────────────────────────────────────── */
/* Map                                                                      */
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
/* Saga union ABI                                                            */
/*                                                                           */
/* Matches the LLVM struct `{ i8, [8 x i8] }` used by every union whose     */
/* largest alternative is 8 bytes (Int/Float/interface fat ptr).            */
/* Layout: 1 byte tag, 8 bytes raw payload, alignment 1.                     */
/*                                                                           */
/* Callers in Saga land allocate this struct as the sret slot for any        */
/* extern fn returning a union whose payload is 8 bytes.  If a new try-      */
/* style extern needs a wider payload, define a new `saga_union_<N>` rather  */
/* than widening this one — the static_asserts below pin the contract.       */
/* ───────────────────────────────────────────────────────────────────────── */

typedef struct {
  uint8_t tag;
  uint8_t payload[8];
} saga_union_8;

_Static_assert(sizeof(saga_union_8) == 9,
               "saga_union_8 must be 9 bytes to match LLVM { i8, [8 x i8] }");
_Static_assert(offsetof(saga_union_8, payload) == 1,
               "saga_union_8 payload must start at offset 1");
_Static_assert(_Alignof(saga_union_8) == 1,
               "saga_union_8 must have alignment 1 to match LLVM layout");

static inline void saga_union_8_set_i64(saga_union_8 *out, uint8_t tag, int64_t v) {
  out->tag = tag;
  memcpy(out->payload, &v, sizeof v);
}

static inline void saga_union_8_set_double(saga_union_8 *out, uint8_t tag, double v) {
  out->tag = tag;
  memcpy(out->payload, &v, sizeof v);
}

static inline void saga_union_8_set_ptr(saga_union_8 *out, uint8_t tag, void *v) {
  out->tag = tag;
  memcpy(out->payload, &v, sizeof v);
}

saga_runtime_string *saga_runtime_alloc_string(const char *buf, int64_t len);
void saga_retain_string(saga_runtime_string *s);
void saga_release_string(saga_runtime_string *s);
void *saga_missing_new(const char *msg, int64_t len);
uint64_t saga_runtime_siphash(const uint8_t *data, int64_t len);

void saga_retain_array(saga_runtime_array *arr);
void saga_release_array(saga_runtime_array *arr);
saga_runtime_array *saga_array_new_internal(int64_t elem_size,
                                            int64_t initial_cap);
void saga_array_push_internal(saga_runtime_array *arr, const void *elem);
saga_runtime_array *saga_array_clone(const saga_runtime_array *src);
saga_runtime_array *saga_array_make_unique(saga_runtime_array *arr);

#endif
