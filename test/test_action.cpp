#include "catch2/catch.hpp"
#include "eshet.hpp"

#define NS "/eshetcpp_test_action"

TEST_CASE("make an action and call it") {
  ESHETClient client("localhost", 11236);

  int register_success = 0, register_error = 0;
  std::mutex mut;
  std::condition_variable cv;

  client.on_connect([&]() {
    std::cerr << "on connect" << std::endl;
    client.action_register(
        NS "/action",
        [&](msgpack::object_handle args) {
          std::tuple<int> argss;
          std::cout << args.get() << std::endl;
          args.get().convert(argss);
          return Success(std::get<0>(argss) + 1);
        },
        [&](Result r) {
          {
            std::unique_lock<std::mutex> guard(mut);
            if (std::holds_alternative<Success>(r))
              register_success++;
            else
              register_error++;
          }
          cv.notify_all();
        });
  });

  {
    std::unique_lock<std::mutex> guard(mut);
    cv.wait(guard, [&]() { return register_success || register_error; });
    REQUIRE(register_success == 1);
    REQUIRE(register_error == 0);
  }

  ESHETClient client2("localhost", 11236);

  int action_success = 0, action_error = 0, action_result = 0;

  client2.on_connect([&]() {
    std::cerr << "on connect 2" << std::endl;
    client2.action_call(
        NS "/action",
        [&](Result r) {
          {
            std::unique_lock<std::mutex> guard(mut);
            if (std::holds_alternative<Success>(r)) {
              action_success++;
              std::get<Success>(r).value.get().convert(action_result);
            } else
              register_error++;
          }
          cv.notify_all();
        },
        5);
  });

  {
    std::unique_lock<std::mutex> guard(mut);
    cv.wait(guard, [&]() { return action_success || action_error; });
    REQUIRE(action_success == 1);
    REQUIRE(action_error == 0);
    REQUIRE(action_result == 6);
  }
}
