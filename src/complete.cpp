#include "eshet.hpp"
#include "utils.hpp"
#include <sstream>
#include <string>

using namespace eshet;

std::pair<std::string, std::string> split_path(std::string path) {
  size_t last_slash = path.find_last_of('/');
  if (last_slash == std::string::npos)
    return {"", ""};

  std::string dir_part = path.substr(0, last_slash) + "/";
  std::string last_part = path.substr(last_slash + 1);
  return {dir_part, last_part};
}

int main(int argc, char **argv) {
  ESHETClient client(get_host_port());

  assert(argc >= 3);
  std::string word = argv[2];

  std::string dir_part, prefix;
  std::tie(dir_part, prefix) = split_path(word);

  Channel<Result> call_result;
  client.action_call_pack("/meta/ls", call_result, std::make_tuple(dir_part));
  auto res = call_result.read();

  client.exit();

  if (!std::holds_alternative<Success>(res))
    return 0;

  auto res_mp = std::get<Success>(res).value.get();

  if (res_mp.type != msgpack::type::object_type::ARRAY)
    return 1;

  // expect ["dir", entries], where each entry is [type, name]
  assert(res_mp.via.array.size == 2);
  assert(res_mp.via.array.ptr[0].as<std::string>() == "dir");

  auto entries = res_mp.via.array.ptr[1]
                     .as<std::vector<std::pair<std::string, std::string>>>();

  for (auto &entry : entries) {
    std::string name, type;
    std::tie(name, type) = entry;

    if (name.substr(0, prefix.size()) == prefix) {
      if (type == "dir")
        std::cout << dir_part << name << "/" << std::endl;
      else
        std::cout << dir_part << name << " " << std::endl;
    }
  }

  return 0;
}
