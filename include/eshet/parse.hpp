#pragma once
#include "data.hpp"

namespace eshet {
namespace detail {
std::pair<uint16_t, size_t> read8(const uint8_t *data, size_t size) {
  if (size < 1)
    throw ProtocolError();
  return {data[0], 1};
}

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

// hold a buffer for message construction, with operations for writing various
// types of data
struct SendBuf {
  SendBuf(size_t size) :sbuf(size) {}

  void start_msg(uint8_t type) {
    sbuf.clear();
    uint8_t header[] = {0x47, 0, 0, type};
    sbuf.write((char *)header, sizeof(header));
  }

  void write8(uint8_t value) { sbuf.write((char *)&value, sizeof(value)); }

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

  msgpack::sbuffer sbuf;
};

} // namespace detail
} // namespace eshet
