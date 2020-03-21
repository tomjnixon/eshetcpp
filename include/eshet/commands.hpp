#pragma once
#include "actorpp/actor.hpp"
#include "data.hpp"
#include "msgpack.hpp"

namespace eshet {
using namespace actorpp;

struct Call {
  std::string path;
  Channel<Result> result_chan;
  msgpack::object_handle args;
};

struct ActionRegister {
  std::string path;
  Channel<Result> result_chan;
  Channel<std::tuple<uint16_t, msgpack::object_handle>> call_chan;
};

using Command = std::variant<Call, ActionRegister>;
} // namespace eshet
