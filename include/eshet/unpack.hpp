#pragma once
#include "parse.hpp"
#include <optional>
#include <vector>

namespace eshet {
namespace detail {

// accept a stream of data in arbitrary chunks, and produce complete messages
class Unpacker {
  std::vector<uint8_t> buffer;

public:
  void push(std::vector<uint8_t> buf) {
    for (uint8_t chr : buf)
      buffer.push_back(chr);
  }

  std::optional<std::vector<uint8_t>> read() {
    if (buffer.size() < 3)
      return std::nullopt;

    Parser p(buffer.data(), 3);
    uint8_t magic = p.read8();
    uint16_t length = p.read16();
    p.check_empty();

    if (magic != 0x47)
      throw ProtocolError();

    if (length > (buffer.size() - 3))
      return std::nullopt;

    std::vector<uint8_t> message(buffer.data() + 3, buffer.data() + 3 + length);

    for (size_t i = length + 3; i < buffer.size(); i++)
      buffer[i - (length + 3)] = buffer[i];
    buffer.resize(buffer.size() - (length + 3));

    return message;
  }
};

} // namespace detail
} // namespace eshet
