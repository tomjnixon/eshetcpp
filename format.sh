#!/bin/bash
find src/ include/ test/ -name '*.cpp' -or -name '*.hpp' | xargs clang-format -i
