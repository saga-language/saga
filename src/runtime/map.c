/* Copyright 2026 Rob Thornton
 * SPDX-License-Identifier: MIT
 */

#include <stdlib.h>
#include <string.h>

#include "runtime_internal.h"

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
/* Hash function: SipHash-1-3 (hash.c), resistant to hash flooding.         */
/* ───────────────────────────────────────────────────────────────────────── */

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

/* Shallow clone: new map with its own entry blocks, key and value bytes    */
/* copied.  Matches saga_map_equals: a pointer key or value is shared by     */
/* byte-copy, not deeply duplicated.                                        */
saga_runtime_map *saga_map_clone(const saga_runtime_map *src) {
  if (!src) return NULL;
  saga_runtime_map *dst = (saga_runtime_map *)malloc(sizeof(*dst));
  *dst = *src;
  dst->refcount = 1;
  dst->entries = (saga_runtime_map_entry *)malloc(
      (size_t)src->entries_cap * sizeof(saga_runtime_map_entry));
  dst->indices = (int64_t *)malloc((size_t)src->index_cap * sizeof(int64_t));
  memcpy(dst->indices, src->indices,
         (size_t)src->index_cap * sizeof(int64_t));
  for (int64_t i = 0; i < src->len; i++) {
    dst->entries[i].key = malloc((size_t)src->key_size);
    memcpy(dst->entries[i].key, src->entries[i].key, (size_t)src->key_size);
    dst->entries[i].value = malloc((size_t)src->val_size);
    memcpy(dst->entries[i].value, src->entries[i].value,
           (size_t)src->val_size);
  }
  return dst;
}

/* Copy-on-write: hand back a uniquely-owned map carrying a +1 reference for */
/* the caller.  Uniquely owned (refcount == 1) is retained in place; shared  */
/* (> 1) or arena-owned (-1) is cloned.                                     */
saga_runtime_map *saga_map_make_unique(saga_runtime_map *m) {
  if (!m) return NULL;
  if (m->refcount == 1) {
    saga_retain_map(m);
    return m;
  }
  return saga_map_clone(m);
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
