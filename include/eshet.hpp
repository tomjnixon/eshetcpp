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

class ESHETClient : public Actor {
public:
  ESHETClient(const std::string &hostname, int port,
              std::optional<msgpack::object_handle> id = {})
      : hostname(hostname), port(port), id(std::move(id)), should_exit(*this),
        on_message(*this), on_close(*this), on_command(*this), on_reply(*this),
        send_buf(128) {}

  ESHETClient(const std::pair<std::string, int> &hostport,
              std::optional<msgpack::object_handle> id = {})
      : ESHETClient(hostport.first, hostport.second, std::move(id)) {}

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

  bool connect_once() {
    if (recv_thread)
      recv_thread->exit();

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

  bool connect() {
    // call connect_once until true, with exponential sleep, waiting on exit
    std::chrono::seconds delay(1);
    std::chrono::seconds max_delay(30);

    while (true) {
      if (connect_once())
        return true;

      // XXX: wait with timeout
      if (should_exit.readable())
        return false;
      std::this_thread::sleep_for(delay);

      delay *= 2;
      if (delay > max_delay)
        delay = max_delay;
    }
  }

  // send and recieve hello messages, returns success
  bool do_hello() {
    send_buf.start_msg(id ? 0x02 : 0x01);
    send_buf.write8(1);
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

  void run() {
    while (true) {
      // exponential backoff here depending on how long loop has ran for
      loop();

      if (should_exit.readable() && should_exit.read()) {
        if (recv_thread)
          recv_thread->exit();
        // XXX: close connection
        return;
      }
    }
  }

  // connect, say hello, then loop recieving messages; returns if there was an
  // error, or if we should exit
  void loop() {
    if (!connect())
      return;
    if (!do_hello())
      return;

    while (true) {
      switch (wait(on_close, on_message, on_reply, on_command, should_exit)) {
      case 0: {
        on_close.read(); // XXX: do something with this
        return;
      } break;
      case 1: {
        unpacker.push(on_message.read());

        std::optional<std::vector<uint8_t>> message;
        while ((message = unpacker.read())) {
          handle_message(*message);
        }
      } break;
      case 2: {
        auto reply = on_reply.read();
        uint16_t id = std::get<0>(reply);
        Result &result = std::get<1>(reply);

        send_buf.start_msg(std::holds_alternative<Success>(result) ? 0x05
                                                                   : 0x06);
        send_buf.write16(id);
        send_buf.write_msgpack(
            std::visit([](const auto &x) { return x.value.get(); }, result));
        send_buf.write_size();
        send_send_buf();
      } break;
      case 3: {
        Command c = on_command.read();
        std::visit(CommandVisitor(*this), std::move(c));
      } break;
      case 4:
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

      it->second.emplace(id, std::move(oh), on_reply);
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

  struct CommandVisitor {
    CommandVisitor(ESHETClient &c) : c(c) {}

    void operator()(ActionCall cmd) {
      c.send_buf.start_msg(0x11);
      uint16_t id = c.get_id();
      c.send_buf.write16(id);
      c.send_buf.write_string(cmd.path);
      c.send_buf.write_msgpack(*cmd.args);
      c.reply_channels.emplace(id, std::move(cmd.result_chan));
      c.send_buf.write_size();
      c.send_send_buf();
    }

    void operator()(ActionRegister cmd) {
      c.send_buf.start_msg(0x10);
      uint16_t id = c.get_id();
      c.send_buf.write16(id);
      c.send_buf.write_string(cmd.path);
      c.reply_channels.emplace(id, std::move(cmd.result_chan));
      c.action_channels.emplace(cmd.path, std::move(cmd.call_chan));
      c.send_buf.write_size();
      c.send_send_buf();
    }

    ESHETClient &c;
  };

  uint16_t get_id() { return next_id++; }

  void send_send_buf() {
    if (send(sockfd, send_buf.sbuf.data(), send_buf.sbuf.size(), MSG_NOSIGNAL) <
        0)
      throw Disconnected{};
  }

  void exit() { should_exit.push(true); }

private:
  std::string hostname;
  int port;
  std::optional<msgpack::object_handle> id;

  Logger log;

  Channel<bool> should_exit;

  int sockfd;
  Channel<std::vector<uint8_t>> on_message;
  Channel<CloseReason> on_close;
  std::unique_ptr<ActorThread<RecvThread>> recv_thread;

  Unpacker unpacker;

  using ReplyChannel = std::variant<Channel<Result>, Channel<StateResult>>;
  std::map<uint16_t, ReplyChannel> reply_channels;
  std::map<std::string, Channel<Call>> action_channels;

  Channel<Command> on_command;
  Channel<std::tuple<uint16_t, Result>> on_reply;

  SendBuf send_buf;
  uint16_t next_id = 0;
}; // namespace eshet
} // namespace eshet
