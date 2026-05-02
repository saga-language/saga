/* Channel tests for the Saga runtime. */

#include "runtime_test_types.h"
#include <gtest/gtest.h>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>

// ─────────────────────────────────────────────────────────────────────────
// Basic lifecycle
// ─────────────────────────────────────────────────────────────────────────

TEST(ChannelTest, NewAndDestroy) {
  saga_runtime_channel *ch = saga_channel_new(sizeof(int64_t), 8);
  ASSERT_NE(ch, nullptr);
  saga_channel_close(ch);
  saga_channel_destroy(ch);
}

TEST(ChannelTest, NewInvalidElemSize) {
  EXPECT_EQ(saga_channel_new(0, 8), nullptr);
  EXPECT_EQ(saga_channel_new(-1, 8), nullptr);
}

TEST(ChannelTest, NewDefaultCapacity) {
  saga_runtime_channel *ch = saga_channel_new(sizeof(int64_t), 0);
  ASSERT_NE(ch, nullptr);
  saga_channel_close(ch);
  saga_channel_destroy(ch);
}

TEST(ChannelTest, DestroyNullIsSafe) {
  saga_channel_destroy(nullptr); // must not crash
}

TEST(ChannelTest, CloseNullIsSafe) {
  saga_channel_close(nullptr); // must not crash
}

// ─────────────────────────────────────────────────────────────────────────
// Single-threaded send/recv
// ─────────────────────────────────────────────────────────────────────────

TEST(ChannelTest, SendRecvSingleValue) {
  saga_runtime_channel *ch = saga_channel_new(sizeof(int64_t), 8);
  int64_t val = 42;
  EXPECT_EQ(saga_runtime_channel_send(ch, &val, nullptr), 0);

  int64_t out = 0;
  EXPECT_EQ(saga_channel_recv(ch, &out), 0);
  EXPECT_EQ(out, 42);

  saga_channel_close(ch);
  saga_channel_destroy(ch);
}

TEST(ChannelTest, SendRecvMultipleValues) {
  saga_runtime_channel *ch = saga_channel_new(sizeof(int64_t), 8);

  for (int64_t i = 1; i <= 5; i++)
    EXPECT_EQ(saga_runtime_channel_send(ch, &i, nullptr), 0);

  for (int64_t i = 1; i <= 5; i++) {
    int64_t out = 0;
    EXPECT_EQ(saga_channel_recv(ch, &out), 0);
    EXPECT_EQ(out, i);
  }

  saga_channel_close(ch);
  saga_channel_destroy(ch);
}

TEST(ChannelTest, FIFO_Order) {
  saga_runtime_channel *ch = saga_channel_new(sizeof(int64_t), 16);

  int64_t vals[] = {10, 20, 30, 40, 50};
  for (auto v : vals)
    saga_runtime_channel_send(ch, &v, nullptr);

  for (auto expected : vals) {
    int64_t out = 0;
    saga_channel_recv(ch, &out);
    EXPECT_EQ(out, expected);
  }

  saga_channel_close(ch);
  saga_channel_destroy(ch);
}

TEST(ChannelTest, LargerElements) {
  struct Payload { int64_t a; double b; char tag[8]; };
  saga_runtime_channel *ch = saga_channel_new(sizeof(Payload), 4);

  Payload p1 = {1, 3.14, "hello"};
  Payload p2 = {2, 2.71, "world"};
  saga_runtime_channel_send(ch, &p1, nullptr);
  saga_runtime_channel_send(ch, &p2, nullptr);

  Payload out = {};
  saga_channel_recv(ch, &out);
  EXPECT_EQ(out.a, 1);
  EXPECT_DOUBLE_EQ(out.b, 3.14);
  EXPECT_STREQ(out.tag, "hello");

  saga_channel_recv(ch, &out);
  EXPECT_EQ(out.a, 2);
  EXPECT_STREQ(out.tag, "world");

  saga_channel_close(ch);
  saga_channel_destroy(ch);
}

// ─────────────────────────────────────────────────────────────────────────
// Close semantics
// ─────────────────────────────────────────────────────────────────────────

TEST(ChannelTest, RecvAfterCloseEmpty) {
  saga_runtime_channel *ch = saga_channel_new(sizeof(int64_t), 8);
  saga_channel_close(ch);

  int64_t out = 0;
  EXPECT_EQ(saga_channel_recv(ch, &out), -1); // EOF
  saga_channel_destroy(ch);
}

