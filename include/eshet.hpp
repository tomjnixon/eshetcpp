#pragma once
#include "actorpp/actor.hpp"
#include "actorpp/net.hpp"
#include "eshet/commands.hpp"
#include "eshet/data.hpp"
#include "eshet/log.hpp"
#include "eshet/unpack.hpp"
#include "eshet/util.hpp"
#include <string>

namespace eshet {
using namespace actorpp;
using namespace detail;

struct TimeoutConfig {
  // send a ping if we haven't sent anything for this long
  std::chrono::seconds idle_ping{15};
  // tell the server to time out if it hasn't received a message for this long;
  // must be more than idle_ping
  std::chrono::seconds server_timeout{30};
  // how long to wait for a ping before assuming the connection is dead
  std::chrono::seconds ping_timeout{5};
};

class ESHETClient : public Actor {
  using clock = std::chrono::steady_clock;
  using time_point = std::chrono::time_point<clock>;

public:
  ESHETClient(const std::string &hostname, int port,
              std::optional<msgpack::object_handle> id = {},
              TimeoutConfig timeout_config = {})
      : hostname(hostname), port(port), id(std::move(id)),
        timeout_config(std::move(timeout_config)), ping_result(*this),
        should_exit(*this), on_message(*this), on_close(*this),
        on_command(*this), on_reply(*this), send_buf(128) {}

  ESHETClient(const std::pair<std::string, int> &hostport,
              std::optional<msgpack::object_handle> id = {},
              TimeoutConfig timeout_config = {})
      : ESHETClient(hostport.first, hostport.second, std::move(id),
                    std::move(timeout_config)) {}

  template <typename T>
  void action_call_pack(std::string path, Channel<Result> result_chan,
                        const T &args) {
    std::unique_ptr<msgpack::zone> z = std::make_unique<msgpack::zone>();
    msgpack::object_handle oh(msgpack::object(args, *z), std::move(z));
    on_command.emplace(ActionCall{path, result_chan, std::move(oh)});
  }

  void action_register(std::string path, Channel<Result> result_chan,
                       Channel<Call> call_chan) {
    on_command.emplace(ActionRegister{path, result_chan, call_chan});
  }

  void state_register(std::string path, Channel<Result> result_chan) {
    on_command.emplace(StateRegister{path, result_chan});
  }

  template <typename T>
  void state_changed(std::string path, const T &value,
                     Channel<Result> result_chan) {
    std::unique_ptr<msgpack::zone> z = std::make_unique<msgpack::zone>();
    msgpack::object_handle oh(msgpack::object(value, *z), std::move(z));
    on_command.emplace(StateChanged{path, result_chan, Known{std::move(oh)}});
  }

  void state_unknown(std::string path, Channel<Result> result_chan) {
    on_command.emplace(StateChanged{path, result_chan, Unknown{}});
  }

  void state_observe(std::string path, Channel<StateResult> result_chan,
                     Channel<StateUpdate> changed_chan) {
    on_command.emplace(StateObserve{path, result_chan, changed_chan});
  }

  void test_disconnect() { on_command.emplace(Disconnect{}); }

  void cleanup_connection() {
    if (recv_thread) {
      recv_thread.reset();
    }
    if (sockfd != -1) {
      assert(close(sockfd) == 0);
      sockfd = -1;
    }

    ping_result.clear();
    on_close.clear();
    on_message.clear();

    for (auto &pair : reply_channels)
      std::visit([](auto &c) { c.push(Error("disconnected")); }, pair.second);
    reply_channels.clear();

    for (auto &state : state_channels)
      state.second.emplace(Unknown{});
  }

  bool connect() {
    cleanup_connection();

    try {
      sockfd = actorpp::connect(hostname, port);
    } catch (std::runtime_error &e) {
      log.error(e.what());
      return false;
    }

    recv_thread = std::make_unique<actorpp::ActorThread<actorpp::RecvThread>>(
        sockfd, on_message, on_close);
    return true;
  }

  // send and receive hello messages, returns success
  bool do_hello() {
    send_buf.start_msg(id ? 0x02 : 0x01);
    send_buf.write8(1);
    send_buf.write16(timeout_config.server_timeout.count());
    if (id)
      send_buf.write_msgpack(id->get());
    send_buf.write_size();
    send_send_buf();

    while (true) {
      switch (wait(on_close, on_message, should_exit)) {
      case 0:
        on_close.read();
        return false;
      case 1: {
        unpacker.push(on_message.read());

        std::optional<std::vector<uint8_t>> message;
        if ((message = unpacker.read())) {
          handle_hello_message(*message);

          // no reason for the server to have sent us any more messages here
          assert(!unpacker.read());
          return true;
        }
      } break;
      case 2:
        return false;
      }
    }
  }

