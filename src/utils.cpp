#include "utils.hpp"

std::pair<std::string, int> get_host_port() {
  std::string host = "localhost";
  int port = 11236;

  char *hostport_chr = getenv("ESHET_SERVER");
  if (hostport_chr != NULL) {
    std::string hostport(hostport_chr);

    size_t colon_pos = hostport.find(':');
    if (colon_pos == std::string::npos) {
      host = hostport;
    } else {
      host = hostport.substr(0, colon_pos);
      port = std::stoi(hostport.substr(colon_pos + 1));
    }
  }

  return {host, port};
}
