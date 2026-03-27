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
/* ───────────────────────────────────────────────────────────────────────── */

typedef struct {
  const char *data;
  int64_t len;
} mc_string;

/* ───────────────────────────────────────────────────────────────────────── */
/* String helpers                                                           */
/* ───────────────────────────────────────────────────────────────────────── */

/* Concatenate two strings, returning a newly heap-allocated mc_string.     */
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
  return result;
}

/* Compare two strings.  Returns <0, 0, or >0 like memcmp.                 */
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
  char *heap = (char *)malloc((size_t)n);
  memcpy(heap, buf, (size_t)n);

  mc_string *s = (mc_string *)malloc(sizeof(mc_string));
  s->data = heap;
  s->len = n;
  return s;
}

mc_string *mc_float_to_string(double val) {
  char buf[64];
  int n = snprintf(buf, sizeof(buf), "%g", val);
  char *heap = (char *)malloc((size_t)n);
  memcpy(heap, buf, (size_t)n);

  mc_string *s = (mc_string *)malloc(sizeof(mc_string));
  s->data = heap;
  s->len = n;
  return s;
}

mc_string *mc_bool_to_string(int64_t val) {
  const char *text = val ? "true" : "false";
  int64_t len = val ? 4 : 5;
  /* Return a pointer to static data — no allocation needed. */
  mc_string *s = (mc_string *)malloc(sizeof(mc_string));
  s->data = text;
  s->len = len;
  return s;
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
