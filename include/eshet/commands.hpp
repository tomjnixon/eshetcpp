#pragma once
#include "actorpp/actor.hpp"
#include "data.hpp"
#include "msgpack.hpp"

namespace eshet {
using namespace actorpp;

struct ActionCall {
  std::string path;
  Channel<Result> result_chan;
  msgpack::object_handle args;
};

struct ActionRegister {
  std::string path;
  Channel<Result> result_chan;
  Channel<Call> call_chan;
};

using Command = std::variant<ActionCall, ActionRegister>;
} // namespace eshet
