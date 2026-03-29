/* Channel tests for the Saga runtime. */

#include <gtest/gtest.h>
#include <cstring>
#include <csetjmp>
#include <pthread.h>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>

extern "C" {

/* ── Types from runtime.c ─────────────────────────────────────────────── */

typedef struct {
  char    *base;
  int64_t  offset;
  int64_t  committed;
  int64_t  reserved;
  int64_t  max_limit;
} mc_arena;

typedef struct mc_channel mc_channel;

enum {
  MC_ACTOR_PENDING   = 0,
  MC_ACTOR_RUNNING   = 1,
  MC_ACTOR_COMPLETED = 2,
  MC_ACTOR_CANCELLED = 3,
  MC_ACTOR_KILLED    = 4,
  MC_ACTOR_ZOMBIE    = 5
};

typedef struct mc_actor {
  int64_t          refcount;
  void            *result;
  int64_t          result_size;
  int64_t          status;
  int64_t          cancelled;
  pthread_mutex_t  lock;
  pthread_cond_t   done_cond;
  mc_arena        *arena;
  void           (*entry)(struct mc_actor *);
  void            *closure_data;
  int64_t          closure_size;
  int64_t          reduction_count;
  int64_t          last_cycle;
  mc_channel      *channel;
  jmp_buf          trap;
} mc_actor;

/* ── Functions under test ─────────────────────────────────────────────── */

mc_channel *mc_channel_new(int64_t elem_size, int64_t capacity);
int         mc_channel_send(mc_channel *ch, const void *data, mc_actor *actor);
int         mc_channel_recv(mc_channel *ch, void *out_buf);
void        mc_channel_close(mc_channel *ch);
void        mc_channel_destroy(mc_channel *ch);

} // extern "C"

// ─────────────────────────────────────────────────────────────────────────
// Basic lifecycle
// ─────────────────────────────────────────────────────────────────────────

TEST(ChannelTest, NewAndDestroy) {
  mc_channel *ch = mc_channel_new(sizeof(int64_t), 8);
  ASSERT_NE(ch, nullptr);
  mc_channel_close(ch);
  mc_channel_destroy(ch);
}

TEST(ChannelTest, NewInvalidElemSize) {
  EXPECT_EQ(mc_channel_new(0, 8), nullptr);
  EXPECT_EQ(mc_channel_new(-1, 8), nullptr);
}

TEST(ChannelTest, NewDefaultCapacity) {
  mc_channel *ch = mc_channel_new(sizeof(int64_t), 0);
  ASSERT_NE(ch, nullptr);
  mc_channel_close(ch);
  mc_channel_destroy(ch);
}

TEST(ChannelTest, DestroyNullIsSafe) {
  mc_channel_destroy(nullptr); // must not crash
}

TEST(ChannelTest, CloseNullIsSafe) {
  mc_channel_close(nullptr); // must not crash
}

// ─────────────────────────────────────────────────────────────────────────
// Single-threaded send/recv
// ─────────────────────────────────────────────────────────────────────────

TEST(ChannelTest, SendRecvSingleValue) {
  mc_channel *ch = mc_channel_new(sizeof(int64_t), 8);
  int64_t val = 42;
  EXPECT_EQ(mc_channel_send(ch, &val, nullptr), 0);

  int64_t out = 0;
  EXPECT_EQ(mc_channel_recv(ch, &out), 0);
  EXPECT_EQ(out, 42);

  mc_channel_close(ch);
  mc_channel_destroy(ch);
}

TEST(ChannelTest, SendRecvMultipleValues) {
  mc_channel *ch = mc_channel_new(sizeof(int64_t), 8);

  for (int64_t i = 1; i <= 5; i++)
    EXPECT_EQ(mc_channel_send(ch, &i, nullptr), 0);

  for (int64_t i = 1; i <= 5; i++) {
    int64_t out = 0;
    EXPECT_EQ(mc_channel_recv(ch, &out), 0);
    EXPECT_EQ(out, i);
  }

  mc_channel_close(ch);
  mc_channel_destroy(ch);
}

TEST(ChannelTest, FIFO_Order) {
  mc_channel *ch = mc_channel_new(sizeof(int64_t), 16);

  int64_t vals[] = {10, 20, 30, 40, 50};
  for (auto v : vals)
    mc_channel_send(ch, &v, nullptr);

  for (auto expected : vals) {
    int64_t out = 0;
    mc_channel_recv(ch, &out);
    EXPECT_EQ(out, expected);
  }

  mc_channel_close(ch);
  mc_channel_destroy(ch);
}

