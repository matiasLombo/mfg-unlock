#!/bin/sh
# Compila la parte de Windows de gpuprobe desde Linux con mingw-w64.
#
#   sh gpuprobe/build-gpuprobe.sh
#
# Sale en build/: gpuprobe-testbed.exe y (cuando exista) version.dll.
# En Windows se usa CMake: cmake -S gpuprobe -B build && cmake --build build.
set -e
cd "$(dirname "$0")/.."

CXX=${MINGW:-x86_64-w64-mingw32-g++}
if ! command -v "$CXX" >/dev/null 2>&1; then
    echo "falta $CXX (apt install g++-mingw-w64-x86-64)"
    exit 1
fi

OUT=build
mkdir -p "$OUT"
FLAGS="-std=c++20 -O2 -Wall -Wextra -Werror -I gpuprobe"
LINK="-static -static-libgcc -static-libstdc++"

CORE="gpuprobe/core/types.cpp gpuprobe/core/jsonl.cpp gpuprobe/core/frame.cpp gpuprobe/core/profile.cpp"

echo "testbed..."
$CXX $FLAGS gpuprobe/testbed/testbed.cpp gpuprobe/d3d12/collector.cpp $CORE \
    -o "$OUT/gpuprobe-testbed.exe" -ld3d12 -ldxgi -ld3dcompiler $LINK

if [ -f gpuprobe/proxy/proxy.cpp ]; then
    echo "proxy..."
    $CXX $FLAGS -shared gpuprobe/proxy/proxy.cpp gpuprobe/d3d12/hooks.cpp \
        gpuprobe/d3d12/collector.cpp gpuprobe/d3d12/executor.cpp $CORE \
        -I external/minhook/include \
        external/minhook/src/hook.c external/minhook/src/buffer.c \
        external/minhook/src/trampoline.c external/minhook/src/hde/hde64.c \
        -o "$OUT/version.dll" -ld3d12 -ldxgi $LINK
fi

ls -la "$OUT"
