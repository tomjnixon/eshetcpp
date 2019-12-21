#pragma once
#include <condition_variable>
#include <functional>
#include <list>
#include <mutex>
#include <thread>

namespace eshet {

class CallbackThread {
public:
  CallbackThread() : thread(&CallbackThread::cb_thread_fn, this) {}

  ~CallbackThread() {
    {
      std::lock_guard<std::mutex> lock(mut);
      thread_exit = true;
    }
    cv.notify_all();
    thread.join();
  }

  void cb_thread_fn() {
    while (true) {
      std::unique_lock<std::mutex> lock(mut);

      cv.wait(lock, [&] { return thread_exit || !to_call.empty(); });

      while (!to_call.empty()) {
        lock.unlock();
        to_call.front()();
        lock.lock();

        to_call.pop_front();
      }

      if (thread_exit)
        return;
    }
  }

  void call_on_thread(std::function<void(void)> fn) {
    {
      std::lock_guard<std::mutex> lock(mut);
      to_call.emplace_back(std::move(fn));
    }
    cv.notify_all();
  }

private:
  std::mutex mut;
  std::condition_variable cv;
  std::list<std::function<void(void)>> to_call;
  bool thread_exit = false;

  std::thread thread;
};
} // namespace eshet
