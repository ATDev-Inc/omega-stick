#pragma once

// Host-test stand-in for espp/base_component: a name plus a Logger.

#include <string>
#include <string_view>

#include "logger.hpp"

namespace espp {

class BaseComponent {
public:
  explicit BaseComponent(std::string_view name,
                         Logger::Verbosity level = Logger::Verbosity::WARN)
      : logger_({.tag = name, .level = level}) {}

  const std::string &get_name() const { return logger_.get_tag(); }
  void set_log_tag(std::string_view tag) { logger_.set_tag(tag); }
  Logger::Verbosity get_log_level() const { return logger_.get_verbosity(); }
  void set_log_level(Logger::Verbosity level) { logger_.set_verbosity(level); }

protected:
  Logger logger_;
};

} // namespace espp
