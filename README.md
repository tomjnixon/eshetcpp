# eshetcpp

eshet client library and command-line utility in C++.

This works with [the ESHET server]{https://github.com/tomjnixon/eshetsrv). For
ESP32 integration, see
[eshet-esp-idf](https://github.com/tomjnixon/eshet-esp-idf).

## build and install

First, make sure to clone this repository recursively, or run

```
git submodule update --init
```

Configure and build with:

```
cmake -G Ninja -B build . -DCMAKE_INSTALL_PREFIX=$HOME/.local
ninja -C build
```

The CLI tool can be ran from the build directory (`./build/src/eshet`) or
installed with:

```
ninja -C build install
```

This will install to the path set in `CMAKE_INSTALL_PREFIX`, which should
contain a `bin` directory on your path.

To enable bash completion, source the installed `eshet_complete.bash` in your
`.bashrc`:

```bash
source ~/.local/share/eshet_complete.bash
```

## CLI usage

See `eshet --help` for a list of commands and `eshet [command] --help` for help
on each command.

The server to connect to is configured with the `ESHET_SERVER` environment
variable, which should either contain just a host name (for port 11236), or a
host name and port number separated by a colon.

## development

Build and test like:

    cmake -G Ninja -B build . -DCMAKE_BUILD_TYPE=Debug
    ninja -C build && ninja -C build test

You will need to have an eshet server listening on localhost port 11236.
