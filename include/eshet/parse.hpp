#pragma once
#include "data.hpp"

namespace eshet {
namespace detail {
std::pair<uint16_t, size_t> read16(const uint8_t *data, size_t size) {
  if (size < 2)
    throw ProtocolError();
  return {((uint16_t)data[0] << 8) + data[1], 2};
}

std::pair<std::string, size_t> read_string(const uint8_t *data, size_t size) {
  size_t length;
  for (length = 0; length < size; length++)
    if (data[length] == 0)
      break;

  if (length == size)
    throw ProtocolError();

  return {std::string((const char *)data, length), length + 1};
}

std::pair<msgpack::object_handle, size_t> read_msgpack(const uint8_t *data,
                                                       size_t size) {
  if (size < 1)
    throw ProtocolError();
  return {msgpack::unpack((char *)data, size), size};
}

std::tuple<> parse(const uint8_t *data, size_t size) {
  if (size > 0)
    throw ProtocolError();
  return std::make_tuple();
}

template <typename CB, typename... CBs>
auto parse(const uint8_t *data, size_t size, CB cb, CBs... cbs) {
  auto ret = cb(data, size);

  return std::tuple_cat(std::make_tuple(std::move(ret.first)),
                        parse(data + ret.second, size - ret.second, cbs...));
}
} // namespace detail
} // namespace eshet
