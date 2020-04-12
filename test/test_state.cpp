#include "catch2/catch.hpp"
#include "eshet.hpp"

using namespace eshet;
#define NS "/eshetcpp_test_state"

TEST_CASE("make a state and observe") {
  // connect one client which has a state
  ESHETClient client("localhost", 11236);

  Actor self;
  Channel<Result> register_result(self);
  client.state_register(NS "/state", register_result);
  REQUIRE(std::holds_alternative<Success>(register_result.read()));

  // connect another client which observes the state, and check that it gets
  // the unknown callback

  ESHETClient client2("localhost", 11236);

  Channel<StateResult> observe_result(self);
  Channel<StateUpdate> on_change(self);
  client2.state_observe(NS "/state", observe_result, on_change);
  REQUIRE(std::holds_alternative<Unknown>(observe_result.read()));

  // no change yet
  REQUIRE(self.wait_for(std::chrono::seconds(1), on_change) == -1);

  // publish a change; check that it got to the server and was observed by
  // client2

  Channel<Result> update_result(self);
  client.state_changed(NS "/state", 5, update_result);
  REQUIRE(std::holds_alternative<Success>(update_result.read()));

  REQUIRE(on_change.read() == StateUpdate(Known(5)));

  // publish an unknown; check that it got to the server and was observed by
  // client2

  client.state_unknown(NS "/state", update_result);
  REQUIRE(std::holds_alternative<Success>(update_result.read()));

  REQUIRE(on_change.read() == StateUpdate(Unknown()));
}

TEST_CASE("test_reconnection") {
  // connect one client which has a state
  ESHETClient client("localhost", 11236);

  Actor self;
  Channel<Result> register_result(self);
  client.state_register(NS "/state2", register_result);
  REQUIRE(std::holds_alternative<Success>(register_result.read()));

  // connect another client which observes the state, and check that it gets
  // the unknown callback

  ESHETClient client2("localhost", 11236);

  Channel<StateResult> observe_result(self);
  Channel<StateUpdate> on_change(self);
  client2.state_observe(NS "/state2", observe_result, on_change);
  REQUIRE(std::holds_alternative<Unknown>(observe_result.read()));

  auto check_connection = [&](int x) {
    Channel<Result> update_result(self);
    client.state_changed(NS "/state2", x, update_result);
    REQUIRE(std::holds_alternative<Success>(update_result.read()));

    REQUIRE(on_change.read() == StateUpdate(Known(x)));
  };

  check_connection(5);

  // if the state owner disconnects, the observer should see unknown, then the
  // last value, sent by the owner reconnection
  client.test_disconnect();
  REQUIRE(on_change.read() == StateUpdate(Unknown()));
  REQUIRE(on_change.read() == StateUpdate(Known(5)));

  check_connection(6);

  // if the state observer disconnects, it should see unknown, then the
  // last value, sent by the server during reconnection
  client2.test_disconnect();
  REQUIRE(on_change.read() == StateUpdate(Unknown()));
  REQUIRE(on_change.read() == StateUpdate(Known(6)));

  check_connection(7);
}
