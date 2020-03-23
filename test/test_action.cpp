#include "catch2/catch.hpp"
#include "eshet.hpp"

using namespace eshet;
#define NS "/eshetcpp_test_action"

TEST_CASE("make and call") {
  ActorThread<ESHETClient> client1("localhost", 11236);

  Actor self;
  Channel<Call> action_chan(self);
  Channel<Result> result_chan(self);

  // register on client1
  client1.action_register(NS "/action", result_chan, action_chan);
  REQUIRE(std::holds_alternative<Success>(result_chan.read()));

  // call on client2
  Actor self2;
  ActorThread<ESHETClient> client2("localhost", 11236);

  auto do_call = [&]() {
    Channel<Result> call_result(self2);
    client2.action_call_pack(NS "/action", call_result, std::make_tuple(5));

    // handle this call
    auto call = action_chan.read();
    auto args = call.as<std::tuple<int>>();
    REQUIRE(std::get<0>(args) == 5);
    call.reply(Success(6));

    // check result
    auto result = std::get<Success>(call_result.read());
    REQUIRE(result.as<int>() == 6);
  };

  do_call();

  // try calling a non-existent action
  {
    Channel<Result> call_result(self2);
    client2.action_call_pack(NS "/actionz", call_result, std::make_tuple(5));
    REQUIRE(std::holds_alternative<Error>(call_result.read()));
  }

  // check reconnection
  client1.test_disconnect();
  // XXX: should add and use disconnect/connect channels
  std::this_thread::sleep_for(std::chrono::seconds(2));

  do_call();

  client1.exit();
  client2.exit();
}