TEST(ChannelTest, LargerElements) {
  struct Payload { int64_t a; double b; char tag[8]; };
  mc_channel *ch = mc_channel_new(sizeof(Payload), 4);

  Payload p1 = {1, 3.14, "hello"};
  Payload p2 = {2, 2.71, "world"};
  mc_channel_send(ch, &p1, nullptr);
  mc_channel_send(ch, &p2, nullptr);

  Payload out = {};
  mc_channel_recv(ch, &out);
  EXPECT_EQ(out.a, 1);
  EXPECT_DOUBLE_EQ(out.b, 3.14);
  EXPECT_STREQ(out.tag, "hello");

  mc_channel_recv(ch, &out);
  EXPECT_EQ(out.a, 2);
  EXPECT_STREQ(out.tag, "world");

  mc_channel_close(ch);
  mc_channel_destroy(ch);
}

// ─────────────────────────────────────────────────────────────────────────
// Close semantics
// ─────────────────────────────────────────────────────────────────────────

TEST(ChannelTest, RecvAfterCloseEmpty) {
  mc_channel *ch = mc_channel_new(sizeof(int64_t), 8);
  mc_channel_close(ch);

  int64_t out = 0;
  EXPECT_EQ(mc_channel_recv(ch, &out), -1); // EOF
  mc_channel_destroy(ch);
}

TEST(ChannelTest, RecvDrainsBeforeEOF) {
  mc_channel *ch = mc_channel_new(sizeof(int64_t), 8);

  int64_t val = 99;
  mc_channel_send(ch, &val, nullptr);
  mc_channel_close(ch);

  int64_t out = 0;
  EXPECT_EQ(mc_channel_recv(ch, &out), 0); // buffered value
  EXPECT_EQ(out, 99);
  EXPECT_EQ(mc_channel_recv(ch, &out), -1); // now EOF

  mc_channel_destroy(ch);
}

TEST(ChannelTest, SendAfterCloseReturnsError) {
  mc_channel *ch = mc_channel_new(sizeof(int64_t), 8);
  mc_channel_close(ch);

  int64_t val = 1;
  EXPECT_EQ(mc_channel_send(ch, &val, nullptr), -1);
  mc_channel_destroy(ch);
}

TEST(ChannelTest, DoubleCloseIsSafe) {
  mc_channel *ch = mc_channel_new(sizeof(int64_t), 8);
  mc_channel_close(ch);
  mc_channel_close(ch); // must not crash or deadlock
  mc_channel_destroy(ch);
}

// ─────────────────────────────────────────────────────────────────────────
// Ring buffer wrap-around
// ─────────────────────────────────────────────────────────────────────────

TEST(ChannelTest, WrapAround) {
  /* Capacity 4: send 3, recv 3, send 4 more to force wrap. */
  mc_channel *ch = mc_channel_new(sizeof(int64_t), 4);

  for (int64_t i = 1; i <= 3; i++)
    mc_channel_send(ch, &i, nullptr);
  for (int i = 0; i < 3; i++) {
    int64_t out;
    mc_channel_recv(ch, &out);
  }

  /* Now head=3, tail=3. Send 4 more to wrap around. */
  for (int64_t i = 10; i <= 13; i++)
    mc_channel_send(ch, &i, nullptr);

  for (int64_t expected = 10; expected <= 13; expected++) {
    int64_t out = 0;
    EXPECT_EQ(mc_channel_recv(ch, &out), 0);
    EXPECT_EQ(out, expected);
  }

  mc_channel_close(ch);
  mc_channel_destroy(ch);
}

// ─────────────────────────────────────────────────────────────────────────
// Multi-threaded tests
// ─────────────────────────────────────────────────────────────────────────

TEST(ChannelTest, ProducerConsumer) {
  mc_channel *ch = mc_channel_new(sizeof(int64_t), 8);
  const int64_t N = 1000;

  /* Producer thread. */
  std::thread producer([&] {
    for (int64_t i = 0; i < N; i++)
      mc_channel_send(ch, &i, nullptr);
    mc_channel_close(ch);
  });

  /* Consumer in this thread. */
  int64_t sum = 0;
  int64_t count = 0;
  int64_t val;
  while (mc_channel_recv(ch, &val) == 0) {
    sum += val;
    count++;
  }

  producer.join();

  EXPECT_EQ(count, N);
  EXPECT_EQ(sum, N * (N - 1) / 2); // 0+1+2+...+(N-1)

  mc_channel_destroy(ch);
}

TEST(ChannelTest, MultipleProducers) {
  mc_channel *ch = mc_channel_new(sizeof(int64_t), 16);
  const int NUM_PRODUCERS = 4;
  const int64_t PER_PRODUCER = 250;
  const int64_t TOTAL = NUM_PRODUCERS * PER_PRODUCER;

  std::atomic<int> finished_producers{0};

  std::vector<std::thread> producers;
  for (int p = 0; p < NUM_PRODUCERS; p++) {
    producers.emplace_back([&, p] {
      for (int64_t i = 0; i < PER_PRODUCER; i++) {
        int64_t val = p * PER_PRODUCER + i;
        mc_channel_send(ch, &val, nullptr);
      }
      if (++finished_producers == NUM_PRODUCERS)
        mc_channel_close(ch);
    });
  }

  /* Consume concurrently — producers block on send if buffer is full. */
  int64_t count = 0;
  int64_t val;
  while (mc_channel_recv(ch, &val) == 0)
    count++;

  for (auto &t : producers) t.join();

  EXPECT_EQ(count, TOTAL);
  mc_channel_destroy(ch);
}