  struct CheckResultSuccess {
    ESHETClient &c;
    const std::string &path;

    bool operator()(const Success &s) { return true; }
    bool operator()(const Error &e) {
      std::stringstream error_msg;
      error_msg << "error while adding " << path << ": " << e.value.get();
      c.log.error(error_msg.str());
      return false;
    }
  };

  struct HandleStateReply {
    ESHETClient &c;
    const std::string &path;
    Channel<StateUpdate> &channel;

    bool operator()(Known s) {
      channel.push(std::move(s));
      return true;
    }
    bool operator()(Unknown s) {
      channel.push(std::move(s));
      return true;
    }
    bool operator()(const Error &e) {
      std::stringstream error_msg;
      error_msg << "error while adding " << path << ": " << e.value.get();
      c.log.error(error_msg.str());
      return false;
    }
  };

  // wait for a reply message, and check that it is not an error, while
  // processing other messages normally
  bool check_success(uint16_t id, const std::string &path) {
    auto reply = wait_for_reply<Result>(id);
    if (reply)
      return std::visit(CheckResultSuccess{*this, path}, *reply);
    else
      return false;
  }

  template <typename Resultt>
  std::optional<Resultt> wait_for_reply(uint16_t id) {
    Channel<Resultt> result_chan(*this);
    reply_channels.emplace(id, result_chan);

    while (true) {
      switch (wait(on_close, on_message, result_chan, should_exit)) {
      case 0: // on_close
        on_close.read();
        return {};
      case 1: { // on_message
        unpacker.push(on_message.read());

        std::optional<std::vector<uint8_t>> message;
        while ((message = unpacker.read())) {
          handle_message(*message);
        }
      } break;
      case 2: { // result_chan
        return result_chan.read();
      } break;
      case 3:
        return {};
      }
    }
  }

  // send registration commands after reconnecting
  bool reregister() {
    for (auto &action : action_channels) {
      const std::string &path = action.first;
      uint16_t id = get_id();
      send_action_register(id, path);
      if (!check_success(id, path))
        return false;
    }

    for (auto &state : states) {
      uint16_t id = get_id();
      const std::string &path = state.first;
      send_state_register(id, path);
      if (!check_success(id, path))
        return false;

      id = get_id();
      send_state_changed(id, path, state.second);
      if (!check_success(id, path))
        return false;
    }

    for (auto &state : state_channels) {
      uint16_t id = get_id();
      const std::string &path = state.first;
      send_state_observe(id, path);
      auto reply = wait_for_reply<StateResult>(id);
      if (!reply)
        return false;
      if (!std::visit(HandleStateReply{*this, path, state.second},
                      std::move(*reply)))
        return false;
    }

    return true;
  }

  void run() {
    // repeatedly call loop, with exponential backoff from min_delay to
    // max_delay, resetting back to min_delay if loop runs for at least
    // reset_thresh
    const std::chrono::seconds min_delay(1);
    const std::chrono::seconds max_delay(30);
    const std::chrono::seconds reset_thresh(10);

    std::chrono::seconds delay(min_delay);

    while (true) {
      auto start = clock::now();
      loop();
      auto end = clock::now();

      if ((end - start) >= reset_thresh)
        delay = min_delay;

      if (wait_for(delay, should_exit) == 0) {
        cleanup_connection();
        return;
      }

      delay *= 2;
      if (delay > max_delay)
        delay = max_delay;
    }
  }

  // connect, say hello, then loop recieving messages; returns if there was
  // an error, or if we should exit
  void loop() {
    if (!connect())
      return;
    connection_id++;
    if (!do_hello())
      return;
    if (!reregister())
      return;

    while (true) {
      time_point timeout =
          ping_timeout ? std::min(*ping_timeout, idle_timeout) : idle_timeout;

      switch (wait_until(timeout, ping_result, on_close, on_message, on_reply,
                         on_command, should_exit)) {
      case -1: {
        if (timeout == idle_timeout) {
          std::visit(CommandVisitor(*this), Command{Ping{ping_result}});
          ping_timeout = clock::now() + timeout_config.ping_timeout;
        } else { // ping_timeout
          return;
        }
      } break;
      case 0: {
        Result r = ping_result.read();
        if (!std::holds_alternative<Success>(r))
          throw ProtocolError(); // bad response to ping
        ping_timeout.reset();
      } break;
      case 1: {
        on_close.read(); // XXX: do something with this
        return;
      } break;
      case 2: {
        unpacker.push(on_message.read());

        std::optional<std::vector<uint8_t>> message;
        while ((message = unpacker.read())) {
          handle_message(*message);
        }
      } break;
      case 3: {
        uint16_t call_connection_id;
        uint16_t id;
        Result result;
        std::tie(call_connection_id, id, result) = on_reply.read();

        if (call_connection_id == connection_id) {
          send_buf.start_msg(std::holds_alternative<Success>(result) ? 0x05
                                                                     : 0x06);
          send_buf.write16(id);
          send_buf.write_msgpack(
              std::visit([](const auto &x) { return x.value.get(); }, result));
          send_buf.write_size();
          send_send_buf();
        }
      } break;
      case 4: {
        Command c = on_command.read();
        std::visit(CommandVisitor(*this), std::move(c));
      } break;
      case 5:
        return;
      }
    }
  }

