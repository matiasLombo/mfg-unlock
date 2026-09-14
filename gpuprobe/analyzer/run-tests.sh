#!/bin/sh
# Tests del analizador. Solo biblioteca estandar: nada que instalar.
#
#   sh gpuprobe/analyzer/run-tests.sh
set -e
cd "$(dirname "$0")"
PYTHONPATH=".:tests" python3 -m unittest discover -s tests -q
