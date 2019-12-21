#pragma once
#include "eshet/callback_thread.hpp"
#include "eshet/data.hpp"
#include "eshet/parse.hpp"
#include "eshet/util.hpp"
#include <condition_variable>
#include <functional>
#include <future>
#include <iostream>
#include <map>
#include <mutex>
#include <netdb.h>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>

namespace eshet {

class ESHETClient {
public:
  ESHETClient(const std::string &hostname, int port)
      : hostname(hostname), port(port), sbuf(128),
        read_thread(&ESHETClient::read_thread_fn, this) {}

  ~ESHETClient() {
    disconnect();
    read_thread.join();
  }

  // register a callback to be called when the connection has been made
  void on_connect(std::function<void(void)> cb) {
    std::lock_guard<std::mutex> guard(conn_mut);

    // do this in the conn_mut mutex to ensure it doesn't get
    // called twice
    if (connected) {
      cb_thread.call_on_thread(cb);
    }

    connect_callbacks.emplace_back(std::move(cb));
  }

  std::future<void> wait_connected() {
    auto p = std::make_shared<std::promise<void>>();
    auto cb = [p, called = false]() mutable {
      if (!called) {
        p->set_value();
        called = true;
      }
    };

    on_connect(std::move(cb));

    return p->get_future();
  }

  // disconnect and wait for cleanup to finish
  void disconnect() {
    std::unique_lock<std::mutex> cv_guard(conn_mut);
    if (should_disconnect)
      return;
    should_disconnect = 1;

    if (sockfd >= 0) {
      shutdown(sockfd, SHUT_RDWR);
      close(sockfd);
      sockfd = -1;
      connected = false;
    }

    cv_guard.unlock();
    conn_cv.notify_one();
    cv_guard.lock();

    conn_cv.wait(cv_guard, [&] { return !connected; });
  }

  // register a state
  void state_register(const std::string &path, std::function<void(Result)> cb) {
    std::lock_guard<std::mutex> guard(send_mut);
    start_msg(0x40);
    uint16_t id = get_id();
    write16(id);
    write_string(path);
    add_reply_cb(id, std::move(cb));
    write_size();
    send_buf();
  }

  std::future<Success> state_register(const std::string &path) {
    return detail::wrap_promise<Success, Result>(
        [&](auto cb) { state_register(path, std::move(cb)); });
  }

  // notify observers that a registered state has changed
  template <typename T>
  void state_changed(const std::string &path, const T &value,
                     std::function<void(Result)> cb) {
    std::lock_guard<std::mutex> guard(send_mut);
    start_msg(0x41);
    uint16_t id = get_id();
    write16(id);
    write_string(path);
    write_msgpack(value);
    add_reply_cb(id, std::move(cb));
    write_size();
    send_buf();
  }

  template <typename T>
  std::future<Success> state_changed(const std::string &path, const T &value) {
    return detail::wrap_promise<Success, Result>(
        [&](auto cb) { state_changed(path, value, std::move(cb)); });
  }

  // notify observers that a registered state is unknown
  void state_unknown(const std::string &path, std::function<void(Result)> cb) {
    std::lock_guard<std::mutex> guard(send_mut);
    start_msg(0x42);
    uint16_t id = get_id();
    write16(id);
    write_string(path);
    add_reply_cb(id, std::move(cb));
    write_size();
    send_buf();
  }

  std::future<Success> state_unknown(const std::string &path) {
    return detail::wrap_promise<Success, Result>(
        [&](auto cb) { state_unknown(path, std::move(cb)); });
  }

  // observe a state. cb will be called with the state once during
  // registration, then once whenever it changes
  // XXX: should call cb when disconnected
  void state_observe(const std::string &path,
                     std::function<void(StateResult)> cb) {
    std::lock_guard<std::mutex> guard(send_mut);
    start_msg(0x43);
    uint16_t id = get_id();
    write16(id);
    write_string(path);
    add_reply_cb(id, cb);
    {
      std::lock_guard<std::mutex> guard(callbacks_mut);
      state_callbacks.emplace(std::make_pair(path, std::move(cb)));
    }
    write_size();
    send_buf();
  }

