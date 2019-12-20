#pragma once
#define MSGPACK_VREFBUFFER_HPP
#include "msgpack.hpp"
#include <condition_variable>
#include <functional>
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

template <typename Base> struct HasMsgpackObject {
  msgpack::object_handle value;
  HasMsgpackObject(msgpack::object_handle value) : value(std::move(value)) {}

  template <typename T> HasMsgpackObject(T t) {
    value.set(msgpack::object(std::move(t)));
  }

  template <typename T> T as() {
    T v;
    value.get().convert(v);
    return v;
  }
};

struct Success : public HasMsgpackObject<Success> {
  using HasMsgpackObject<Success>::HasMsgpackObject;
  static constexpr const char *name = "Success";
};

// Error is usable as an exception, so needs to be copyable
struct Error : public HasMsgpackObject<Error>, public std::exception {
  using HasMsgpackObject<Error>::HasMsgpackObject;
  static constexpr const char *name = "Error";

  Error(const Error &other) : HasMsgpackObject<Error>(other.value.get()) {}

  const char *what() const throw() {
    if (!error_str.size()) {
      std::ostringstream stream;
      stream << "Error(" << value.get() << ")";
      error_str = stream.str();
    }
    return error_str.c_str();
  }

private:
  mutable std::string error_str;
};

using Result = std::variant<Success, Error>;

struct Known : public HasMsgpackObject<Known> {
  using HasMsgpackObject<Known>::HasMsgpackObject;
  static constexpr const char *name = "Known";
};

struct Unknown {};

using StateResult = std::variant<Known, Unknown, Error>;

using ResultCB = std::function<void(Result)>;
using StateResultCB = std::function<void(StateResult)>;

using ActionCB = std::function<Result(msgpack::object_handle args)>;

using AnyResult = std::variant<Success, Known, Unknown, Error>;

using ReplyCB = std::variant<ResultCB, StateResultCB>;

// make these printable

template <typename Base>
std::ostream &operator<<(std::ostream &stream,
                         const HasMsgpackObject<Base> &value) {
  return stream << Base::name << "(" << value.value.get() << ")";
}

std::ostream &operator<<(std::ostream &stream, const Unknown &unknown) {
  return stream << "unknown";
}

template <typename T,
          typename = std::enable_if_t<std::is_same<T, Result>::value ||
                                      std::is_same<T, StateResult>::value ||
                                      std::is_same<T, AnyResult>::value>>
std::ostream &operator<<(std::ostream &stream, const T &result) {
  std::visit([&](const auto &v) { stream << v; }, result);
  return stream;
}

struct Disconnected : public std::exception {
  const char *what() const throw() { return "Disconnected"; }
};

struct ProtocolError : public std::exception {
  const char *what() const throw() { return "ProtocolError"; }
};

namespace detail {
template <typename In, typename Cb>
auto do_convert_variant(In in, Cb cb) -> decltype(cb(std::move(in)), bool()) {
  cb(std::move(in));
  return true;
}

bool do_convert_variant(...) { return false; }
} // namespace detail

// Helper to convert between variants holding the same type.
// If `in` holds something that `cb` can be called with, it will be, and the
// return value will be true; otherwise the return value will be false.
template <typename In, typename Cb> bool convert_variant(In in, Cb cb) {
  return std::visit(
      [&](auto x) { return detail::do_convert_variant(std::move(x), cb); },
      std::move(in));
}

class ESHETClient {
public:
  ESHETClient(const std::string &hostname, int port)
      : hostname(hostname), port(port), sbuf(128),
        thread(&ESHETClient::thread_fn, this) {}

  ~ESHETClient() {
    disconnect();
    thread.join();
  }

