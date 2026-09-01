#!/bin/sh
# Ask the snippet, on this machine, how many frames it will let us generate.
# Compares the file you point at against a patched copy of the same file.
set -e
cd "$(dirname "$0")"
[ -f nvngx.dll.probe.exe ] || g++ -std=c++20 -O2 -w -o nvngx.dll.probe.exe \
    snippet_probe.cpp -ld3d12 -ldxgi -lole32 -static -static-libgcc -static-libstdc++
src="$1"
[ -n "$src" ] || { echo "usage: run_probe.sh <nvngx_dlssg.dll>"; exit 2; }
tmp="$(mktemp -d)"
python mfg_unlock.py "$src" "$tmp/patched.dll" >/dev/null
echo "=== as shipped ==="
./nvngx.dll.probe.exe "$src"       | grep -E "adapter|Init_Ext|MultiFrameCountMax|multiplier"
echo
echo "=== patched ==="
./nvngx.dll.probe.exe "$tmp/patched.dll" | grep -E "Init_Ext|MultiFrameCountMax|multiplier"
rm -rf "$tmp"