  void action_register(const std::string &path, ActionCB action_cb,
                       ResultCB register_cb) {
    std::lock_guard<std::mutex> guard(send_mut);
    start_msg(0x10);
    uint16_t id = get_id();
    write16(id);
    write_string(path);
    add_reply_cb(id, std::move(register_cb));
    {
      std::lock_guard<std::mutex> guard(callbacks_mut);
      action_callbacks.emplace(std::make_pair(path, std::move(action_cb)));
    }
    write_size();
    send_buf();
  }

  std::future<Success> action_register(const std::string &path,
                                       ActionCB action_cb) {
    return detail::wrap_promise<Success, Result>([&](auto cb) {
      action_register(path, std::move(action_cb), std::move(cb));
    });
  }

  template <typename... T>
  void action_call(const std::string &path, ResultCB cb, const T &... value) {
    std::lock_guard<std::mutex> guard(send_mut);
    start_msg(0x11);
    uint16_t id = get_id();
    write16(id);
    write_string(path);
    write_msgpack(std::make_tuple(value...));
    add_reply_cb(id, std::move(cb));
    write_size();
    send_buf();
  }

  template <typename... T>
  std::future<Success> action_call_promise(const std::string &path,
                                           const T &... value) {
    return detail::wrap_promise<Success, Result>(
        [&](auto cb) { action_call(path, std::move(cb), value...); });
  }

  void prop_register(const std::string &path, GetCB get_cb, SetCB set_cb,
                     ResultCB register_cb) {
    std::lock_guard<std::mutex> guard(send_mut);
    start_msg(0x20);
    uint16_t id = get_id();
    write16(id);
    write_string(path);
    add_reply_cb(id, std::move(register_cb));
    {
      std::lock_guard<std::mutex> guard(callbacks_mut);
      prop_callbacks.emplace(std::make_pair(
          path, std::make_pair(std::move(get_cb), std::move(set_cb))));
    }
    write_size();
    send_buf();
  }

  std::future<Success> prop_register(const std::string &path, GetCB get_cb,
                                     SetCB set_cb) {
    return detail::wrap_promise<Success, Result>([&](auto cb) {
      prop_register(path, std::move(get_cb), std::move(set_cb), std::move(cb));
    });
  }

  void get(const std::string &path, ResultCB cb) {
    std::lock_guard<std::mutex> guard(send_mut);
    start_msg(0x23);
    uint16_t id = get_id();
    write16(id);
    write_string(path);
    add_reply_cb(id, std::move(cb));
    write_size();
    send_buf();
  }

  std::future<Success> get(const std::string &path) {
    return detail::wrap_promise<Success, Result>(
        [&](auto cb) { get(path, std::move(cb)); });
  }

  template <typename T>
  void set(const std::string &path, const T &value, ResultCB cb) {
    std::lock_guard<std::mutex> guard(send_mut);
    start_msg(0x24);
    uint16_t id = get_id();
    write16(id);
    write_string(path);
    write_msgpack(value);
    add_reply_cb(id, std::move(cb));
    write_size();
    send_buf();
  }

  template <typename T>
  std::future<Success> set(const std::string &path, const T &value) {
    return detail::wrap_promise<Success, Result>(
        [&](auto cb) { set(path, value, std::move(cb)); });
  }

private:
  void read_thread_fn() {
    std::vector<uint8_t> msg_buf;
    while (true) {
      if (!ensure_connected())
        return;

      uint8_t header_buf[3];
      if (recv(sockfd, header_buf, sizeof(header_buf), MSG_WAITALL) !=
          sizeof(header_buf)) {
        std::lock_guard<std::mutex> guard(conn_mut);
        connected = false;
        continue;
      }

      if (header_buf[0] != 0x47) {
        std::lock_guard<std::mutex> guard(conn_mut);
        connected = false;
        continue;
      }

      uint16_t size = detail::read16(header_buf + 1, 2).first;

      msg_buf.resize(size);

      if (recv(sockfd, msg_buf.data(), size, MSG_WAITALL) != size) {
        std::lock_guard<std::mutex> guard(conn_mut);
        connected = false;
        continue;
      }

      handle_msg(msg_buf);
    }
  }

