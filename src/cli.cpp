#include "CLI11.hpp"
#include "eshet.hpp"
#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "utils.hpp"
#include <sstream>
#include <string>

using namespace eshet;
namespace rj = rapidjson;

struct error_message : public std::exception {
  error_message(std::string message) : message(std::move(message)) {}
  const char *what() const throw() { return message.c_str(); }
  std::string message;
};

msgpack::object json_to_msgpack(const rj::Value &v, msgpack::zone &zone) {
  if (v.IsNull()) {
    return msgpack::object();
  } else if (v.IsBool()) {
    return msgpack::object(v.GetBool());
  } else if (v.IsDouble()) {
    return msgpack::object(v.GetDouble());
  } else if (v.IsFloat()) {
    return msgpack::object(v.GetFloat());
  } else if (v.IsInt64()) {
    return msgpack::object(v.GetInt64());
  } else if (v.IsString()) {
    std::string s(v.GetString(), v.GetStringLength());
    return msgpack::object(s, zone);
  } else if (v.IsArray()) {
    std::vector<msgpack::object> arr;
    for (auto &el : v.GetArray())
      arr.emplace_back(json_to_msgpack(el, zone));
    return msgpack::object(arr, zone);
  } else if (v.IsObject()) {
    msgpack::type::assoc_vector<msgpack::object, msgpack::object> vec;
    for (auto &entry : v.GetObject())
      vec.emplace_back(std::make_pair(json_to_msgpack(entry.name, zone),
                                      json_to_msgpack(entry.value, zone)));
    return msgpack::object(vec, zone);
  } else {
    assert(false);
  }

  return {};
}

msgpack::object json_str_to_msgpack(std::string json, msgpack::zone &zone) {
  rj::Document d;
  rj::ParseResult res = d.Parse(json.data(), json.size());
  if (!res) {
    std::stringstream ss;
    ss << rj::GetParseError_En(res.Code()) << " (" << res.Offset() << ")";
    throw error_message(ss.str());
  }

  return json_to_msgpack(d, zone);
}

msgpack::object_handle json_str_to_msgpack(std::string json) {
  auto zone = std::make_unique<msgpack::zone>();
  msgpack::object obj = json_str_to_msgpack(json, *zone);
  return {obj, std::move(zone)};
}

int handle_result(const Success &r) {
  std::cout << r.value.get() << std::endl;
  return 0;
}

int handle_result(const Error &r) {
  std::cout << r << std::endl;
  return 1;
}

int main(int argc, char **argv) {
  CLI::App app{"eshet CLI"};

  CLI::App *call = app.add_subcommand("call", "call an action");

  std::string path;
  std::vector<std::string> args;
  call->add_option("path", path)->required();
  call->add_option("args", args);
  call->callback([&]() {
    std::vector<msgpack::object> args_o;

    auto zone = std::make_unique<msgpack::zone>();
    for (auto &arg : args)
      args_o.emplace_back(json_str_to_msgpack(arg, *zone));

    ESHETClient client(get_host_port());
    Channel<Result> call_result;
    client.action_call_pack(path, call_result, args_o);
    auto res = call_result.read();
    client.exit();

    return std::visit([](auto &r) { return handle_result(r); }, res);
  });

  app.require_subcommand(1);

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError &e) {
    return app.exit(e);
  } catch (const error_message &e) {
    std::cerr << "error: " << e.what() << std::endl;
    return 1;
  }
  return 0;
}
