# eshetcpp

eshet client library in C++.

## Usage

The library is header-only and requires C++17, msgpack-c, and the UNIX sockets
API.

Add `include` to your include path, then:

```cpp
#include "eshet.hpp"
#include <unistd.h>

int main() {
  ESHETClient client("localhost", 11236);

  client.on_connect([&]() {
    client.state_register("/state", [&](Result result) {
      std::cout << "registered: " << result << std::endl;
    });
  });

  // wait for the callback and do something with /state here
  while (true) sleep(1);
}
```

## Development

Build and test like:

    cmake -G Ninja -B build . -DCMAKE_BUILD_TYPE=Debug
    ninja -C build && ninja -C build test

You will need to have an eshet server listening on localhost port 11236.
