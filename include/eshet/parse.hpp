#pragma once
#include "data.hpp"

namespace eshet {
namespace detail {

class Parser {
public:
  Parser(const uint8_t *data, size_t size) : data(data), size(size) {}

  uint8_t read8() {
    if (size - pos < 1)
      throw ProtocolError();
    return data[pos++];
  }

  uint16_t read16() {
    if (size - pos < 2)
      throw ProtocolError();
    uint16_t value = ((uint16_t)data[pos] << 8) + data[pos + 1];
    pos += 2;
    return value;
  }

  uint32_t read32() {
    if (size - pos < 4)
      throw ProtocolError();
    uint32_t value = ((uint32_t)data[pos] << 24) +
                     ((uint32_t)data[pos + 1] << 16) +
                     ((uint32_t)data[pos + 2] << 8) + data[pos + 3];
    pos += 4;
    return value;
  }

  std::string read_string() {
    size_t end;
    for (end = pos; end < size; end++)
      if (data[end] == 0)
        break;

    if (end == size)
      throw ProtocolError();

    std::string value{(const char *)data + pos, (const char *)data + end};
    pos = end + 1;

    return value;
  }

  msgpack::object_handle read_msgpack() {
    if (size - pos < 1)
      throw ProtocolError();
    msgpack::object_handle value =
        msgpack::unpack((char *)data + pos, size - pos);
    pos = size;
    return value;
  }

  void check_empty() {
    if (pos != size)
      throw ProtocolError();
  }

private:
  const uint8_t *data;
  size_t size;
  size_t pos = 0;
};

// hold a buffer for message construction, with operations for writing various
// types of data
struct SendBuf {
  explicit SendBuf(size_t size) : sbuf(size) {}

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

  // methods for writing common eshet command formats

  void write_path(uint8_t message, uint16_t id, const std::string &path) {
    start_msg(message);
    write16(id);
    write_string(path);
    write_size();
  }

  void write_pack(uint8_t message, uint16_t id, const msgpack::object &value) {
    start_msg(message);
    write16(id);
    write_msgpack(value);
    write_size();
  }

  void write_path_pack(uint8_t message, uint16_t id, const std::string &path,
                       const msgpack::object &value) {
    start_msg(message);
    write16(id);
    write_string(path);
    write_msgpack(value);
    write_size();
  }

  // methods for writing whole eshet commands

  void write_hello(const std::optional<msgpack::object_handle> &id,
                   uint16_t server_timeout) {
    start_msg(id ? 0x02 : 0x01);
    write8(1);
    write16(server_timeout);
    if (id)
      write_msgpack(**id);
    write_size();
  }

  void write_reply(uint16_t id, const Success &success) {
    write_pack(0x05, id, *success.value);
  }

  void write_reply(uint16_t id, const Error &error) {
    write_pack(0x06, id, *error.value);
  }

  void write_reply(uint16_t id, const Result &result) {
    std::visit([&](const auto &r) { write_reply(id, r); }, result);
  }

  void write_action_register(uint16_t id, const std::string &path) {
    write_path(0x10, id, path);
  }

  void write_action_call(uint16_t id, const std::string &path,
                         const msgpack::object &args) {
    write_path_pack(0x11, id, path, args);
  }

  void write_state_register(uint16_t id, const std::string &path) {
    write_path(0x40, id, path);
  }

  void write_state_observe(uint16_t id, const std::string &path) {
    write_path(0x46, id, path);
  }

  void write_state_changed(uint16_t id, const std::string &path,
                           const Known &state) {
    write_path_pack(0x41, id, path, *state.value);
  }

  void write_state_changed(uint16_t id, const std::string &path,
                           const Unknown &state) {
    write_path(0x42, id, path);
  }

  void write_state_changed(uint16_t id, const std::string &path,
                           const StateUpdate &state) {
    std::visit([&](const auto &s) { write_state_changed(id, path, s); }, state);
  }

  void write_event_register(uint16_t id, const std::string &path) {
    write_path(0x30, id, path);
  }

  void write_event_emit(uint16_t id, const std::string &path,
                        const msgpack::object &value) {
    write_path_pack(0x31, id, path, value);
  }

  void write_event_listen(uint16_t id, const std::string &path) {
    write_path(0x32, id, path);
  }

  void write_ping(uint16_t id) {
    start_msg(0x09);
    write16(id);
    write_size();
  }

  void write_get(uint16_t id, const std::string &path) {
    write_path(0x23, id, path);
  }

  void write_set(uint16_t id, const std::string &path,
                 const msgpack::object &value) {
    write_path_pack(0x24, id, path, value);
  }

  msgpack::sbuffer sbuf;
};

} // namespace detail
} // namespace eshet
