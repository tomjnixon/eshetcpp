#include "catch2/catch.hpp"
#include "eshet.hpp"

using namespace eshet;
#define NS "/eshetcpp_test_action"

TEST_CASE("make and call") {
  ActorThread<ESHETClient> client1("localhost", 11236);

  Actor self;
  Channel<std::tuple<uint16_t, msgpack::object_handle>> action_chan(self);
  Channel<Result> result_chan(self);

  // register on client1
  client1.action_register(NS "/action", result_chan, action_chan);
  REQUIRE(std::holds_alternative<Success>(result_chan.read()));

  // call on client2
  Actor self2;
  Channel<Result> call_result(self2);
  ActorThread<ESHETClient> client2("localhost", 11236);
  client2.action_call_pack(NS "/action", call_result, std::make_tuple(5));

  auto call = action_chan.read();
  std::tuple<int> args;
  // int x;
  std::get<1>(call).get().convert(args);
  REQUIRE(std::get<0>(args) == 5);


  /* auto res = client2.action_call_promise(NS "/action", 5).get(); */
  /* REQUIRE(res.as<int>() == 6); */

  /* auto res2 = client2.action_call_promise(NS "/actionz", 5); */
  /* REQUIRE_THROWS_AS(res2.get().as<int>(), Error); */

  client1.exit();
  client2.exit();
}
