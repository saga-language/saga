// Copyright 2026 Rob Thornton
// SPDX-License-Identifier: MIT

#include "util/json.hpp"

#include <gtest/gtest.h>

using json::Value;
using json::parse;
using json::obj;
using json::arr;

// ===========================================================================
// Parsing — primitive types
// ===========================================================================

TEST(JsonParse, Null) {
  Value v = parse("null");
  EXPECT_TRUE(v.is_null());
}

TEST(JsonParse, BoolTrue) {
  Value v = parse("true");
  ASSERT_TRUE(v.is_bool());
  EXPECT_TRUE(v.as_bool());
}

TEST(JsonParse, BoolFalse) {
  Value v = parse("false");
  ASSERT_TRUE(v.is_bool());
  EXPECT_FALSE(v.as_bool());
}

TEST(JsonParse, PositiveInt) {
  Value v = parse("42");
  ASSERT_TRUE(v.is_int());
  EXPECT_EQ(v.as_int(), 42);
}

TEST(JsonParse, NegativeInt) {
  Value v = parse("-7");
  ASSERT_TRUE(v.is_int());
  EXPECT_EQ(v.as_int(), -7);
}

TEST(JsonParse, Zero) {
  Value v = parse("0");
  ASSERT_TRUE(v.is_int());
  EXPECT_EQ(v.as_int(), 0);
}

TEST(JsonParse, Double) {
  Value v = parse("3.14");
  ASSERT_TRUE(v.is_double());
  EXPECT_DOUBLE_EQ(v.as_double(), 3.14);
}

TEST(JsonParse, DoubleExponent) {
  Value v = parse("1.5e2");
  ASSERT_TRUE(v.is_double());
  EXPECT_DOUBLE_EQ(v.as_double(), 150.0);
}

TEST(JsonParse, SimpleString) {
  Value v = parse(R"("hello")");
  ASSERT_TRUE(v.is_string());
  EXPECT_EQ(v.as_string(), "hello");
}

TEST(JsonParse, StringEscapes) {
  Value v = parse(R"("line1\nline2\ttab\"quote\\back")");
  ASSERT_TRUE(v.is_string());
  EXPECT_EQ(v.as_string(), "line1\nline2\ttab\"quote\\back");
}

TEST(JsonParse, EmptyString) {
  Value v = parse(R"("")");
  ASSERT_TRUE(v.is_string());
  EXPECT_TRUE(v.as_string().empty());
}

// ===========================================================================
// Parsing — arrays
// ===========================================================================

TEST(JsonParse, EmptyArray) {
  Value v = parse("[]");
  ASSERT_TRUE(v.is_array());
  EXPECT_EQ(v.size(), 0u);
}

TEST(JsonParse, IntArray) {
  Value v = parse("[1,2,3]");
  ASSERT_TRUE(v.is_array());
  ASSERT_EQ(v.size(), 3u);
  EXPECT_EQ(v.at(0).as_int(), 1);
  EXPECT_EQ(v.at(1).as_int(), 2);
  EXPECT_EQ(v.at(2).as_int(), 3);
}

TEST(JsonParse, MixedArray) {
  Value v = parse(R"([null, true, 42, "hi"])");
  ASSERT_TRUE(v.is_array());
  ASSERT_EQ(v.size(), 4u);
  EXPECT_TRUE(v.at(0).is_null());
  EXPECT_TRUE(v.at(1).as_bool());
  EXPECT_EQ(v.at(2).as_int(), 42);
  EXPECT_EQ(v.at(3).as_string(), "hi");
}

TEST(JsonParse, NestedArray) {
  Value v = parse("[[1,2],[3,4]]");
  ASSERT_TRUE(v.is_array());
  ASSERT_EQ(v.size(), 2u);
  EXPECT_EQ(v.at(0).at(0).as_int(), 1);
  EXPECT_EQ(v.at(1).at(1).as_int(), 4);
}

// ===========================================================================
// Parsing — objects
// ===========================================================================

