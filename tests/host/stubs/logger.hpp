#pragma once

// Host-test stand-in for espp/logger. Mirrors the subset of espp::Logger the
// omega_calibration component uses: the Verbosity enum, Config, tag/level
// accessors, and the fmt-style debug/info/warn/error templates. Messages go
// to stderr through std::vformat so a mismatched placeholder shows up as
// text rather than a crash.

#include <chrono>
#include <cstdio>
#include <format>
#include <string>
#include <string_view>

namespace espp {

class Logger {
public:
  enum class Verbosity { DEBUG, INFO, WARN, ERROR, NONE };

  struct Config {
    std::string_view tag;
    bool include_time{true};
    std::chrono::duration<float> rate_limit{0};
    Verbosity level{Verbosity::WARN};
  };

  explicit Logger(const Config &config)
      : tag_(config.tag)
      , level_(config.level) {}

  const std::string &get_tag() const { return tag_; }
  void set_tag(std::string_view tag) { tag_ = std::string(tag); }
  Verbosity get_verbosity() const { return level_; }
  void set_verbosity(Verbosity level) { level_ = level; }

  template <typename... Args> void debug(std::string_view fmt, Args &&...args) const {
    log(Verbosity::DEBUG, "D", fmt, std::make_format_args(args...));
  }
  template <typename... Args> void info(std::string_view fmt, Args &&...args) const {
    log(Verbosity::INFO, "I", fmt, std::make_format_args(args...));
  }
  template <typename... Args> void warn(std::string_view fmt, Args &&...args) const {
    log(Verbosity::WARN, "W", fmt, std::make_format_args(args...));
  }
  template <typename... Args> void error(std::string_view fmt, Args &&...args) const {
    log(Verbosity::ERROR, "E", fmt, std::make_format_args(args...));
  }

private:
  void log(Verbosity level, const char *prefix, std::string_view fmt, std::format_args args) const {
    if (level < level_) {
      return;
    }
    std::string message;
    try {
      message = std::vformat(fmt, args);
    } catch (const std::format_error &e) {
      message = std::string(fmt) + " [format error: " + e.what() + "]";
    }
    std::fprintf(stderr, "[%s][%s] %s\n", prefix, tag_.c_str(), message.c_str());
  }

  std::string tag_;
  Verbosity level_;
};

} // namespace espp
