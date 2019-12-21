#pragma once
#include "data.hpp"
#include <future>

namespace eshet {
namespace detail {

template <typename In, typename Cb>
auto do_convert_variant(In in, Cb cb) -> decltype(cb(std::move(in)), bool()) {
  cb(std::move(in));
  return true;
}

bool do_convert_variant(...) { return false; }

// Helper to convert between variants holding the same type.
// If `in` holds something that `cb` can be called with, it will be, and the
// return value will be true; otherwise the return value will be false.
template <typename In, typename Cb> bool convert_variant(In in, Cb cb) {
  return std::visit(
      [&](auto x) { return detail::do_convert_variant(std::move(x), cb); },
      std::move(in));
}

// visitor to resolve a promise with a result; calls set_value for non-Error
// inputs (which may well get converted to another variant), or set_exception
// with Errors.
template <typename PromiseT> struct ResultToPromise {
  ResultToPromise(std::promise<PromiseT> &p) : p(p) {}
  std::promise<PromiseT> &p;

  void operator()(Success v) { p.set_value(std::move(v)); }
  void operator()(Known v) { p.set_value(std::move(v)); }
  void operator()(Unknown v) { p.set_value(std::move(v)); }
  void operator()(Error v) {
    p.set_exception(std::make_exception_ptr(std::move(v)));
  }
};

template <typename PT, typename RT, typename CB>
std::future<PT> wrap_promise(CB wrap_cb) {
  auto p = std::make_shared<std::promise<PT>>();
  auto cb = [p](RT r) { std::visit(ResultToPromise(*p), std::move(r)); };

  wrap_cb(std::move(cb));

  return p->get_future();
}

} // namespace detail
} // namespace eshet
