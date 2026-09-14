#!/bin/sh
# Los tests de host de gpuprobe: todo lo que no necesita GPU ni Windows.
# Corren en CI y antes de cada build. Si algo esta en rojo, no se instala.
#
#   sh gpuprobe/run-host-tests.sh
#
# Lo que NO se prueba aca es la capa d3d12/ y el testbed: eso se compila en el
# runner de Windows y se corre sobre WARP (ver .github/workflows/gpuprobe.yml).
set -e
cd "$(dirname "$0")/.."

CXX=${CXX:-g++}
FLAGS="-std=c++20 -O2 -Wall -Wextra -Werror -I gpuprobe -pthread"
OUT=${TMPDIR:-/tmp}/gpuprobe-tests
mkdir -p "$OUT"

CORE="gpuprobe/core/types.cpp gpuprobe/core/jsonl.cpp gpuprobe/core/frame.cpp gpuprobe/core/profile.cpp gpuprobe/core/executor_core.cpp"

fail=0
for t in test_hash test_keys test_ring test_frame test_profile test_stats test_executor; do
    [ -f "gpuprobe/tests/$t.cpp" ] || continue
    $CXX $FLAGS "gpuprobe/tests/$t.cpp" $CORE -o "$OUT/$t"
    if ! "$OUT/$t"; then fail=1; fi
done

if [ -f gpuprobe/analyzer/run-tests.sh ]; then
    sh gpuprobe/analyzer/run-tests.sh || fail=1
fi

# El contrato entre el writer de C++, el JSONL y el analizador de Python.
if [ -f gpuprobe/tests/contract.sh ]; then
    sh gpuprobe/tests/contract.sh || fail=1
fi

if [ "$fail" != "0" ]; then
    echo "HAY TESTS EN ROJO"
    exit 1
fi
echo "todos los tests de host en verde"
