#pragma once
#include <cstdio>
#include <memory>
#include <mutex>

namespace eshet {

struct LogCallbacks {
  virtual void debug(const std::string &s) {
    fprintf(stderr, "eshet: %s\n", s.c_str());
  }
  virtual void error(const std::string &s) {
    fprintf(stderr, "eshet: %s\n", s.c_str());
  }

  virtual ~LogCallbacks() {}
};

class Logger {
public:
  void set_log_callbacks(std::shared_ptr<LogCallbacks> new_log_callbacks) {
    std::lock_guard<std::mutex> guard(callbacks_mut);

    log_callbacks = new_log_callbacks;
  }

  void debug(const std::string &s) {
    std::lock_guard<std::mutex> guard(callbacks_mut);
    log_callbacks->debug(s);
  }

  void error(const std::string &s) {
    std::lock_guard<std::mutex> guard(callbacks_mut);
    log_callbacks->error(s);
  }

private:
  std::mutex callbacks_mut;
  std::shared_ptr<LogCallbacks> log_callbacks =
      std::make_shared<LogCallbacks>();
};

} // namespace eshet
