#pragma once
#include "msgpack.hpp"
#include <string>
#include <vector>

namespace eshet {
namespace detail {
// adapted from msgpack/v1/object.hpp, Boost Software License
struct object_stringize_visitor {
  explicit object_stringize_visitor(std::string &s) : s(s) {}
  bool visit_nil() {
    s += "null";
    return true;
  }
  bool visit_boolean(bool v) {
    if (v)
      s += "true";
    else
      s += "false";
    return true;
  }
  bool visit_positive_integer(uint64_t v) {
    s += std::to_string(v);
    return true;
  }
  bool visit_negative_integer(int64_t v) {
    s += std::to_string(v);
    return true;
  }
  bool visit_float32(float v) {
    s += std::to_string(v);
    return true;
  }
  bool visit_float64(double v) {
    s += std::to_string(v);
    return true;
  }
  bool visit_str(const char *v, uint32_t size) {
    s.push_back('"');
    for (uint32_t i = 0; i < size; ++i) {
      char c = v[i];
      switch (c) {
      case '\\':
        s += "\\\\";
        break;
      case '"':
        s += "\\\"";
        break;
      case '/':
        s += "\\/";
        break;
      case '\b':
        s += "\\b";
        break;
      case '\f':
        s += "\\f";
        break;
      case '\n':
        s += "\\n";
        break;
      case '\r':
        s += "\\r";
        break;
      case '\t':
        s += "\\t";
        break;
      default: {
        unsigned int code = static_cast<unsigned int>(c);
        if (code < 0x20 || code == 0x7f) {
          s += "\\u00";
          char codes[16] = {'0', '1', '2', '3', '4', '5', '6', '7',
                            '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
          s.push_back(codes[(code >> 4) & 0xf]);
          s.push_back(codes[(code >> 0) & 0xf]);
        } else {
          s.push_back(c);
        }
      } break;
      }
    }
    s.push_back('"');
    return true;
  }
  bool visit_bin(const char *v, uint32_t size) {
    s.push_back('"');
    for (uint32_t i = 0; i < size; ++i)
      s.push_back(v[i]);
    s.push_back('"');
    return true;
  }
  bool visit_ext(const char * /*v*/, uint32_t /*size*/) {
    s += "EXT";
    return true;
  }
  bool start_array(uint32_t num_elements) {
    m_current_size.push_back(num_elements);
    s.push_back('[');
    return true;
  }
  bool start_array_item() { return true; }
  bool end_array_item() {
    --m_current_size.back();
    if (m_current_size.back() != 0) {
      s.push_back(',');
    }
    return true;
  }
  bool end_array() {
    m_current_size.pop_back();
    s.push_back(']');
    return true;
  }
  bool start_map(uint32_t num_kv_pairs) {
    m_current_size.push_back(num_kv_pairs);
    s.push_back('{');
    return true;
  }
  bool start_map_key() { return true; }
  bool end_map_key() {
    s.push_back(':');
    return true;
  }
  bool start_map_value() { return true; }
  bool end_map_value() {
    --m_current_size.back();
    if (m_current_size.back() != 0) {
      s.push_back(',');
    }
    return true;
  }
  bool end_map() {
    m_current_size.pop_back();
    s.push_back('}');
    return true;
  }

private:
  std::string &s;
  std::vector<uint32_t> m_current_size;
};
} // namespace detail

void append_msgpack(std::string &s, const msgpack::object &v) {
  detail::object_stringize_visitor vis(s);
  msgpack::object_parser(v).parse(vis);
}

std::string msgpack_to_string(const msgpack::object &v) {
  std::string s;
  append_msgpack(s, v);
  return s;
}
} // namespace eshet