  void handle_hello_message(const std::vector<uint8_t> &msg) {
    if (msg.size() < 1)
      throw ProtocolError();

    switch (msg[0]) {
    case 0x03: {
      // {hello}
      parse(&msg[1], msg.size() - 1);
    } break;
    case 0x04: {
      // {hello_id, ClientID}
      std::tie(id) = parse(&msg[1], msg.size() - 1, read_msgpack);
    } break;
    default:
      throw ProtocolError();
    }
  }

  void handle_message(const std::vector<uint8_t> &msg) {
    if (msg.size() < 1)
      throw ProtocolError();

    switch (msg[0]) {
    case 0x03:
    case 0x04:
      throw ProtocolError(); // shouldn't get a hello message
    case 0x05: {
      // {reply, Id, {ok, Msg}}
      uint16_t id;
      msgpack::object_handle oh;
      std::tie(id, oh) = parse(&msg[1], msg.size() - 1, read16, read_msgpack);
      handle_reply(id, Success(std::move(oh)));
    } break;
    case 0x06: {
      // {reply, Id, {error, Msg}}
      uint16_t id;
      msgpack::object_handle oh;
      std::tie(id, oh) = parse(&msg[1], msg.size() - 1, read16, read_msgpack);
      handle_reply(id, Error(std::move(oh)));
    } break;
    case 0x07: {
      // {reply_state, Id, {known, Msg}}
      uint16_t id;
      msgpack::object_handle oh;
      std::tie(id, oh) = parse(&msg[1], msg.size() - 1, read16, read_msgpack);
      handle_reply(id, Known(std::move(oh)));
    } break;
    case 0x08: {
      // {reply_state, Id, unknown}
      uint16_t id;
      std::tie(id) = parse(&msg[1], msg.size() - 1, read16);
      handle_reply(id, Unknown());
    } break;
    case 0x11: {
      // {action_call, Id, Path, Msg}
      uint16_t id;
      std::string path;
      msgpack::object_handle oh;
      std::tie(id, path, oh) =
          parse(&msg[1], msg.size() - 1, read16, read_string, read_msgpack);

      auto it = action_channels.find(path);
      if (it == action_channels.end())
        // missing callback
        throw ProtocolError();

      it->second.emplace(connection_id, id, std::move(oh), on_reply);
    } break;
    case 0x44: {
      // {state_changed, Path, {known, State}}
      std::string path;
      msgpack::object_handle oh;
      std::tie(path, oh) =
          parse(&msg[1], msg.size() - 1, read_string, read_msgpack);

      auto it = state_channels.find(path);
      if (it == state_channels.end())
        // unknown state
        throw ProtocolError();

      it->second.emplace(Known(std::move(oh)));
    } break;
    case 0x45: {
      // {state_changed, Path, unknown}
      std::string path;
      std::tie(path) = parse(&msg[1], msg.size() - 1, read_string);

      auto it = state_channels.find(path);
      if (it == state_channels.end())
        // unknown state
        throw ProtocolError();

      it->second.emplace(Unknown());
    } break;
    }
  }

  void handle_reply(uint16_t id, AnyResult result) {
    auto it = reply_channels.find(id);
    if (it == reply_channels.end())
      // missing callback
      throw ProtocolError();

    auto nh = reply_channels.extract(it);
    if (!std::visit(
            [&](auto &cb) {
              using T =
                  typename std::remove_reference<decltype(cb)>::type::type;
              return detail::convert_variant(
                  std::move(result),
                  [&](T result_t) { return cb.push(std::move(result_t)); });
            },
            nh.mapped()))
      // wrong type of return
      throw ProtocolError();
  }

  void send_action_register(uint16_t id, const std::string &path) {
    send_register(0x10, id, path);
  }

  void send_state_register(uint16_t id, const std::string &path) {
    send_register(0x40, id, path);
  }