  // register a callback to be called when the connection has been made
  void on_connect(std::function<void(void)> cb) {
    std::lock_guard<std::mutex> guard(conn_mut);

    // do this in the conn_mut mutex to ensure it doesn't get
    // called twice
    if (connected) {
      cb();
    }

    connect_callbacks.emplace_back(std::move(cb));
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

  template <typename... T>
  void action_call(const std::string &path, ResultCB cb, const T &... value) {
    std::lock_guard<std::mutex> guard(send_mut);
    start_msg(0x11);
    uint16_t id = get_id();
    write16(id);
    write_string(path);
    write_msgpack(std::make_tuple(value...));
    add_reply_cb(id, cb);
    write_size();
    send_buf();
  }

private:
  void thread_fn() {
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

      uint16_t size = read16(header_buf + 1);

      msg_buf.resize(size);

      if (recv(sockfd, msg_buf.data(), size, MSG_WAITALL) != size) {
        std::lock_guard<std::mutex> guard(conn_mut);
        connected = false;
        continue;
      }

      handle_msg(msg_buf);
    }
  }

  uint16_t read16(const uint8_t *data) {
    return ((uint16_t)data[0] << 8) + data[1];
  }

  std::string read_string(const uint8_t *data, size_t size) {
    size_t length;
    for (length = 0; length < size; length++)
      if (data[length] == 0)
        break;

    if (length == size)
      throw ProtocolError();

    return std::string((const char *)data, length);
  }

  void handle_msg(const std::vector<uint8_t> &msg) {
    if (msg.size() < 1)
      throw ProtocolError();

    switch (msg[0]) {
    case 0x05: {
      // {reply, Id, {ok, Msg}}
      if (msg.size() < 3)
        throw ProtocolError();
      uint16_t id = read16(&msg[1]);
      reply(id, Success(msgpack::unpack((char *)&msg[3], msg.size() - 3)));
    } break;
    case 0x06: {
      // {reply, Id, {error, Msg}}
      if (msg.size() < 3)
        throw ProtocolError();
      uint16_t id = read16(&msg[1]);
      reply(id, Error(msgpack::unpack((char *)&msg[3], msg.size() - 3)));
    } break;
    case 0x07: {
      // {reply_state, Id, {known, Msg}}
      if (msg.size() < 3)
        throw ProtocolError();
      uint16_t id = read16(&msg[1]);
      reply(id, Known(msgpack::unpack((char *)&msg[3], msg.size() - 3)));
    } break;
    case 0x08: {
      // {reply_state, Id, unknown}
      if (msg.size() != 3)
        throw ProtocolError();
      uint16_t id = read16(&msg[1]);
      reply(id, Unknown());
    } break;
    case 0x11: {
      // {action_call, Id, Path, Msg}
      if (msg.size() < 4)
        throw ProtocolError();
      uint16_t id = read16(&msg[1]);
      std::string path = read_string(&msg[3], msg.size() - 3);
      size_t pos = 3 + path.size() + 1;
      msgpack::object_handle oh =
          msgpack::unpack((char *)&msg[pos], msg.size() - pos);

      std::unique_lock<std::mutex> callbacks_guard(callbacks_mut);
      auto it = action_callbacks.find(path);
      if (it == action_callbacks.end())
        // missing callback
        throw ProtocolError();

      Result r = it->second(std::move(oh));
      callbacks_guard.unlock();

      // reply
      std::lock_guard<std::mutex> guard(send_mut);
      start_msg(std::holds_alternative<Success>(r) ? 0x05 : 0x06);
      write16(id);
      write_msgpack(std::visit([](const auto &x) { return x.value.get(); }, r));
      write_size();
      send_buf();
    } break;
    case 0x44: {
      // {state_changed, Path, unknown}
      std::string path = read_string(&msg[1], msg.size() - 1);
      size_t pos = 1 + path.size() + 1;
      msgpack::object_handle oh =
          msgpack::unpack((char *)&msg[pos], msg.size() - pos);

      std::lock_guard<std::mutex> guard(callbacks_mut);
      auto it = state_callbacks.find(path);
      if (it == state_callbacks.end())
        // missing callback
        throw ProtocolError();
      it->second(Known(std::move(oh)));
    } break;
    case 0x45: {
      // {state_changed, Path, {known, State}}
      std::string path = read_string(&msg[1], msg.size() - 1);
      if (path.size() + 2 != msg.size())
        // space at the end
        throw ProtocolError();

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
            [&](auto &cb) { return convert_variant(std::move(result), cb); },
            nh.mapped()))
      // wrong type of return
      throw ProtocolError();
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
          cb();
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

  std::thread thread;
};