TEST(JsonParse, EmptyObject) {
  Value v = parse("{}");
  ASSERT_TRUE(v.is_object());
  EXPECT_EQ(v.get("any"), nullptr);
}

TEST(JsonParse, SimpleObject) {
  Value v = parse(R"({"a":1,"b":"two"})");
  ASSERT_TRUE(v.is_object());
  ASSERT_NE(v.get("a"), nullptr);
  EXPECT_EQ(v.get("a")->as_int(), 1);
  ASSERT_NE(v.get("b"), nullptr);
  EXPECT_EQ(v.get("b")->as_string(), "two");
  EXPECT_EQ(v.get("c"), nullptr);
}

TEST(JsonParse, NestedObject) {
  Value v = parse(R"({"outer":{"inner":99}})");
  ASSERT_TRUE(v.is_object());
  auto *outer = v.get("outer");
  ASSERT_NE(outer, nullptr);
  ASSERT_TRUE(outer->is_object());
  auto *inner = outer->get("inner");
  ASSERT_NE(inner, nullptr);
  EXPECT_EQ(inner->as_int(), 99);
}

TEST(JsonParse, ObjectWithArray) {
  Value v = parse(R"({"nums":[10,20,30]})");
  auto *nums = v.get("nums");
  ASSERT_NE(nums, nullptr);
  ASSERT_TRUE(nums->is_array());
  EXPECT_EQ(nums->size(), 3u);
  EXPECT_EQ(nums->at(2).as_int(), 30);
}

TEST(JsonParse, WhitespaceHandled) {
  Value v = parse("  {  \"k\"  :  \"v\"  }  ");
  ASSERT_TRUE(v.is_object());
  ASSERT_NE(v.get("k"), nullptr);
  EXPECT_EQ(v.get("k")->as_string(), "v");
}

// ===========================================================================
// Parsing — real JSON-RPC shapes (as used by LSP/MCP)
// ===========================================================================

TEST(JsonParse, JsonRpcRequest) {
  Value v = parse(R"({
    "jsonrpc": "2.0",
    "id": 1,
    "method": "textDocument/hover",
    "params": {
      "textDocument": { "uri": "file:///foo.sg" },
      "position": { "line": 5, "character": 12 }
    }
  })");
  ASSERT_TRUE(v.is_object());
  EXPECT_EQ(v.str("jsonrpc"), "2.0");
  EXPECT_EQ(v.integer("id"), 1);
  EXPECT_EQ(v.str("method"), "textDocument/hover");
  auto *params = v.get("params");
  ASSERT_NE(params, nullptr);
  auto *pos = params->get("position");
  ASSERT_NE(pos, nullptr);
  EXPECT_EQ(pos->integer("line"), 5);
  EXPECT_EQ(pos->integer("character"), 12);
}

// ===========================================================================
// Serialisation — dump()
// ===========================================================================

TEST(JsonDump, Null) {
  EXPECT_EQ(Value{}.dump(), "null");
}

TEST(JsonDump, BoolTrue) {
  EXPECT_EQ(Value(true).dump(), "true");
}

TEST(JsonDump, BoolFalse) {
  EXPECT_EQ(Value(false).dump(), "false");
}

TEST(JsonDump, Int) {
  EXPECT_EQ(Value(42).dump(), "42");
}

TEST(JsonDump, NegativeInt) {
  EXPECT_EQ(Value(-7).dump(), "-7");
}

TEST(JsonDump, String) {
  EXPECT_EQ(Value("hello").dump(), "\"hello\"");
}

TEST(JsonDump, StringEscapes) {
  // newlines and quotes must be escaped
  Value v(std::string("a\nb\"c"));
  EXPECT_EQ(v.dump(), "\"a\\nb\\\"c\"");
}

TEST(JsonDump, EmptyArray) {
  EXPECT_EQ(json::make_array().dump(), "[]");
}

