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
        on_message(*this), on_close(*this), on_command(*this), send_buf(128) {}

  ESHETClient(const std::pair<std::string, int> &hostport,
              std::optional<msgpack::object_handle> id = {})
      : ESHETClient(hostport.first, hostport.second, std::move(id)) {}

  template <typename T>
  void action_call_pack(std::string path, Channel<Result> result_chan,
                        const T &args) {
    std::unique_ptr<msgpack::zone> z = std::make_unique<msgpack::zone>();
    msgpack::object_handle oh(msgpack::object(args, *z), std::move(z));
    on_command.emplace(Call{path, result_chan, std::move(oh)});
  }

  void action_register(
      std::string path, Channel<Result> result_chan,
      Channel<std::tuple<uint16_t, msgpack::object_handle>> call_chan) {
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

  void connect() {
    // call connect_once until true, with exponential sleep, waiting on exit
    std::chrono::seconds delay(1);
    std::chrono::seconds max_delay(30);

    while (true) {
      if (connect_once())
        return;

      // XXX: wait with timeout
      if (should_exit.readable())
        return;
      std::this_thread::sleep_for(delay);

      delay *= 2;
      if (delay > max_delay)
        delay = max_delay;
    }
  }

  void do_hello() {
    send_buf.start_msg(id ? 0x02 : 0x01);
    send_buf.write8(1);
    if (id)
      send_buf.write_msgpack(id->get());
    send_buf.write_size();
    send_send_buf();
    // XXX: recieve reply here
  }

  void run() {
    // channels: on_message, on_close, exit, command
    while (true) {
    do_connect:
      connect();
      // XXX: error during hello causes tight reconnect loop
      do_hello();
      while (true) {
        switch (wait(on_close, on_message, on_command, should_exit)) {
        case 0: {
          on_close.read(); // XXX: do something with this
          goto do_connect;
        } break;
        case 1: {
          std::vector<uint8_t> data = on_message.read();
          unpacker.push(data);

          std::optional<std::vector<uint8_t>> message;
          while ((message = unpacker.read())) {
            handle_message(*message);
          }
        } break;
        case 2: {
          Command c = on_command.read();
          std::visit(CommandVisitor(*this), std::move(c));
        } break;
        case 3: {
          if (should_exit.read()) {
            if (recv_thread)
              recv_thread->exit();
            // XXX: close connection
            return;
          }
        } break;
        }
      }
    }
  }

  void handle_message(const std::vector<uint8_t> &msg) {
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

      it->second.emplace(id, std::move(oh));
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

    void operator()(Call cmd) {
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
  std::map<std::string, Channel<std::tuple<uint16_t, msgpack::object_handle>>>
      action_channels;

  Channel<Command> on_command;

  SendBuf send_buf;
  uint16_t next_id = 0;
}; // namespace eshet
} // namespace eshet
