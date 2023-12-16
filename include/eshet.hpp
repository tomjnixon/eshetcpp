#pragma once
#include "actorpp/actor.hpp"
#include "actorpp/net.hpp"
#include "eshet/commands.hpp"
#include "eshet/data.hpp"
#include "eshet/log.hpp"
#include "eshet/msgpack_to_string.hpp"
#include "eshet/unpack.hpp"
#include "eshet/util.hpp"
#include <string>

namespace eshet {
using namespace actorpp;

struct TimeoutConfig {
  /// send a ping if we haven't sent anything for this long
  std::chrono::seconds idle_ping{15};
  /// tell the server to time out if it hasn't received a message for this long;
  /// must be more than idle_ping
  std::chrono::seconds server_timeout{30};
  /// how long to wait for a ping before assuming the connection is dead
  std::chrono::seconds ping_timeout{5};
};

namespace detail {

/// ESHET client
///
/// Methods of this class can be safely called from any thread, so many actors
/// can share a client connection.
///
/// Generally, methods return immediately, and ultimately push their result
/// onto the provided result_chan.
class ESHETClientActor : public Actor {
  using clock = std::chrono::steady_clock;
  using time_point = std::chrono::time_point<clock>;

public:
  explicit ESHETClientActor(const std::string &hostname, int port,
                            std::optional<msgpack::object_handle> id = {},
                            TimeoutConfig timeout_config = {})
      : hostname(hostname), port(port), id(std::move(id)),
        timeout_config(std::move(timeout_config)), ping_result(*this),
        should_exit(*this), on_message(*this), on_close(*this),
        on_command(*this), on_reply(*this), send_buf(128) {}

  explicit ESHETClientActor(const std::pair<std::string, int> &hostport,
                            std::optional<msgpack::object_handle> id = {},
                            TimeoutConfig timeout_config = {})
      : ESHETClientActor(hostport.first, hostport.second, std::move(id),
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
    on_command.push(StateChanged{std::move(path), std::move(result_chan),
                                 Known{std::move(oh)}});
  }

  void state_unknown(std::string path, Channel<Result> result_chan) {
    on_command.push(
        StateChanged{std::move(path), std::move(result_chan), Unknown{}});
  }

  void state_observe(std::string path, Channel<StateResult> result_chan,
                     Channel<StateUpdate> changed_chan) {
    on_command.push(StateObserve{std::move(path), std::move(result_chan),
                                 std::move(changed_chan)});
  }

  void event_register(std::string path, Channel<Result> result_chan) {
    on_command.push(EventRegister{std::move(path), std::move(result_chan)});
  }

  template <typename T>
  void event_emit(std::string path, const T &value,
                  Channel<Result> result_chan) {
    std::unique_ptr<msgpack::zone> z = std::make_unique<msgpack::zone>();
    msgpack::object_handle oh(msgpack::object(value, *z), std::move(z));
    on_command.push(
        EventEmit{std::move(path), std::move(result_chan), std::move(oh)});
  }

  void event_listen(std::string path,
                    Channel<msgpack::object_handle> event_chan,
                    Channel<Result> result_chan) {
    on_command.push(EventListen{std::move(path), std::move(result_chan),
                                std::move(event_chan)});
  }

  void get(std::string path, Channel<Result> result_chan) {
    on_command.push(Get{std::move(path), std::move(result_chan)});
  }

  template <typename T>
  void set(std::string path, const T &value, Channel<Result> result_chan) {
    std::unique_ptr<msgpack::zone> z = std::make_unique<msgpack::zone>();
    msgpack::object_handle oh(msgpack::object(value, *z), std::move(z));
    on_command.push(
        Set{std::move(path), std::move(result_chan), std::move(oh)});
  }

  void test_disconnect() { on_command.push(Disconnect{}); }

