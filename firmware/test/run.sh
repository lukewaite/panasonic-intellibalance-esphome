#!/bin/sh
# Build and run the host tests for components/ervlink. VERBOSE=1 prints every core log.
set -e
cd "$(dirname "$0")"
mkdir -p build
c++ -std=c++17 -O1 -g -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I../components/ervlink -DFIXTURES="\"$PWD/fixtures\"" \
  test_core.cpp ../components/ervlink/ervlink_core.cpp -o build/test_core
./build/test_core
