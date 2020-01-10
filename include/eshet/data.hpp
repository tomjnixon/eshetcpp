#pragma once
#define MSGPACK_VREFBUFFER_HPP
#include "msgpack.hpp"
#include <functional>
#include <variant>

namespace eshet {
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

using GetCB = std::function<Result(void)>;
using SetCB = std::function<Result(msgpack::object_handle args)>;

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
} // namespace eshet