  // disconnect and stop the threads. It's not necessary to call this, but it
  // may help the destructor run faster
  void exit() { should_exit.push(true); }

protected:
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

private:
  // connect, say hello, then loop receiving messages; returns if there was
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
      case -1: { // timeout
        if (timeout == idle_timeout) {
          CommandVisitor{*this}(Ping{ping_result});
          ping_timeout = clock::now() + timeout_config.ping_timeout;
        } else { // ping_timeout
          return;
        }
      } break;
      case 0: { // ping_result
        Result r = ping_result.read();
        if (!std::holds_alternative<Success>(r))
          throw ProtocolError(); // bad response to ping
        ping_timeout.reset();
      } break;
      case 1: { // on_close
        on_close.read(); // XXX: do something with this
        return;
      } break;
      case 2: { // on_message
        unpacker.push(on_message.read());

        std::optional<std::vector<uint8_t>> message;
        while ((message = unpacker.read())) {
          handle_message(*message);
        }
      } break;
      case 3: { // on_reply
        uint16_t call_connection_id;
        uint16_t id;
        Result result;
        std::tie(call_connection_id, id, result) = on_reply.read();

        if (call_connection_id == connection_id) {
          send_buf.write_reply(id, result);
          send_send_buf();
        }
      } break;
      case 4: { // on_command
        Command c = on_command.read();
        std::visit(CommandVisitor{*this}, std::move(c));
      } break;
      case 5: { // should_exit
        return;
      } break;
      }
    }
  }

  // methods relating to connection setup and teardown

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

  void cleanup_connection() {
    if (recv_thread) {
      recv_thread.reset();
    }
    if (sockfd != -1) {
      if (close(sockfd) != 0)
        throw std::runtime_error("close(sockfd) failed");
      sockfd = -1;
    }

    for (auto &pair : reply_channels)
      std::visit([](auto &c) { c.push(Error("disconnected")); }, pair.second);
    reply_channels.clear();

    for (auto &state : observed_states)
      state.second.push(Unknown{});

    ping_timeout.reset();
    // make sure to clear this after sending the disconnected messages,
    // otherwise there may still be a disconnect message left over
    ping_result.clear();
    on_close.clear();
    on_message.clear();
  }

  // send and receive hello messages, returns success
  bool do_hello() {
    send_buf.write_hello(id, timeout_config.server_timeout.count());
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
          if (unpacker.read())
            throw ProtocolError();
          return true;
        }
      } break;
      case 2:
        return false;
      }
    }
  }

  void handle_hello_message(const std::vector<uint8_t> &msg) {
    if (msg.size() < 1)
      throw ProtocolError();

    switch (msg[0]) {
    case 0x03: {
      // {hello}
      Parser p(&msg[1], msg.size() - 1);
      p.check_empty();
    } break;
    case 0x04: {
      // {hello_id, ClientID}
      Parser p(&msg[1], msg.size() - 1);
      id = p.read_msgpack();
      p.check_empty();
    } break;
    default:
      throw ProtocolError();
    }
  }

  // send registration commands after reconnecting
  bool reregister() {
    for (auto &action : action_channels) {
      const std::string &path = action.first;
      uint16_t id = get_id();
      send_buf.write_action_register(id, path);
      send_send_buf();
      if (!check_success(id, path))
        return false;
    }

    for (auto &state : registered_states) {
      uint16_t id = get_id();
      const std::string &path = state.first;
      send_buf.write_state_register(id, path);
      send_send_buf();
      if (!check_success(id, path))
        return false;

      id = get_id();
      send_buf.write_state_changed(id, path, state.second);
      send_send_buf();
      if (!check_success(id, path))
        return false;
    }

    for (auto &state : observed_states) {
      uint16_t id = get_id();
      const std::string &path = state.first;
      send_buf.write_state_observe(id, path);
      send_send_buf();
      auto reply = wait_for_reply<StateResult>(id);
      if (!reply)
        return false;
      if (!std::visit(HandleStateReplyVisitor{*this, path, state.second},
                      std::move(*reply)))
        return false;
    }

    for (auto &path : registered_events) {
      uint16_t id = get_id();
      send_buf.write_event_register(id, path);
      send_send_buf();
      if (!check_success(id, path))
        return false;
    }

    for (auto &event : listened_events) {
      uint16_t id = get_id();
      const std::string &path = event.first;
      send_buf.write_event_listen(id, path);
      send_send_buf();
      if (!check_success(id, path))
        return false;
    }

    return true;
  }

  // wait for a reply message, and check that it is not an error, while
  // processing other messages normally
  bool check_success(uint16_t id, const std::string &path) {
    auto reply = wait_for_reply<Result>(id);
    if (reply)
      return std::visit(CheckResultSuccessVisitor{*this, path}, *reply);
    else
      return false;
  }

  template <typename ResultT>
  std::optional<ResultT> wait_for_reply(uint16_t id) {
    Channel<ResultT> result_chan(*this);
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

  struct CheckResultBase {
    ESHETClientActor &c;
    const std::string &path;

    bool operator()(const Error &e) {
      std::string error_msg = "error while adding " + path + ": ";
      append_msgpack(error_msg, e.value.get());
      c.log.error(error_msg);
      return false;
    }
  };

  struct CheckResultSuccessVisitor : public CheckResultBase {
    using CheckResultBase::operator();
    bool operator()(const Success &s) { return true; }
  };

  struct HandleStateReplyVisitor : public CheckResultBase {
    using CheckResultBase::operator();
    Channel<StateUpdate> &channel;

    bool operator()(Known s) {
      channel.push(std::move(s));
      return true;
    }
    bool operator()(Unknown s) {
      channel.push(std::move(s));
      return true;
    }
  };

  // methods for handling commands from the user and sending outgoing messages

  struct CommandVisitor {
    ESHETClientActor &c;

    void operator()(ActionCall cmd) {
      uint16_t id = c.get_id();

      c.reply_channels.emplace(id, std::move(cmd.result_chan));

      c.send_buf.write_action_call(id, cmd.path, *cmd.args);
      c.send_send_buf();
    }

    void operator()(ActionRegister cmd) {
      uint16_t id = c.get_id();
      c.reply_channels.emplace(id, std::move(cmd.result_chan));
      auto it = c.action_channels
                    .emplace(std::move(cmd.path), std::move(cmd.call_chan))
                    .first;

      c.send_buf.write_action_register(id, it->first);
      c.send_send_buf();
    }

    void operator()(StateRegister cmd) {
      uint16_t id = c.get_id();
      c.reply_channels.emplace(id, std::move(cmd.result_chan));
      auto it =
          c.registered_states.emplace(std::move(cmd.path), Unknown{}).first;

      c.send_buf.write_state_register(id, it->first);
      c.send_send_buf();
    }

    void operator()(StateChanged cmd) {
      uint16_t id = c.get_id();
      c.reply_channels.emplace(id, std::move(cmd.result_chan));
      c.registered_states[cmd.path] = std::move(cmd.value);

      c.send_buf.write_state_changed(id, cmd.path, cmd.value);
      c.send_send_buf();
    }

    void operator()(StateObserve cmd) {
      uint16_t id = c.get_id();
      c.reply_channels.emplace(id, std::move(cmd.result_chan));
      auto it = c.observed_states
                    .emplace(std::move(cmd.path), std::move(cmd.changed_chan))
                    .first;

      c.send_buf.write_state_observe(id, it->first);
      c.send_send_buf();
    }

    void operator()(EventRegister cmd) {
      uint16_t id = c.get_id();
      c.reply_channels.emplace(id, std::move(cmd.result_chan));

      auto it = c.registered_events.emplace(std::move(cmd.path)).first;

      c.send_buf.write_event_register(id, *it);
      c.send_send_buf();
    }

    void operator()(EventEmit cmd) {
      uint16_t id = c.get_id();
      c.reply_channels.emplace(id, std::move(cmd.result_chan));

      c.send_buf.write_event_emit(id, cmd.path, *cmd.value);
      c.send_send_buf();
    }

    void operator()(EventListen cmd) {
      uint16_t id = c.get_id();
      c.reply_channels.emplace(id, std::move(cmd.result_chan));

      auto it = c.listened_events
                    .emplace(std::move(cmd.path), std::move(cmd.event_chan))
                    .first;

      c.send_buf.write_event_listen(id, it->first);
      c.send_send_buf();
    }

    void operator()(Get cmd) {
      uint16_t id = c.get_id();
      c.reply_channels.emplace(id, std::move(cmd.result_chan));

      c.send_buf.write_get(id, cmd.path);
      c.send_send_buf();
    }

    void operator()(Set cmd) {
      uint16_t id = c.get_id();
      c.reply_channels.emplace(id, std::move(cmd.result_chan));

      c.send_buf.write_set(id, cmd.path, *cmd.value);
      c.send_send_buf();
    }

    void operator()(Ping cmd) {
      uint16_t id = c.get_id();
      c.reply_channels.emplace(id, std::move(cmd.result_chan));

      c.send_buf.write_ping(id);
      c.send_send_buf();
    }

    void operator()(Disconnect d) { c.on_close.push(CloseReason::Error); }
  };

  uint16_t get_id() { return next_id++; }

  void send_send_buf() {
    idle_timeout = clock::now() + timeout_config.idle_ping;
    if (send(sockfd, send_buf.sbuf.data(), send_buf.sbuf.size(), MSG_NOSIGNAL) <
        0)
      throw Disconnected{};
  }

  // methods for handling incoming messages

  void handle_message(const std::vector<uint8_t> &msg) {
    if (msg.size() < 1)
      throw ProtocolError();

    switch (msg[0]) {
    case 0x03:
    case 0x04:
      throw ProtocolError(); // shouldn't get a hello message
    case 0x05: {
      // {reply, Id, {ok, Msg}}
      Parser p(&msg[1], msg.size() - 1);
      uint16_t id = p.read16();
      msgpack::object_handle oh = p.read_msgpack();
      p.check_empty();
      handle_reply(id, Success(std::move(oh)));
    } break;
    case 0x06: {
      // {reply, Id, {error, Msg}}
      Parser p(&msg[1], msg.size() - 1);
      uint16_t id = p.read16();
      msgpack::object_handle oh = p.read_msgpack();
      p.check_empty();
      handle_reply(id, Error(std::move(oh)));
    } break;
    case 0x07: {
      // {reply_state, Id, {known, Msg}}
      Parser p(&msg[1], msg.size() - 1);
      uint16_t id = p.read16();
      msgpack::object_handle oh = p.read_msgpack();
      p.check_empty();
      handle_reply(id, Known(std::move(oh)));
    } break;
    case 0x08: {
      // {reply_state, Id, unknown}
      Parser p(&msg[1], msg.size() - 1);
      uint16_t id = p.read16();
      p.check_empty();
      handle_reply(id, Unknown());
    } break;
    case 0x0a: {
      // {reply_state, Id, {known, Msg}, T}
      Parser p(&msg[1], msg.size() - 1);
      uint16_t id = p.read16();
      uint32_t t = p.read32();
      msgpack::object_handle oh = p.read_msgpack();
      p.check_empty();
      handle_reply(id, Known(std::move(oh), Time{t}));
    } break;
    case 0x0b: {
      // {reply_state, Id, unknown, T}
      Parser p(&msg[1], msg.size() - 1);
      uint16_t id = p.read16();
      uint32_t t = p.read32();
      p.check_empty();
      handle_reply(id, Unknown(Time(t)));
    } break;
    case 0x11: {
      // {action_call, Id, Path, Msg}
      Parser p(&msg[1], msg.size() - 1);
      uint16_t id = p.read16();
      std::string path = p.read_string();
      msgpack::object_handle oh = p.read_msgpack();
      p.check_empty();

      auto it = action_channels.find(path);
      if (it == action_channels.end())
        // missing callback
        throw ProtocolError();

      it->second.emplace(connection_id, id, std::move(oh), on_reply);
    } break;
    case 0x33: {
      // {event_notify, Path, Msg}
      Parser p(&msg[1], msg.size() - 1);
      std::string path = p.read_string();
      msgpack::object_handle oh = p.read_msgpack();
      p.check_empty();

      auto it = listened_events.find(path);
      if (it == listened_events.end())
        // unknown event
        throw ProtocolError();

      it->second.emplace(std::move(oh));
    } break;
    case 0x44: {
      // {state_changed, Path, {known, State}}
      Parser p(&msg[1], msg.size() - 1);
      std::string path = p.read_string();
      msgpack::object_handle oh = p.read_msgpack();
      p.check_empty();

      auto it = observed_states.find(path);
      if (it == observed_states.end())
        // unknown state
        throw ProtocolError();

      it->second.emplace(Known(std::move(oh)));
    } break;
    case 0x45: {
      // {state_changed, Path, unknown}
      Parser p(&msg[1], msg.size() - 1);
      std::string path = p.read_string();
      p.check_empty();

      auto it = observed_states.find(path);
      if (it == observed_states.end())
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

  std::map<std::string, StateUpdate> registered_states;
  std::map<std::string, Channel<StateUpdate>> observed_states;

  std::set<std::string> registered_events;
  std::map<std::string, Channel<msgpack::object_handle>> listened_events;

  Channel<Command> on_command;
  Channel<std::tuple<uint16_t, uint16_t, Result>> on_reply;

  SendBuf send_buf;
  uint16_t next_id = 0;
};

} // namespace detail

using ESHETClient = ActorThread<detail::ESHETClientActor>;

} // namespace eshet