  void handle_msg(const std::vector<uint8_t> &msg) {
    using namespace detail;

    if (msg.size() < 1)
      throw ProtocolError();

    switch (msg[0]) {
    case 0x05: {
      // {reply, Id, {ok, Msg}}
      uint16_t id;
      msgpack::object_handle oh;
      std::tie(id, oh) = parse(&msg[1], msg.size() - 1, read16, read_msgpack);
      reply(id, Success(std::move(oh)));
    } break;
    case 0x06: {
      // {reply, Id, {error, Msg}}
      uint16_t id;
      msgpack::object_handle oh;
      std::tie(id, oh) = parse(&msg[1], msg.size() - 1, read16, read_msgpack);
      reply(id, Error(std::move(oh)));
    } break;
    case 0x07: {
      // {reply_state, Id, {known, Msg}}
      uint16_t id;
      msgpack::object_handle oh;
      std::tie(id, oh) = parse(&msg[1], msg.size() - 1, read16, read_msgpack);
      reply(id, Known(std::move(oh)));
    } break;
    case 0x08: {
      // {reply_state, Id, unknown}
      uint16_t id;
      std::tie(id) = parse(&msg[1], msg.size() - 1, read16);
      reply(id, Unknown());
    } break;
    case 0x11: {
      // {action_call, Id, Path, Msg}
      uint16_t id;
      std::string path;
      msgpack::object_handle oh;
      std::tie(id, path, oh) =
          parse(&msg[1], msg.size() - 1, read16, read_string, read_msgpack);

      std::unique_lock<std::mutex> callbacks_guard(callbacks_mut);
      auto it = action_callbacks.find(path);
      if (it == action_callbacks.end())
        // missing callback
        throw ProtocolError();

      Result r = it->second(std::move(oh));
      callbacks_guard.unlock();

      send_reply(id, std::move(r));
    } break;
    case 0x21: {
      // {prop_get, Id, Path}
      uint16_t id;
      std::string path;
      std::tie(id, path) = parse(&msg[1], msg.size() - 1, read16, read_string);

      std::unique_lock<std::mutex> callbacks_guard(callbacks_mut);
      auto it = prop_callbacks.find(path);
      if (it == prop_callbacks.end())
        // missing callback
        throw ProtocolError();

      Result r = it->second.first();
      callbacks_guard.unlock();

      send_reply(id, std::move(r));
    } break;
    case 0x22: {
      // {prop_set, Id, Path, Value}
      uint16_t id;
      std::string path;
      msgpack::object_handle oh;
      std::tie(id, path, oh) =
          parse(&msg[1], msg.size() - 1, read16, read_string, read_msgpack);

      std::unique_lock<std::mutex> callbacks_guard(callbacks_mut);
      auto it = prop_callbacks.find(path);
      if (it == prop_callbacks.end())
        // missing callback
        throw ProtocolError();

      Result r = it->second.second(std::move(oh));
      callbacks_guard.unlock();

      send_reply(id, std::move(r));
    } break;
    case 0x44: {
      // {state_changed, Path, {known, State}}
      std::string path;
      msgpack::object_handle oh;
      std::tie(path, oh) =
          parse(&msg[1], msg.size() - 1, read_string, read_msgpack);

      std::lock_guard<std::mutex> guard(callbacks_mut);
      auto it = state_callbacks.find(path);
      if (it == state_callbacks.end())
        // missing callback
        throw ProtocolError();
      it->second(Known(std::move(oh)));
    } break;
    case 0x45: {
      // {state_changed, Path, unknown}
      std::string path;
      std::tie(path) = parse(&msg[1], msg.size() - 1, read_string);

      std::lock_guard<std::mutex> guard(callbacks_mut);
      auto it = state_callbacks.find(path);
      if (it == state_callbacks.end())
        // missing callback
        throw ProtocolError();
      it->second(Unknown());
    } break;
    default: {
      assert(false); // unknown message
    } break;
    }
  }

  void reply(uint16_t id, AnyResult result) {
    std::unique_lock<std::mutex> guard(callbacks_mut);
    auto it = reply_callbacks.find(id);
    if (it == reply_callbacks.end())
      // missing callback
      throw ProtocolError();

    auto nh = reply_callbacks.extract(it);
    guard.unlock();

    if (!std::visit(
            [&](auto &cb) {
              return detail::convert_variant(std::move(result), cb);
            },
            nh.mapped()))
      // wrong type of return
      throw ProtocolError();
  }

  void send_reply(uint16_t id, Result r) {
    std::lock_guard<std::mutex> guard(send_mut);
    start_msg(std::holds_alternative<Success>(r) ? 0x05 : 0x06);
    write16(id);
    write_msgpack(std::visit([](const auto &x) { return x.value.get(); }, r));
    write_size();
    send_buf();
  }

