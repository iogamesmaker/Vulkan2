#!/usr/bin/env bash

set -e

rm -rf build

cmake -B build -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER_TARGET=x86_64-pc-windows-gnu

cmake --build build

./build/vulkan.exe
