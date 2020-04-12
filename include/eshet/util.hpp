#pragma once
#include "data.hpp"
#include <type_traits>

namespace eshet {
namespace detail {

template <typename In, typename Cb>
std::enable_if_t<std::is_invocable_v<Cb, In>, bool> do_convert_variant(In in,
                                                                       Cb cb) {
  cb(std::move(in));
  return true;
}

template <typename In, typename Cb>
std::enable_if_t<!std::is_invocable_v<Cb, In>, bool> do_convert_variant(In in,
                                                                        Cb cb) {
  return false;
}

// Helper to convert between variants holding the same type.
// If `in` holds something that `cb` can be called with, it will be, and the
// return value will be true; otherwise the return value will be false.
template <typename In, typename Cb> bool convert_variant(In in, Cb cb) {
  return std::visit(
      [&](auto x) { return detail::do_convert_variant(std::move(x), cb); },
      std::move(in));
}

} // namespace detail
} // namespace eshet
