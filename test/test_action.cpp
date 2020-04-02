#include "catch2/catch.hpp"
#include "eshet.hpp"

using namespace eshet;
#define NS "/eshetcpp_test_action"

class TestAction : public Actor {
public:
  TestAction(ESHETClient &client) : action_chan(*this), exit_chan(*this) {
    Channel<Result> result_chan(*this);
    client.action_register(NS "/action", result_chan, action_chan);
    REQUIRE(std::holds_alternative<Success>(result_chan.read()));
  }
  void run() {
    while (true) {
      switch (wait(action_chan, exit_chan)) {
      case 0: {
        auto call = action_chan.read();
        auto args = call.as<std::tuple<int>>();
        REQUIRE(std::get<0>(args) == 5);
        call.reply(Success(6));
      } break;
      case 1: {
        return;
      } break;
      }
    }
  }
  void exit() { exit_chan.push(true); }

private:
  Channel<Call> action_chan;
  Channel<Result> result_chan;
  Channel<bool> exit_chan;
};

TEST_CASE("make and call") {
  ActorThread<ESHETClient> client1("localhost", 11236);
  ActorThread<TestAction> test_action(client1);

  Actor self;
  ActorThread<ESHETClient> client2("localhost", 11236);

  auto do_call = [&]() {
    Channel<Result> call_result(self);
    client2.action_call_pack(NS "/action", call_result, std::make_tuple(5));

    // check result
    auto result = call_result.read();
    REQUIRE(std::holds_alternative<Success>(result));
    auto success = std::get<Success>(std::move(result));
    REQUIRE(success.as<int>() == 6);
  };

  do_call();

  // try calling a non-existent action
  {
    Channel<Result> call_result(self);
    client2.action_call_pack(NS "/actionz", call_result, std::make_tuple(5));
    REQUIRE(std::holds_alternative<Error>(call_result.read()));
  }

  // check reconnection
  client1.test_disconnect();
  // XXX: should add and use disconnect/connect channels
  std::this_thread::sleep_for(std::chrono::seconds(2));

  do_call();

  test_action.exit();
  client1.exit();
  client2.exit();
}
