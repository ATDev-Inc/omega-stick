#include "json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace sim::json {

const std::vector<Value> Value::kNone{};

namespace {

void append_escaped(std::string &out, const std::string &s) {
  out.push_back('"');
  for (const char ch : s) {
    const auto c = static_cast<unsigned char>(ch);
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (c < 0x20) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
        out += buf;
      } else {
        out.push_back(ch);
      }
    }
  }
  out.push_back('"');
}

void append_number(std::string &out, double n) {
  if (!std::isfinite(n)) {
    out += "null";
    return;
  }
  char buf[40];
  if (n == std::floor(n) && std::fabs(n) < 1e15) {
    std::snprintf(buf, sizeof(buf), "%.0f", n);
  } else {
    std::snprintf(buf, sizeof(buf), "%.10g", n);
  }
  out += buf;
}

void dump_to(const Value &v, std::string &out) {
  switch (v.type()) {
  case Value::Type::Null:
    out += "null";
    break;
  case Value::Type::Bool:
    out += v.as_bool() ? "true" : "false";
    break;
  case Value::Type::Number:
    append_number(out, v.as_number());
    break;
  case Value::Type::String:
    append_escaped(out, v.as_string());
    break;
  case Value::Type::Array: {
    out.push_back('[');
    bool first = true;
    for (const Value &item : v.items()) {
      if (!first) {
        out.push_back(',');
      }
      first = false;
      dump_to(item, out);
    }
    out.push_back(']');
    break;
  }
  case Value::Type::Object: {
    out.push_back('{');
    for (size_t i = 0; i < v.keys().size(); i++) {
      if (i != 0) {
        out.push_back(',');
      }
      append_escaped(out, v.keys()[i]);
      out.push_back(':');
      dump_to(v.at_index(i), out);
    }
    out.push_back('}');
    break;
  }
  }
}

class Parser {
public:
  explicit Parser(std::string_view text)
      : s_(text) {}

  std::optional<Value> run(std::string *error) {
    skip_ws();
    std::optional<Value> v = parse_value(0);
    if (v) {
      skip_ws();
      if (pos_ != s_.size()) {
        fail("unexpected trailing characters");
        v.reset();
      }
    }
    if (!v && error) {
      *error = error_ + " at byte " + std::to_string(pos_);
    }
    return v;
  }

private:
  void fail(const char *msg) {
    if (error_.empty()) {
      error_ = msg;
    }
  }

