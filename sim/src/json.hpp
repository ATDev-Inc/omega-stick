#pragma once

// A small JSON value, parser and writer for the simulator's control messages and
// scripts. Objects keep insertion order and look keys up linearly (messages are
// tiny). The parser accepts RFC 8259 JSON, nests at most kMaxDepth levels, and
// rejects trailing garbage.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sim::json {

class Value {
public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Value() = default;
  // cppcheck-suppress noExplicitConstructor ; implicit on purpose: Value v = 3, "x", true
  Value(bool b)
      : type_(Type::Bool)
      , bool_(b) {}
  // cppcheck-suppress noExplicitConstructor ; implicit on purpose: Value v = 3, "x", true
  Value(double n)
      : type_(Type::Number)
      , num_(n) {}
  // cppcheck-suppress noExplicitConstructor ; implicit on purpose: Value v = 3, "x", true
  Value(int n)
      : type_(Type::Number)
      , num_(n) {}
  // cppcheck-suppress noExplicitConstructor ; implicit on purpose: Value v = 3, "x", true
  Value(const char *s)
      : type_(Type::String)
      , str_(s) {}
  // cppcheck-suppress noExplicitConstructor ; implicit on purpose: Value v = 3, "x", true
  Value(std::string s)
      : type_(Type::String)
      , str_(std::move(s)) {}

  static Value array() {
    Value v;
    v.type_ = Type::Array;
    return v;
  }
  static Value object() {
    Value v;
    v.type_ = Type::Object;
    return v;
  }

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }
  bool is_bool() const { return type_ == Type::Bool; }
  bool is_number() const { return type_ == Type::Number; }
  bool is_string() const { return type_ == Type::String; }
  bool is_array() const { return type_ == Type::Array; }
  bool is_object() const { return type_ == Type::Object; }

  bool as_bool(bool def = false) const { return is_bool() ? bool_ : def; }
  double as_number(double def = 0.0) const { return is_number() ? num_ : def; }
  const std::string &as_string() const {
    static const std::string empty;
    return is_string() ? str_ : empty;
  }

  /// Array elements (empty for anything that is not an array).
  const std::vector<Value> &items() const { return is_array() ? values_ : kNone; }
  size_t size() const { return is_array() || is_object() ? values_.size() : 0; }

  /// The member `key` of an object, or nullptr.
  const Value *find(std::string_view key) const {
    if (!is_object()) {
      return nullptr;
    }
    for (size_t i = 0; i < keys_.size(); i++) {
      if (keys_[i] == key) {
        return &values_[i];
      }
    }
    return nullptr;
  }
  /// Member names of an object, in insertion order.
  const std::vector<std::string> &keys() const { return keys_; }
  const Value &at_index(size_t i) const { return values_[i]; }

  /// Convenience lookups that fall back to `def` for a missing or mistyped member.
  double number_or(std::string_view key, double def) const {
    const Value *v = find(key);
    return v && v->is_number() ? v->num_ : def;
  }
  std::string string_or(std::string_view key, const std::string &def = {}) const {
    const Value *v = find(key);
    return v && v->is_string() ? v->str_ : def;
  }
  bool bool_or(std::string_view key, bool def) const {
    const Value *v = find(key);
    return v && v->is_bool() ? v->bool_ : def;
  }

  /// Sets (or replaces) an object member. Turns a null value into an object.
  Value &set(const std::string &key, Value v) {
    if (type_ == Type::Null) {
      type_ = Type::Object;
    }
    for (size_t i = 0; i < keys_.size(); i++) {
      if (keys_[i] == key) {
        values_[i] = std::move(v);
        return *this;
      }
    }
    keys_.push_back(key);
    values_.push_back(std::move(v));
    return *this;
  }
  /// Appends to an array. Turns a null value into an array.
  Value &push(Value v) {
    if (type_ == Type::Null) {
      type_ = Type::Array;
    }
    values_.push_back(std::move(v));
    return *this;
  }

  /// Compact JSON text.
  std::string dump() const;

private:
  static const std::vector<Value> kNone;

  Type type_{Type::Null};
  bool bool_{false};
  double num_{0.0};
  std::string str_;
  std::vector<std::string> keys_;
  std::vector<Value> values_;
};

inline constexpr int kMaxDepth = 32;

/// Parses `text`. On failure returns nullopt and, when `error` is given, a short
/// description with the byte offset.
std::optional<Value> parse(std::string_view text, std::string *error = nullptr);

} // namespace sim::json
