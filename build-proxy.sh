#!/bin/sh
# Builds version.dll. Needs a MinGW-w64 g++ with C++20 and MinHook in external/.
set -e
cd "$(dirname "$0")"

# src/cubins.h holds kernels rebuilt from the PTX in the snippet installed on
# this machine. It is not in the repository -- it is derived from NVIDIA's
# binary, so it is generated locally or not at all. Without it the proxy still
# builds; the kernel swap is simply compiled out.
if [ ! -f src/cubins.h ]; then
  echo "src/cubins.h missing -- run: python tools/rebuild_cubins.py"
  echo "building without the kernel rebuild"
  cat > src/cubins.h <<'STUB'
// Placeholder. Run tools/rebuild_cubins.py to generate the real one.
#pragma once
static const char kCubinsBuiltFor[] = "(not built)";
struct CubinPatch {
    unsigned text, shared, regs, orig_size, size;
    const unsigned char *data;
    const char *what;
};
static const CubinPatch kCubinPatches[] = {};
STUB
fi

g++ -shared -std=c++20 -O2 -DNDEBUG -w -I external/minhook/include \
  -o version.dll src/proxy.cpp \
  external/minhook/src/hook.c external/minhook/src/buffer.c \
  external/minhook/src/trampoline.c external/minhook/src/hde/hde64.c \
  -static -static-libgcc -static-libstdc++ -Wl,--enable-stdcall-fixup -lkernel32
echo "built: $(ls -la version.dll | awk '{print $5}') bytes"