  void do_disconnect(std::unique_lock<std::mutex> conn_mut_guard) {
    if (sockfd >= 0) {
      close(sockfd);
      sockfd = -1;
      connected = false;
      conn_mut_guard.unlock();
      conn_cv.notify_one();
    }
  }

  bool ensure_connected() {
    {
      std::unique_lock<std::mutex> guard(conn_mut);
      if (should_disconnect) {
        if (connected)
          do_disconnect(std::move(guard));
        return false;
      } else {
        if (connected)
          return true;
      }
    }

    std::chrono::seconds delay(1);
    std::chrono::seconds max_delay(30);

    while (true) {
      std::unique_lock<std::mutex> guard(conn_mut);
      if (connect_once()) {
        for (auto &cb : connect_callbacks) {
          cb_thread.call_on_thread(cb);
        }
        return true;
      }

      if (conn_cv.wait_for(guard, delay, [&] { return should_disconnect; }))
        return false;

      delay *= 2;
      if (delay > max_delay)
        delay = max_delay;
    }
  }

  // set up the connection; conn_mut must be held
  bool connect_once() {
    if (connected)
      return true;

    if (sockfd >= 0) {
      close(sockfd);
      sockfd = -1;
    }

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res;

    std::string port_str = std::to_string(port);
    int err = getaddrinfo(hostname.c_str(), port_str.c_str(), &hints, &res);

    if (err != 0 || res == NULL) {
      std::cerr << "eshet: dns lookup failed" << std::endl;
      return false;
    }

    sockfd = socket(res->ai_family, res->ai_socktype, 0);
    if (sockfd < 0) {
      std::cerr << "eshet: failed to allocate socket" << std::endl;
      freeaddrinfo(res);
      return false;
    }

    if (connect(sockfd, res->ai_addr, res->ai_addrlen) != 0) {
      std::cerr << "eshet: failed to connect" << std::endl;
      close(sockfd);
      sockfd = -1;
      freeaddrinfo(res);
      return false;
    }

    freeaddrinfo(res);

    std::cerr << "eshet: connected" << std::endl;

    connected = true;
    return true;
  }

private:
  uint16_t get_id() { return next_id++; }

  void start_msg(uint8_t type) {
    sbuf.clear();
    uint8_t header[] = {0x47, 0, 0, type};
    sbuf.write((char *)header, sizeof(header));
  }

  void write16(uint16_t value) {
    uint8_t data[] = {(uint8_t)(value >> 8), (uint8_t)(value & 0xff)};
    sbuf.write((char *)data, sizeof(data));
  }

  void write_string(const std::string &s) {
    sbuf.write(s.c_str(), s.size() + 1);
  }

  template <typename T> void write_msgpack(const T &value) {
    msgpack::pack(sbuf, value);
  }

  void write_size() {
    size_t size = sbuf.size() - 3;
    uint8_t size_fmt[] = {(uint8_t)(size >> 8), (uint8_t)(size & 0xff)};
    sbuf.data()[1] = size_fmt[0];
    sbuf.data()[2] = size_fmt[1];
  }

  void send_buf() {
    std::lock_guard<std::mutex> lock(conn_mut);
    if (send(sockfd, sbuf.data(), sbuf.size(), 0) < 0)
      throw Disconnected{};
  }

  void add_reply_cb(uint16_t id, ReplyCB cb) {
    std::lock_guard<std::mutex> guard(callbacks_mut);
    reply_callbacks.emplace(std::make_pair(id, std::move(cb)));
  }

  std::string hostname;
  int port;

  std::mutex send_mut;
  msgpack::sbuffer sbuf;
  uint16_t next_id = 0;

  std::mutex conn_mut;
  std::condition_variable conn_cv;
  int sockfd = -1;
  bool connected = false;
  bool should_disconnect = false;
  std::vector<std::function<void(void)>> connect_callbacks;

  std::mutex callbacks_mut;
  std::map<uint16_t, ReplyCB> reply_callbacks;
  std::map<std::string, StateResultCB> state_callbacks;
  std::map<std::string, ActionCB> action_callbacks;
  std::map<std::string, std::pair<GetCB, SetCB>> prop_callbacks;

  std::thread read_thread;

  CallbackThread cb_thread;
};

} // namespace eshet