TEST(ChannelTest, RecvDrainsBeforeEOF) {
  saga_runtime_channel *ch = saga_channel_new(sizeof(int64_t), 8);

  int64_t val = 99;
  saga_runtime_channel_send(ch, &val, nullptr);
  saga_channel_close(ch);

  int64_t out = 0;
  EXPECT_EQ(saga_channel_recv(ch, &out), 0); // buffered value
  EXPECT_EQ(out, 99);
  EXPECT_EQ(saga_channel_recv(ch, &out), -1); // now EOF

  saga_channel_destroy(ch);
}

TEST(ChannelTest, SendAfterCloseReturnsError) {
  saga_runtime_channel *ch = saga_channel_new(sizeof(int64_t), 8);
  saga_channel_close(ch);

  int64_t val = 1;
  EXPECT_EQ(saga_runtime_channel_send(ch, &val, nullptr), -1);
  saga_channel_destroy(ch);
}

TEST(ChannelTest, DoubleCloseIsSafe) {
  saga_runtime_channel *ch = saga_channel_new(sizeof(int64_t), 8);
  saga_channel_close(ch);
  saga_channel_close(ch); // must not crash or deadlock
  saga_channel_destroy(ch);
}

// ─────────────────────────────────────────────────────────────────────────
// Ring buffer wrap-around
// ─────────────────────────────────────────────────────────────────────────

TEST(ChannelTest, WrapAround) {
  /* Capacity 4: send 3, recv 3, send 4 more to force wrap. */
  saga_runtime_channel *ch = saga_channel_new(sizeof(int64_t), 4);

  for (int64_t i = 1; i <= 3; i++)
    saga_runtime_channel_send(ch, &i, nullptr);
  for (int i = 0; i < 3; i++) {
    int64_t out;
    saga_channel_recv(ch, &out);
  }

  /* Now head=3, tail=3. Send 4 more to wrap around. */
  for (int64_t i = 10; i <= 13; i++)
    saga_runtime_channel_send(ch, &i, nullptr);

  for (int64_t expected = 10; expected <= 13; expected++) {
    int64_t out = 0;
    EXPECT_EQ(saga_channel_recv(ch, &out), 0);
    EXPECT_EQ(out, expected);
  }

  saga_channel_close(ch);
  saga_channel_destroy(ch);
}

// ─────────────────────────────────────────────────────────────────────────
// Multi-threaded tests
// ─────────────────────────────────────────────────────────────────────────

TEST(ChannelTest, ProducerConsumer) {
  saga_runtime_channel *ch = saga_channel_new(sizeof(int64_t), 8);
  const int64_t N = 1000;

  /* Producer thread. */
  std::thread producer([&] {
    for (int64_t i = 0; i < N; i++)
      saga_runtime_channel_send(ch, &i, nullptr);
    saga_channel_close(ch);
  });

  /* Consumer in this thread. */
  int64_t sum = 0;
  int64_t count = 0;
  int64_t val;
  while (saga_channel_recv(ch, &val) == 0) {
    sum += val;
    count++;
  }

  producer.join();

  EXPECT_EQ(count, N);
  EXPECT_EQ(sum, N * (N - 1) / 2); // 0+1+2+...+(N-1)

  saga_channel_destroy(ch);
}

TEST(ChannelTest, MultipleProducers) {
  saga_runtime_channel *ch = saga_channel_new(sizeof(int64_t), 16);
  const int NUM_PRODUCERS = 4;
  const int64_t PER_PRODUCER = 250;
  const int64_t TOTAL = NUM_PRODUCERS * PER_PRODUCER;

  std::atomic<int> finished_producers{0};

  std::vector<std::thread> producers;
  for (int p = 0; p < NUM_PRODUCERS; p++) {
    producers.emplace_back([&, p] {
      for (int64_t i = 0; i < PER_PRODUCER; i++) {
        int64_t val = p * PER_PRODUCER + i;
        saga_runtime_channel_send(ch, &val, nullptr);
      }
      if (++finished_producers == NUM_PRODUCERS)
        saga_channel_close(ch);
    });
  }

  /* Consume concurrently — producers block on send if buffer is full. */
  int64_t count = 0;
  int64_t val;
  while (saga_channel_recv(ch, &val) == 0)
    count++;

  for (auto &t : producers) t.join();

  EXPECT_EQ(count, TOTAL);
  saga_channel_destroy(ch);
}

