#!/bin/sh
set -e
cd "$(dirname "$0")"
g++ -shared -std=c++20 -O2 -DNDEBUG -w \
  -o version.dll src/proxy.cpp \
  -static -static-libgcc -static-libstdc++ \
  -Wl,--enable-stdcall-fixup -lkernel32
echo "built: $(ls -la version.dll | awk '{print $5}') bytes"
