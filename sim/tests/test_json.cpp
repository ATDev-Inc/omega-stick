// The simulator's small JSON parser and writer.

#include <string>

#include "json.hpp"
#include "test_harness.hpp"

using sim::json::parse;
using sim::json::Value;

TEST(json_parses_the_control_message_shapes) {
  auto v = parse(R"({"type":"stick","x":0.4,"y":-0.1})");
  CHECK(v.has_value());
  CHECK_EQ(v->string_or("type"), std::string("stick"));
  CHECK_NEAR(v->number_or("x", 0), 0.4, 1e-12);
  CHECK_NEAR(v->number_or("y", 0), -0.1, 1e-12);
  CHECK_NEAR(v->number_or("missing", 7), 7, 0);
}

TEST(json_parses_scripts_with_nesting) {
  auto v = parse(R"( { "version": 1, "steps": [ {"at_ms": 0, "stick": [0, 0.5]},
      {"at_ms": 10, "expect": {"calibrated": true, "mode": "xac"}} ] } )");
  CHECK(v.has_value());
  const Value *steps = v->find("steps");
  CHECK(steps != nullptr && steps->is_array());
  CHECK_EQ(steps->items().size(), size_t{2});
  const Value &first = steps->items()[0];
  CHECK_EQ(first.find("stick")->items().size(), size_t{2});
  CHECK_NEAR(first.find("stick")->items()[1].as_number(), 0.5, 0);
  CHECK(steps->items()[1].find("expect")->bool_or("calibrated", false));
}

TEST(json_handles_numbers) {
  CHECK_NEAR(parse("0")->as_number(), 0, 0);
  CHECK_NEAR(parse("-12.5e2")->as_number(), -1250, 0);
  CHECK_NEAR(parse("1E-2")->as_number(), 0.01, 1e-15);
  CHECK(!parse("01").has_value());
  CHECK(!parse("1.").has_value());
  CHECK(!parse("-").has_value());
  CHECK(!parse(".5").has_value());
  CHECK(!parse("1e").has_value());
}

TEST(json_handles_string_escapes_and_unicode) {
  auto str = [](const char *text) {
    auto v = parse(text);
    CHECK(v.has_value());
    return v->as_string();
  };
  CHECK_EQ(str(R"("a\"b\\c\/d\n\t")"), std::string("a\"b\\c/d\n\t"));
  CHECK_EQ(str(R"("é")"), std::string("\xC3\xA9"));
  CHECK_EQ(str(R"("😀")"), std::string("\xF0\x9F\x98\x80"));
  CHECK(!parse(R"("\ud83d")").has_value()); // lone high surrogate
  CHECK(!parse(R"("\ude00")").has_value()); // lone low surrogate
  CHECK(!parse(R"("\q")").has_value());     // bad escape
  CHECK(!parse("\"a\nb\"").has_value());    // raw control character
  CHECK(!parse(R"("unterminated)").has_value());
}

TEST(json_rejects_malformed_documents) {
  for (const char *bad : {"", "{", "}", "[1,]", "{\"a\":1,}", "{\"a\" 1}", "{a:1}", "[1 2]", "tru",
                          "nul", "{\"a\":1} x", "'single'", "[", "{\"a\":"}) {
    std::string err;
    CHECK(!parse(bad, &err).has_value());
    CHECK(!err.empty());
  }
}

TEST(json_limits_nesting_depth) {
  std::string deep(200, '[');
  deep += std::string(200, ']');
  CHECK(!parse(deep).has_value());
  std::string fine(20, '[');
  fine += std::string(20, ']');
  CHECK(parse(fine).has_value());
}

TEST(json_duplicate_keys_keep_the_last_value) {
  auto v = parse(R"({"a":1,"a":2})");
  CHECK(v.has_value());
  CHECK_NEAR(v->number_or("a", 0), 2, 0);
  CHECK_EQ(v->size(), size_t{1});
}

TEST(json_dump_roundtrips) {
  Value v = Value::object();
  v.set("type", "hid").set("dx", 3).set("ok", true).set("x", 0.25).set("msg", "a\"b\n");
  Value arr = Value::array();
  arr.push(1).push("two").push(Value());
  v.set("list", arr);
  const std::string text = v.dump();
  CHECK_EQ(text,
           std::string(
               R"({"type":"hid","dx":3,"ok":true,"x":0.25,"msg":"a\"b\n","list":[1,"two",null]})"));
  auto back = parse(text);
  CHECK(back.has_value());
  CHECK_EQ(back->dump(), text);
}
