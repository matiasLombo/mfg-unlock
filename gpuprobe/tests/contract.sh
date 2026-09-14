#!/bin/sh
# El contrato entre las tres piezas, ejercitado de punta a punta sin GPU:
#
#   writer de C++  ->  JSONL  ->  analizador de Python
#   stats.h (C++)  ==  ab.py (Python)   sobre las mismas muestras
#
# Si esto esta en verde, el DLL puede escribir una sesion y el analizador la
# entiende; y el veredicto que muestra el overlay en vivo es el mismo que sale
# en el reporte. Las dos cosas se rompen en silencio si no se prueban.
set -e
cd "$(dirname "$0")/../.."

OUT=${TMPDIR:-/tmp}/gpuprobe-contract
mkdir -p "$OUT"
CXX=${CXX:-g++}
FLAGS="-std=c++20 -O2 -Wall -Wextra -Werror -I gpuprobe"

$CXX $FLAGS gpuprobe/tools/gen_session.cpp gpuprobe/core/types.cpp \
    gpuprobe/core/jsonl.cpp -o "$OUT/gen_session"
$CXX $FLAGS gpuprobe/tools/ab_check.cpp -o "$OUT/ab_check"

"$OUT/gen_session" "$OUT/sesion.jsonl"          >/dev/null
"$OUT/gen_session" "$OUT/thrash.jsonl" thrash   >/dev/null
"$OUT/gen_session" "$OUT/ab.jsonl" ab           >/dev/null

PYTHONPATH=gpuprobe/analyzer python3 gpuprobe/tests/contract.py "$OUT"
