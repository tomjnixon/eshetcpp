#include "catch2/catch.hpp"
#include "eshet.hpp"

#define NS "/eshetcpp_test_state"

TEST_CASE("make a state and observe") {
  // connect one client which has a state
  ESHETClient client("localhost", 11236);

  client.on_connect([&]() { client.state_register(NS "/state").get(); });

  client.wait_connected().get();

  // connect another client which observes the state, and check that it gets
  // the unknown callback

  ESHETClient client2("localhost", 11236);

  std::mutex mut;
  std::condition_variable cv;
  std::list<StateResult> results;

  client2.on_connect([&]() {
    client2.state_observe(NS "/state", [&](StateResult result) {
      {
        std::unique_lock<std::mutex> guard(mut);
        results.emplace_back(std::move(result));
      }
      cv.notify_all();
    });
  });

  {
    std::unique_lock<std::mutex> guard(mut);
    cv.wait(guard, [&]() { return !results.empty(); });
    REQUIRE(results.size() == 1);
    REQUIRE(std::holds_alternative<Unknown>(results.front()));
    results.clear();
  }

  // publish a change; check that it got to the server and was observed by
  // client2

  int changed_success = 0, changed_error = 0;

  client.state_changed(NS "/state", 5).get();

  {
    std::unique_lock<std::mutex> guard(mut);
    cv.wait(guard, [&]() { return !results.empty(); });
    REQUIRE(results.size() == 1);
    REQUIRE(std::holds_alternative<Known>(results.front()));
    REQUIRE(std::get<Known>(results.front()).as<int>() == 5);
    results.clear();
  }

  // publish an unknown; check that it got to the server and was observed by
  // client2

  client.state_unknown(NS "/state").get();

  {
    std::unique_lock<std::mutex> guard(mut);
    cv.wait(guard, [&]() { return !results.empty(); });
    REQUIRE(results.size() == 1);
    REQUIRE(std::holds_alternative<Unknown>(results.front()));
    results.clear();
  }
}
