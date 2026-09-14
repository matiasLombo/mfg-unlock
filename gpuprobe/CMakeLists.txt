# CMake para la parte de Windows de gpuprobe. Existe sobre todo para el CI:
# el runner de GitHub trae VS2022 y CMake, y con esto no hace falta ninguna
# accion de terceros para compilar con MSVC.
#
#   cmake -S gpuprobe -B build && cmake --build build --config Release
#
# En Linux (mingw) se usa build-gpuprobe.sh, que es mas corto.
cmake_minimum_required(VERSION 3.20)
project(gpuprobe CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    # /W4 y nada de advertencias sobre las funciones "inseguras" de la CRT:
    # usamos las _s donde importa y el resto esta acotado a buffers propios.
    add_compile_options(/W4 /permissive- /D_CRT_SECURE_NO_WARNINGS)
endif()

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_library(gpuprobe_core STATIC
    core/types.cpp
    core/jsonl.cpp
    core/frame.cpp
    core/profile.cpp
    core/executor_core.cpp)

# El testbed: una app D3D12 propia con pasadas deliberadamente mal
# dimensionadas. Corre headless sobre WARP, que es lo que permite probar el
# pipeline entero en CI sin GPU.
add_executable(gpuprobe-testbed
    testbed/testbed.cpp
    d3d12/collector.cpp)
target_link_libraries(gpuprobe-testbed PRIVATE gpuprobe_core d3d12 dxgi d3dcompiler)

# Las herramientas de host, para poder correr el fixture y el chequeo cruzado
# tambien en Windows.
add_executable(gpuprobe-gen-session tools/gen_session.cpp)
target_link_libraries(gpuprobe-gen-session PRIVATE gpuprobe_core)

add_executable(gpuprobe-ab-check tools/ab_check.cpp)
target_link_libraries(gpuprobe-ab-check PRIVATE gpuprobe_core)