TEST(JsonDump, IntArray) {
  auto v = arr({1, 2, 3});
  EXPECT_EQ(v.dump(), "[1,2,3]");
}

TEST(JsonDump, EmptyObject) {
  EXPECT_EQ(json::make_object().dump(), "{}");
}

TEST(JsonDump, SimpleObject) {
  auto v = obj({{"x", 1}, {"y", "hello"}});
  // Field order is insertion order.
  EXPECT_EQ(v.dump(), R"({"x":1,"y":"hello"})");
}

// ===========================================================================
// Round-trip (parse then dump then parse)
// ===========================================================================

static void round_trip(const std::string &input) {
  Value v = parse(input);
  std::string dumped = v.dump();
  Value v2 = parse(dumped);
  EXPECT_EQ(v2.dump(), dumped) << "input was: " << input;
}

TEST(JsonRoundTrip, Null)   { round_trip("null"); }
TEST(JsonRoundTrip, True)   { round_trip("true"); }
TEST(JsonRoundTrip, False)  { round_trip("false"); }
TEST(JsonRoundTrip, Int)    { round_trip("123"); }
TEST(JsonRoundTrip, String) { round_trip(R"("hello world")"); }
TEST(JsonRoundTrip, Array)  { round_trip("[1,2,3]"); }
TEST(JsonRoundTrip, Object) { round_trip(R"({"a":1,"b":true})"); }
TEST(JsonRoundTrip, Nested) { round_trip(R"({"x":[1,{"y":null}]})"); }

// ===========================================================================
// Value helpers (str, integer, boolean with defaults)
// ===========================================================================

TEST(JsonHelpers, StrDefault) {
  Value v = obj({{"name", "saga"}});
  EXPECT_EQ(v.str("name"), "saga");
  EXPECT_EQ(v.str("missing"), "");
  EXPECT_EQ(v.str("missing", "default"), "default");
}

TEST(JsonHelpers, IntegerDefault) {
  Value v = obj({{"n", 99}});
  EXPECT_EQ(v.integer("n"), 99);
  EXPECT_EQ(v.integer("missing"), 0);
  EXPECT_EQ(v.integer("missing", -1), -1);
}

TEST(JsonHelpers, BooleanDefault) {
  Value v = obj({{"flag", true}});
  EXPECT_TRUE(v.boolean("flag"));
  EXPECT_FALSE(v.boolean("missing"));
  EXPECT_TRUE(v.boolean("missing", true));
}

// ===========================================================================
// Builder helpers: obj() and arr()
// ===========================================================================

TEST(JsonBuilders, ObjFromInitList) {
  auto v = obj({
      {"jsonrpc", "2.0"},
      {"id",      42},
      {"result",  json::make_object()},
  });
  ASSERT_TRUE(v.is_object());
  EXPECT_EQ(v.str("jsonrpc"), "2.0");
  EXPECT_EQ(v.integer("id"),  42);
  ASSERT_NE(v.get("result"), nullptr);
  EXPECT_TRUE(v.get("result")->is_object());
}

TEST(JsonBuilders, ArrFromInitList) {
  auto v = arr({"a", "b", "c"});
  ASSERT_TRUE(v.is_array());
  ASSERT_EQ(v.size(), 3u);
  EXPECT_EQ(v.at(1).as_string(), "b");
}

TEST(JsonBuilders, PushToArray) {
  auto v = json::make_array();
  v.push(Value(1));
  v.push(Value(2));
  EXPECT_EQ(v.size(), 2u);
  EXPECT_EQ(v.at(0).as_int(), 1);
}

TEST(JsonBuilders, SetObject) {
  auto v = json::make_object();
  v.set("a", Value(1));
  v.set("b", Value("hello"));
  ASSERT_NE(v.get("a"), nullptr);
  EXPECT_EQ(v.get("a")->as_int(), 1);
  // Overwrite existing key.
  v.set("a", Value(99));
  EXPECT_EQ(v.get("a")->as_int(), 99);
}