TEST(ChannelTest, RecvBlocksUntilSend) {
  mc_channel *ch = mc_channel_new(sizeof(int64_t), 4);
  std::atomic<bool> received{false};

  std::thread consumer([&] {
    int64_t val;
    mc_channel_recv(ch, &val); // blocks until producer sends
    received.store(true);
    EXPECT_EQ(val, 777);
  });

  /* Brief delay to ensure consumer is blocked. */
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(received.load());

  int64_t v = 777;
  mc_channel_send(ch, &v, nullptr);

  consumer.join();
  EXPECT_TRUE(received.load());

  mc_channel_close(ch);
  mc_channel_destroy(ch);
}

TEST(ChannelTest, SendBlocksWhenFull) {
  mc_channel *ch = mc_channel_new(sizeof(int64_t), 2);
  std::atomic<bool> send_returned{false};

  /* Fill the buffer. */
  int64_t a = 1, b = 2, c = 3;
  mc_channel_send(ch, &a, nullptr);
  mc_channel_send(ch, &b, nullptr);

  /* Third send should block. */
  std::thread producer([&] {
    mc_channel_send(ch, &c, nullptr); // blocks — buffer full
    send_returned.store(true);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(send_returned.load());

  /* Consume one element to unblock the producer. */
  int64_t out;
  mc_channel_recv(ch, &out);
  EXPECT_EQ(out, 1);

  producer.join();
  EXPECT_TRUE(send_returned.load());

  /* Drain remaining. */
  mc_channel_recv(ch, &out);
  EXPECT_EQ(out, 2);
  mc_channel_recv(ch, &out);
  EXPECT_EQ(out, 3);

  mc_channel_close(ch);
  mc_channel_destroy(ch);
}

TEST(ChannelTest, CloseUnblocksRecv) {
  mc_channel *ch = mc_channel_new(sizeof(int64_t), 4);

  std::thread consumer([&] {
    int64_t val;
    int rc = mc_channel_recv(ch, &val); // blocks — empty
    EXPECT_EQ(rc, -1); // close wakes it with EOF
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  mc_channel_close(ch);

  consumer.join();
  mc_channel_destroy(ch);
}

TEST(ChannelTest, CloseUnblocksSend) {
  mc_channel *ch = mc_channel_new(sizeof(int64_t), 1);

  /* Fill the buffer. */
  int64_t v = 1;
  mc_channel_send(ch, &v, nullptr);

  std::thread producer([&] {
    int64_t val = 2;
    int rc = mc_channel_send(ch, &val, nullptr); // blocks — full
    EXPECT_EQ(rc, -1); // close wakes it
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  mc_channel_close(ch);

  producer.join();
  mc_channel_destroy(ch);
}

// ─────────────────────────────────────────────────────────────────────────
// Null / edge-case guards
// ─────────────────────────────────────────────────────────────────────────

TEST(ChannelTest, SendNullArgs) {
  mc_channel *ch = mc_channel_new(sizeof(int64_t), 8);
  EXPECT_EQ(mc_channel_send(nullptr, &ch, nullptr), -1);
  EXPECT_EQ(mc_channel_send(ch, nullptr, nullptr), -1);
  mc_channel_close(ch);
  mc_channel_destroy(ch);
}

TEST(ChannelTest, RecvNullArgs) {
  mc_channel *ch = mc_channel_new(sizeof(int64_t), 8);
  int64_t out;
  EXPECT_EQ(mc_channel_recv(nullptr, &out), -1);
  EXPECT_EQ(mc_channel_recv(ch, nullptr), -1);
  mc_channel_close(ch);
  mc_channel_destroy(ch);
}

// ─────────────────────────────────────────────────────────────────────────
// Iterator-style loop pattern (simulates `for msg : task {}`)
// ─────────────────────────────────────────────────────────────────────────

TEST(ChannelTest, IteratorPattern) {
  mc_channel *ch = mc_channel_new(sizeof(int64_t), 8);

  std::thread producer([&] {
    for (int64_t i = 10; i <= 15; i++)
      mc_channel_send(ch, &i, nullptr);
    mc_channel_close(ch);
  });

  std::vector<int64_t> received;
  int64_t val;
  while (mc_channel_recv(ch, &val) == 0)
    received.push_back(val);

  producer.join();

  ASSERT_EQ(received.size(), 6u);
  EXPECT_EQ(received[0], 10);
  EXPECT_EQ(received[5], 15);

  mc_channel_destroy(ch);
}
