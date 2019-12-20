#include "catch2/catch.hpp"
#include "eshet.hpp"

#define NS "/eshetcpp_test_action"

TEST_CASE("make and call") {
  ESHETClient client("localhost", 11236);
  client.on_connect([&] {
    client
        .action_register(NS "/action",
                         [&](msgpack::object_handle args) {
                           std::tuple<int> argss;
                           args.get().convert(argss);
                           return Success(std::get<0>(argss) + 1);
                         })
        .get();
  });

  client.wait_connected().get();

  ESHETClient client2("localhost", 11236);

  client2.wait_connected().get();

  auto res = client2.action_call_promise(NS "/action", 5).get();
  REQUIRE(res.as<int>() == 6);

  auto res2 = client2.action_call_promise(NS "/actionz", 5);
  REQUIRE_THROWS_AS(res2.get().as<int>(), Error);
}
