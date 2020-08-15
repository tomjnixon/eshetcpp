#pragma once
#include "actorpp/actor.hpp"
#include "eshet/msgpack_to_string.hpp"
#include "msgpack.hpp"
#include <functional>
#include <variant>

namespace eshet {
using namespace actorpp;

/// convert an arbitrary value to an object_handle with a zone; this is
/// required if T corresponds to MessegePack format str, bin, ext, array, or
/// map. We should avoid using this automatically as it results in an
/// extra allocation compared to just constructing a msgpack::object
template <typename T> msgpack::object_handle oh_with_zone(const T &value) {
  std::unique_ptr<msgpack::zone> z = std::make_unique<msgpack::zone>();
  return msgpack::object_handle(msgpack::object(value, *z), std::move(z));
}

template <typename Base> struct HasMsgpackObject {
  msgpack::object_handle value;
  explicit HasMsgpackObject(msgpack::object_handle value)
      : value(std::move(value)) {}
  explicit HasMsgpackObject() {}

  template <typename T> explicit HasMsgpackObject(T t) {
    value.set(msgpack::object(std::move(t)));
  }

  template <typename T> T as() const {
    T v;
    value.get().convert(v);
    return v;
  }

  template <typename T> void convert(T v) const { value.get().convert(v); }

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
      error_str += "Error(";
      append_msgpack(error_str, value.get());
      error_str += ")";
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

struct Unknown {
  bool operator==(const Unknown &other) const { return true; }
  bool operator!=(const Unknown &other) const { return !(*this == other); }
};

using StateResult = std::variant<Known, Unknown, Error>;
using StateUpdate = std::variant<Known, Unknown>;

using AnyResult = std::variant<Success, Known, Unknown, Error>;

struct Call : public HasMsgpackObject<Call> {
  static constexpr const char *name = "Call";
  uint16_t connection_id;
  uint16_t id;

  explicit Call(uint16_t connection_id, uint16_t id,
                msgpack::object_handle args,
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
