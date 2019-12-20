#include "catch2/catch.hpp"
#include "eshet.hpp"

#define NS "/eshetcpp_test_state"

TEST_CASE("make a state and observe") {
  // connect one client which has a state
  ESHETClient client("localhost", 11236);

  std::mutex mut;
  std::condition_variable cv;

  client.on_connect([&]() { client.state_register(NS "/state").get(); });

  client.wait_connected().get();

  // connect another client which observes the state, and check that it gets
  // the unknown callback

  ESHETClient client2("localhost", 11236);

  int observe_known = 0, observe_unknown = 0, observe_error = 0;
  int state = 0;

  client2.on_connect([&]() {
    client2.state_observe(NS "/state", [&](StateResult result) {
      {
        std::cerr << "observe cb" << std::endl;
        std::unique_lock<std::mutex> guard(mut);
        if (std::holds_alternative<Known>(result)) {
          observe_known++;
          std::get<Known>(result).value.get().convert(state);
        } else if (std::holds_alternative<Unknown>(result)) {
          observe_unknown++;
        } else {
          observe_error++;
        }
      }
      cv.notify_all();
    });
  });

  {
    std::unique_lock<std::mutex> guard(mut);
    cv.wait(guard, [&]() {
      return observe_known || observe_unknown || observe_error;
    });
    REQUIRE(observe_known == 0);
    REQUIRE(observe_unknown == 1);
    REQUIRE(observe_error == 0);
    observe_unknown = 0;
  }

  // publish a change; check that it got to the server and was observed by
  // client2

  int changed_success = 0, changed_error = 0;

  client.state_changed(NS "/state", 5).get();

  {
    std::unique_lock<std::mutex> guard(mut);
    cv.wait(guard, [&]() {
      return observe_known || observe_unknown || observe_error;
    });
    REQUIRE(observe_known == 1);
    REQUIRE(observe_unknown == 0);
    REQUIRE(observe_error == 0);
    REQUIRE(state == 5);
    observe_known = 0;
  }

  // publish an unknown; check that it got to the server and was observed by
  // client2

  client.state_unknown(NS "/state").get();

  {
    std::unique_lock<std::mutex> guard(mut);
    cv.wait(guard, [&]() {
      return observe_known || observe_unknown || observe_error;
    });
    REQUIRE(observe_known == 0);
    REQUIRE(observe_unknown == 1);
    REQUIRE(observe_error == 0);
    observe_unknown = 0;
  }
}
