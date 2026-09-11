#!/bin/sh
# Los tests de host, todos. Se corren ANTES de cada instalacion; si algo esta
# en rojo, no se instala. Cada test nuevo se suma aca.
#
#   sh tools/test-host.sh
set -e
cd "$(dirname "$0")/.."
for t in test_resolve test_politica test_config test_diag test_controlador test_reparto test_sitios; do
    g++ -std=c++20 -O2 -Wall -Wextra -I src "tools/$t.cpp" -o "/tmp/$t.exe"
    "/tmp/$t.exe" | tail -1 | sed "s/^/$t: /"
done
