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

CORE="gpuprobe/core/types.cpp gpuprobe/core/jsonl.cpp gpuprobe/core/frame.cpp gpuprobe/core/profile.cpp gpuprobe/core/executor_core.cpp"

echo "testbed..."
$CXX $FLAGS gpuprobe/testbed/testbed.cpp gpuprobe/d3d12/collector.cpp \
    gpuprobe/d3d12/capture.cpp gpuprobe/d3d12/gate.cpp $CORE \
    -o "$OUT/gpuprobe-testbed.exe" -ld3d12 -ldxgi -ld3dcompiler $LINK

# El overlay es opcional: se compila si hay ImGui en external/imgui. Sin el,
# gpuprobe mide, analiza y aplica igual -- lo unico que falta es poder mirarlo
# mientras pasa.
IMGUI_OBJS=""
IMGUI_INC=""
IMGUI_LIBS=""
if [ -f external/imgui/imgui.cpp ]; then
    echo "con overlay (ImGui en external/imgui)"
    IMGUI_INC="-I external/imgui"
    # ImGui se compila aparte y con las advertencias apagadas: es codigo de
    # terceros y no tiene por que pasar nuestro -Werror. El shim tapa un hueco
    # del d3d12.h de mingw (falta PFN_D3D12_SERIALIZE_ROOT_SIGNATURE), y el
    # gamepad se apaga porque el mando lo esta usando el juego.
    for f in imgui imgui_draw imgui_tables imgui_widgets \
             backends/imgui_impl_dx12 backends/imgui_impl_win32; do
        o="$OUT/imgui_$(basename $f).o"
        if [ ! -f "$o" ] || [ "external/imgui/$f.cpp" -nt "$o" ]; then
            $CXX -std=c++20 -O2 -w -DIMGUI_IMPL_WIN32_DISABLE_GAMEPAD \
                -include gpuprobe/d3d12/imgui_mingw_shim.h \
                -I external/imgui -I gpuprobe -c "external/imgui/$f.cpp" -o "$o"
        fi
        IMGUI_OBJS="$IMGUI_OBJS $o"
    done
    # imm32 lo usa ImGui para el IME; d3dcompiler, el backend de D3D12 para
    # compilar su shader de UI. Los dos vienen con Windows.
    IMGUI_LIBS="-limm32 -ld3dcompiler"
else
    echo "sin overlay (no hay external/imgui): gpuprobe mide igual"
fi

# El proxy: version.dll. Sin MinHook ni ninguna otra dependencia -- las
# vtables se roban con objetos descartables propios (ver d3d12/hooks.cpp).
echo "proxy version.dll..."
$CXX $FLAGS $IMGUI_INC -shared \
    gpuprobe/proxy/proxy.cpp gpuprobe/proxy/forwards_version.S \
    gpuprobe/proxy/exports.def \
    gpuprobe/d3d12/hooks.cpp gpuprobe/d3d12/collector.cpp \
    gpuprobe/d3d12/executor.cpp gpuprobe/d3d12/gate.cpp \
    gpuprobe/d3d12/capture.cpp gpuprobe/d3d12/overlay.cpp $IMGUI_OBJS $CORE \
    -I gpuprobe/proxy \
    -o "$OUT/version.dll" -ld3d12 -ldxgi -lgdi32 -luser32 -ldwmapi \
    $IMGUI_LIBS $LINK

ls -la "$OUT"
