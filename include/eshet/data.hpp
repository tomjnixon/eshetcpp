#pragma once
#define MSGPACK_VREFBUFFER_HPP
#include "actorpp/actor.hpp"
#include "msgpack.hpp"
#include <functional>
#include <variant>

namespace eshet {
using namespace actorpp;
template <typename Base> struct HasMsgpackObject {
  msgpack::object_handle value;
  HasMsgpackObject(msgpack::object_handle value) : value(std::move(value)) {}
  HasMsgpackObject() {}

  template <typename T> HasMsgpackObject(T t) {
    value.set(msgpack::object(std::move(t)));
  }

  template <typename T> T as() {
    T v;
    value.get().convert(v);
    return v;
  }

  template <typename T> void convert(T v) { value.get().convert(v); }

  bool operator==(const HasMsgpackObject<Base> &other) const {
    return value.get() == other.value.get();
  }
  bool operator!=(const HasMsgpackObject<Base> &other) const {
    return !(*this == other);
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

  Error(const Error &other)
      : HasMsgpackObject<Error>(msgpack::clone(other.value.get())) {}
  Error(Error &&) = default;
  Error &operator=(Error &&) = default;

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

using AnyResult = std::variant<Success, Known, Unknown, Error>;

struct Call : public HasMsgpackObject<Call> {
  static constexpr const char *name = "Call";
  uint16_t connection_id;
  uint16_t id;

  Call(uint16_t connection_id, uint16_t id, msgpack::object_handle args,
       Channel<std::tuple<uint16_t, uint16_t, Result>> reply_chan)
      : HasMsgpackObject<Call>(std::move(args)), connection_id(connection_id),
        id(id), reply_chan(reply_chan) {}

  void reply(Result r) { reply_chan.emplace(connection_id, id, std::move(r)); }

  Channel<std::tuple<uint16_t, uint16_t, Result>> reply_chan;
};

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
} // namespace eshet