  void send_state_observe(uint16_t id, const std::string &path) {
    send_register(0x43, id, path);
  }

  void send_state_changed(uint16_t id, const std::string &path,
                          const Known &state) {
    send_buf.start_msg(0x41);
    send_buf.write16(id);
    send_buf.write_string(path);
    send_buf.write_msgpack(*state.value);
    send_buf.write_size();
    send_send_buf();
  }

  void send_state_changed(uint16_t id, const std::string &path,
                          const Unknown &state) {
    send_buf.start_msg(0x42);
    send_buf.write16(id);
    send_buf.write_string(path);
    send_buf.write_size();
    send_send_buf();
  }

  void send_state_changed(uint16_t id, const std::string &path,
                          const StateUpdate &state) {
    std::visit([&](const auto &s) { send_state_changed(id, path, s); }, state);
  }

  void send_register(uint8_t message, uint16_t id, const std::string &path) {
    send_buf.start_msg(message);
    send_buf.write16(id);
    send_buf.write_string(path);
    send_buf.write_size();
    send_send_buf();
  }

  struct CommandVisitor {
    CommandVisitor(ESHETClient &c) : c(c) {}

    void operator()(ActionCall cmd) {
      uint16_t id = c.get_id();

      c.reply_channels.emplace(id, std::move(cmd.result_chan));

      c.send_buf.start_msg(0x11);
      c.send_buf.write16(id);
      c.send_buf.write_string(cmd.path);
      c.send_buf.write_msgpack(*cmd.args);
      c.send_buf.write_size();
      c.send_send_buf();
    }

    void operator()(ActionRegister cmd) {
      uint16_t id = c.get_id();
      c.reply_channels.emplace(id, std::move(cmd.result_chan));
      auto it = c.action_channels
                    .emplace(std::move(cmd.path), std::move(cmd.call_chan))
                    .first;

      c.send_action_register(id, it->first);
    }

    void operator()(StateRegister cmd) {
      uint16_t id = c.get_id();
      c.reply_channels.emplace(id, std::move(cmd.result_chan));
      auto it = c.states.emplace(std::move(cmd.path), Unknown{}).first;

      c.send_state_register(id, it->first);
    }

    void operator()(StateChanged cmd) {
      uint16_t id = c.get_id();
      c.reply_channels.emplace(id, std::move(cmd.result_chan));
      c.states[cmd.path] = std::move(cmd.value);

      c.send_state_changed(id, cmd.path, cmd.value);
    }

    void operator()(StateObserve cmd) {
      uint16_t id = c.get_id();
      c.reply_channels.emplace(id, std::move(cmd.result_chan));
      auto it = c.state_channels
                    .emplace(std::move(cmd.path), std::move(cmd.changed_chan))
                    .first;

      c.send_state_observe(id, it->first);
    }

    void operator()(Ping cmd) {
      uint16_t id = c.get_id();
      c.reply_channels.emplace(id, std::move(cmd.result_chan));

      c.send_buf.start_msg(0x09);
      c.send_buf.write16(id);
      c.send_buf.write_size();
      c.send_send_buf();
    }

    void operator()(Disconnect d) { c.on_close.push(CloseReason::Error); }

    ESHETClient &c;
  };

  uint16_t get_id() { return next_id++; }

  void send_send_buf() {
    idle_timeout = clock::now() + timeout_config.idle_ping;
    if (send(sockfd, send_buf.sbuf.data(), send_buf.sbuf.size(), MSG_NOSIGNAL) <
        0)
      throw Disconnected{};
  }

  void exit() { should_exit.push(true); }

private:
  std::string hostname;
  int port;
  std::optional<msgpack::object_handle> id;
  TimeoutConfig timeout_config;

  std::optional<time_point> ping_timeout;
  time_point idle_timeout;
  Channel<Result> ping_result;

  Logger log;

  Channel<bool> should_exit;

  int sockfd = -1;
  Channel<std::vector<uint8_t>> on_message;
  Channel<CloseReason> on_close;
  std::unique_ptr<ActorThread<RecvThread>> recv_thread;
  uint16_t connection_id = 0;

  Unpacker unpacker;

  using ReplyChannel = std::variant<Channel<Result>, Channel<StateResult>>;
  std::map<uint16_t, ReplyChannel> reply_channels;
  std::map<std::string, Channel<Call>> action_channels;

  std::map<std::string, StateUpdate> states;
  std::map<std::string, Channel<StateUpdate>> state_channels;

  Channel<Command> on_command;
  Channel<std::tuple<uint16_t, uint16_t, Result>> on_reply;

  SendBuf send_buf;
  uint16_t next_id = 0;
}; // namespace eshet
} // namespace eshet
