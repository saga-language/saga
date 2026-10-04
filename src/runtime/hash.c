/* Copyright 2026 Rob Thornton
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>
#include <sys/random.h>

#include "runtime_internal.h"

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

uint64_t saga_runtime_siphash(const uint8_t *data, int64_t len) {
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
