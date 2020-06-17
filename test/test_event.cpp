#include "catch2/catch.hpp"
#include "eshet.hpp"

using namespace eshet;
#define NS "/eshetcpp_test_event"

TEST_CASE("make an event and observe") {
  // connect one client which has a state
  ESHETClient client("localhost", 11236);

  // register event
  Actor self;
  Channel<Result> register_result(self);
  client.event_register(NS "/event", register_result);
  REQUIRE(std::holds_alternative<Success>(register_result.read()));

  // emit event
  Channel<Result> emit_result(self);
  client.event_emit(NS "/event", 5, emit_result);
  REQUIRE(std::holds_alternative<Success>(emit_result.read()));

  ESHETClient client2("localhost", 11236);

  // listen
  Channel<msgpack::object_handle> event_chan(self);
  Channel<Result> listen_result(self);
  client2.event_listen(NS "/event", event_chan, listen_result);
  REQUIRE(std::holds_alternative<Success>(listen_result.read()));

  // emit and check recieved
  client.event_emit(NS "/event", 6, emit_result);
  REQUIRE(std::holds_alternative<Success>(emit_result.read()));
  REQUIRE(event_chan.read()->as<int>() == 6);
}
