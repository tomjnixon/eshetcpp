#include "catch2/catch.hpp"
#include "eshet.hpp"

using namespace eshet;
#define NS "/eshetcpp_test_prop"

TEST_CASE("make, get and set") {
  ESHETClient client("localhost", 11236);

  int prop_value = 5;

  client.on_connect([&] {
    client
        .prop_register(
            NS "/prop", [&]() { return Success(prop_value); },
            [&](msgpack::object_handle value_pack) {
              value_pack.get().convert(prop_value);
              return Success();
            })
        .get();
  });

  client.wait_connected().get();

  ESHETClient client2("localhost", 11236);

  client2.wait_connected().get();

  REQUIRE(client2.get(NS "/prop").get().as<int>() == 5);

  client2.set(NS "/prop", 7).get();

  REQUIRE(client2.get(NS "/prop").get().as<int>() == 7);
}
