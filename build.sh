#!/bin/sh
set -e
cd "$(dirname "$0")"
g++ -shared -std=c++20 -O2 -DNDEBUG -w \
  -I external/reshade/include -I external/minhook/include -I external/imgui \
  -o mfg-multiplier.addon64 src/addon.cpp \
  external/minhook/src/hook.c external/minhook/src/buffer.c \
  external/minhook/src/trampoline.c external/minhook/src/hde/hde64.c \
  -static -static-libgcc -static-libstdc++
echo "built: $(ls -la mfg-multiplier.addon64 | awk '{print $5}') bytes"
