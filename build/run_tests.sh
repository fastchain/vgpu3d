#!/bin/bash
set -e
R=/workspace/driver
clang -std=c11 -Wall -Werror -I$R/src/include $R/tests/abi_layout_test.c -o /tmp/abi_layout_test && /tmp/abi_layout_test
