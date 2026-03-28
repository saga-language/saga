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
