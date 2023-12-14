#pragma once
#include <string>
#include <utility>

/// get the ESHET host and port from the ESHET_SERVER environment variable,
/// which should either contain just a host name (for port 11236), or a host
/// name and port number separated by a colon
std::pair<std::string, int> get_host_port();