  void skip_ws() {
    while (pos_ < s_.size() &&
           (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) {
      pos_++;
    }
  }

  bool consume(char c) {
    if (pos_ < s_.size() && s_[pos_] == c) {
      pos_++;
      return true;
    }
    return false;
  }

  bool consume_word(std::string_view w) {
    if (s_.substr(pos_, w.size()) == w) {
      pos_ += w.size();
      return true;
    }
    return false;
  }

  std::optional<Value> parse_value(int depth) {
    if (depth > kMaxDepth) {
      fail("nested too deeply");
      return std::nullopt;
    }
    if (pos_ >= s_.size()) {
      fail("unexpected end of input");
      return std::nullopt;
    }
    const char c = s_[pos_];
    if (c == '{') {
      return parse_object(depth);
    }
    if (c == '[') {
      return parse_array(depth);
    }
    if (c == '"') {
      std::string str;
      if (!parse_string(str)) {
        return std::nullopt;
      }
      return Value(std::move(str));
    }
    if (consume_word("true")) {
      return Value(true);
    }
    if (consume_word("false")) {
      return Value(false);
    }
    if (consume_word("null")) {
      return Value();
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
      return parse_number();
    }
    fail("unexpected character");
    return std::nullopt;
  }

  std::optional<Value> parse_number() {
    const size_t start = pos_;
    consume('-');
    if (pos_ >= s_.size() || !is_digit(s_[pos_])) {
      fail("bad number");
      return std::nullopt;
    }
    if (s_[pos_] == '0') {
      pos_++;
    } else {
      while (pos_ < s_.size() && is_digit(s_[pos_])) {
        pos_++;
      }
    }
    if (consume('.')) {
      if (pos_ >= s_.size() || !is_digit(s_[pos_])) {
        fail("bad number");
        return std::nullopt;
      }
      while (pos_ < s_.size() && is_digit(s_[pos_])) {
        pos_++;
      }
    }
    if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
      pos_++;
      if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) {
        pos_++;
      }
      if (pos_ >= s_.size() || !is_digit(s_[pos_])) {
        fail("bad number");
        return std::nullopt;
      }
      while (pos_ < s_.size() && is_digit(s_[pos_])) {
        pos_++;
      }
    }
    const std::string text(s_.substr(start, pos_ - start));
    return Value(std::strtod(text.c_str(), nullptr));
  }

  static bool is_digit(char c) { return c >= '0' && c <= '9'; }

  bool parse_hex4(unsigned &out) {
    if (pos_ + 4 > s_.size()) {
      fail("bad \\u escape");
      return false;
    }
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
      const char c = s_[pos_++];
      v <<= 4;
      if (c >= '0' && c <= '9') {
        v |= static_cast<unsigned>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        v |= static_cast<unsigned>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        v |= static_cast<unsigned>(c - 'A' + 10);
      } else {
        fail("bad \\u escape");
        return false;
      }
    }
    out = v;
    return true;
  }

  static void append_utf8(std::string &out, unsigned cp) {
    if (cp < 0x80) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
      out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
      out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool parse_string(std::string &out) {
    if (!consume('"')) {
      fail("expected string");
      return false;
    }
    while (pos_ < s_.size()) {
      const char c = s_[pos_++];
      if (c == '"') {
        return true;
      }
      if (static_cast<unsigned char>(c) < 0x20) {
        fail("control character in string");
        return false;
      }
      if (c != '\\') {
        out.push_back(c);
        continue;
      }
      if (pos_ >= s_.size()) {
        break;
      }
      const char e = s_[pos_++];
      switch (e) {
      case '"':
      case '\\':
      case '/':
        out.push_back(e);
        break;
      case 'b':
        out.push_back('\b');
        break;
      case 'f':
        out.push_back('\f');
        break;
      case 'n':
        out.push_back('\n');
        break;
      case 'r':
        out.push_back('\r');
        break;
      case 't':
        out.push_back('\t');
        break;
      case 'u': {
        unsigned cp = 0;
        if (!parse_hex4(cp)) {
          return false;
        }
        if (cp >= 0xD800 && cp <= 0xDBFF) {
          unsigned low = 0;
          if (!consume('\\') || !consume('u') || !parse_hex4(low) || low < 0xDC00 || low > 0xDFFF) {
            fail("bad surrogate pair");
            return false;
          }
          cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
          fail("lone surrogate");
          return false;
        }
        append_utf8(out, cp);
        break;
      }
      default:
        fail("bad escape");
        return false;
      }
    }
    fail("unterminated string");
    return false;
  }

  std::optional<Value> parse_array(int depth) {
    consume('[');
    Value arr = Value::array();
    skip_ws();
    if (consume(']')) {
      return arr;
    }
    while (true) {
      skip_ws();
      std::optional<Value> item = parse_value(depth + 1);
      if (!item) {
        return std::nullopt;
      }
      arr.push(std::move(*item));
      skip_ws();
      if (consume(',')) {
        continue;
      }
      if (consume(']')) {
        return arr;
      }
      fail("expected , or ]");
      return std::nullopt;
    }
  }

  std::optional<Value> parse_object(int depth) {
    consume('{');
    Value obj = Value::object();
    skip_ws();
    if (consume('}')) {
      return obj;
    }
    while (true) {
      skip_ws();
      std::string key;
      if (!parse_string(key)) {
        return std::nullopt;
      }
      skip_ws();
      if (!consume(':')) {
        fail("expected :");
        return std::nullopt;
      }
      skip_ws();
      std::optional<Value> val = parse_value(depth + 1);
      if (!val) {
        return std::nullopt;
      }
      obj.set(key, std::move(*val));
      skip_ws();
      if (consume(',')) {
        continue;
      }
      if (consume('}')) {
        return obj;
      }
      fail("expected , or }");
      return std::nullopt;
    }
  }

  std::string_view s_;
  size_t pos_{0};
  std::string error_;
};

} // namespace

std::string Value::dump() const {
  std::string out;
  dump_to(*this, out);
  return out;
}

std::optional<Value> parse(std::string_view text, std::string *error) {
  return Parser(text).run(error);
}

} // namespace sim::json
