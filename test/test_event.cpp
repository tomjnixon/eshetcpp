#include "catch2/catch.hpp"
#include "eshet.hpp"

using namespace eshet;
#define NS "/eshetcpp_test_event"

TEST_CASE("make an event and observe") {
  // connect one client which has a state
  ESHETClient client("localhost", 11236);

  Actor self;
  Channel<Result> register_result(self);
  client.event_register(NS "/event", register_result);
  REQUIRE(std::holds_alternative<Success>(register_result.read()));

  Channel<Result> emit_result(self);
  client.event_emit(NS "/event", 5, emit_result);
  REQUIRE(std::holds_alternative<Success>(emit_result.read()));
}
