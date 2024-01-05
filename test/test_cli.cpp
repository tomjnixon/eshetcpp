#include "catch2/catch.hpp"
#include "eshet.hpp"
#include "msgpack/adaptor/vector.hpp"
#include <array>
#include <cstdio>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

using namespace eshet;
#define NS "/eshetcpp_test_cli"

std::string run_eshet(const std::string &args) {
  // see CMakeLists.txt
  std::string cmd = "ESHET_SERVER=localhost:11236 " ESHET_BIN " " + args;
  std::array<char, 128> buffer;
  std::string result;
  std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"),
                                                pclose);
  if (!pipe)
    throw std::runtime_error("popen(" + cmd + ") failed");

  while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr)
    result += buffer.data();

  int rv = pclose(pipe.release());
  if (rv != 0) {
    std::cerr << result << std::endl;
    throw std::runtime_error("command returned " + std::to_string(rv) + ": " +
                             cmd);
  }
  return result;
}

class CLITestAction : public Actor {
public:
  CLITestAction(ESHETClient &client) : action_chan(*this), exit_chan(*this) {
    Channel<Result> result_chan(*this);
    client.action_register(NS "/action", result_chan, action_chan);
    REQUIRE(std::holds_alternative<Success>(result_chan.read()));
  }
  void run() {
    while (true) {
      switch (wait(action_chan, exit_chan)) {
      case 0: {
        auto call = action_chan.read();
        call.reply(Success(std::move(call.value)));
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
  Channel<bool> exit_chan;
};

TEST_CASE("cli call") {
  ESHETClient client1("localhost", 11236);
  ActorThread<CLITestAction> test_action(client1);

  SECTION("no args") {
    std::string out = run_eshet("call " NS "/action");
    REQUIRE(out == "[]\n");
  };

  SECTION("one arg") {
    std::string out = run_eshet("call " NS "/action 5");
    REQUIRE(out == "[5]\n");
  };

  SECTION("two args") {
    std::string out = run_eshet("call " NS "/action 5 '\"foo\"'");
    REQUIRE(out == "[5,\"foo\"]\n");
  };

  SECTION("one arg with brackets and spaces") {
    std::string out = run_eshet("call " NS "/action '[1, 2]'");
    REQUIRE(out == "[[1,2]]\n");
  };
}
