#!/bin/sh
# CPU-only cadence regression; requires a C++17 compiler, no Vulkan/SDL runtime.
set -eu
cd "$(dirname "$0")/.."
mkdir -p work
TEST_DIR=$(mktemp -d "$PWD/work/present-timing.XXXXXX")
trap 'rm -rf "$TEST_DIR"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror tools/tests/present_timing_test.cpp -o "$TEST_DIR/test"
"$TEST_DIR/test"