TEST(ChannelTest, RecvBlocksUntilSend) {
  saga_runtime_channel *ch = saga_channel_new(sizeof(int64_t), 4);
  std::atomic<bool> received{false};

  std::thread consumer([&] {
    int64_t val;
    saga_channel_recv(ch, &val); // blocks until producer sends
    received.store(true);
    EXPECT_EQ(val, 777);
  });

  /* Brief delay to ensure consumer is blocked. */
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(received.load());

  int64_t v = 777;
  saga_runtime_channel_send(ch, &v, nullptr);

  consumer.join();
  EXPECT_TRUE(received.load());

  saga_channel_close(ch);
  saga_channel_destroy(ch);
}

TEST(ChannelTest, SendBlocksWhenFull) {
  saga_runtime_channel *ch = saga_channel_new(sizeof(int64_t), 2);
  std::atomic<bool> send_returned{false};

  /* Fill the buffer. */
  int64_t a = 1, b = 2, c = 3;
  saga_runtime_channel_send(ch, &a, nullptr);
  saga_runtime_channel_send(ch, &b, nullptr);

  /* Third send should block. */
  std::thread producer([&] {
    saga_runtime_channel_send(ch, &c, nullptr); // blocks — buffer full
    send_returned.store(true);
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(send_returned.load());

  /* Consume one element to unblock the producer. */
  int64_t out;
  saga_channel_recv(ch, &out);
  EXPECT_EQ(out, 1);

  producer.join();
  EXPECT_TRUE(send_returned.load());

  /* Drain remaining. */
  saga_channel_recv(ch, &out);
  EXPECT_EQ(out, 2);
  saga_channel_recv(ch, &out);
  EXPECT_EQ(out, 3);

  saga_channel_close(ch);
  saga_channel_destroy(ch);
}

TEST(ChannelTest, CloseUnblocksRecv) {
  saga_runtime_channel *ch = saga_channel_new(sizeof(int64_t), 4);

  std::thread consumer([&] {
    int64_t val;
    int rc = saga_channel_recv(ch, &val); // blocks — empty
    EXPECT_EQ(rc, -1); // close wakes it with EOF
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  saga_channel_close(ch);

  consumer.join();
  saga_channel_destroy(ch);
}

TEST(ChannelTest, CloseUnblocksSend) {
  saga_runtime_channel *ch = saga_channel_new(sizeof(int64_t), 1);

  /* Fill the buffer. */
  int64_t v = 1;
  saga_runtime_channel_send(ch, &v, nullptr);

  std::thread producer([&] {
    int64_t val = 2;
    int rc = saga_runtime_channel_send(ch, &val, nullptr); // blocks — full
    EXPECT_EQ(rc, -1); // close wakes it
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  saga_channel_close(ch);

  producer.join();
  saga_channel_destroy(ch);
}

// ─────────────────────────────────────────────────────────────────────────
// Null / edge-case guards
// ─────────────────────────────────────────────────────────────────────────

TEST(ChannelTest, SendNullArgs) {
  saga_runtime_channel *ch = saga_channel_new(sizeof(int64_t), 8);
  EXPECT_EQ(saga_runtime_channel_send(nullptr, &ch, nullptr), -1);
  EXPECT_EQ(saga_runtime_channel_send(ch, nullptr, nullptr), -1);
  saga_channel_close(ch);
  saga_channel_destroy(ch);
}

TEST(ChannelTest, RecvNullArgs) {
  saga_runtime_channel *ch = saga_channel_new(sizeof(int64_t), 8);
  int64_t out;
  EXPECT_EQ(saga_channel_recv(nullptr, &out), -1);
  EXPECT_EQ(saga_channel_recv(ch, nullptr), -1);
  saga_channel_close(ch);
  saga_channel_destroy(ch);
}

// ─────────────────────────────────────────────────────────────────────────
// Iterator-style loop pattern (simulates `for msg : task {}`)
// ─────────────────────────────────────────────────────────────────────────

TEST(ChannelTest, IteratorPattern) {
  saga_runtime_channel *ch = saga_channel_new(sizeof(int64_t), 8);

  std::thread producer([&] {
    for (int64_t i = 10; i <= 15; i++)
      saga_runtime_channel_send(ch, &i, nullptr);
    saga_channel_close(ch);
  });

  std::vector<int64_t> received;
  int64_t val;
  while (saga_channel_recv(ch, &val) == 0)
    received.push_back(val);

  producer.join();

  ASSERT_EQ(received.size(), 6u);
  EXPECT_EQ(received[0], 10);
  EXPECT_EQ(received[5], 15);

  saga_channel_destroy(ch);
}
