#!/usr/bin/env bash

set -e

rm -rf build
cmake -B build -G Ninja \
  -DCMAKE_MAKE_PROGRAM=/ucrt64/bin/ninja.exe \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER=clang \
  -DVCPKG_TARGET_TRIPLET=x64-mingw-static \
  -DVCPKG_HOST_TRIPLET=x64-mingw-static \
  -DCMAKE_TOOLCHAIN_FILE="C:/code/clone/vcpkg/scripts/buildsystems/vcpkg.cmake"
