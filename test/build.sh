#!/bin/sh
# Build the 32-bit test app (requires mingw-w64) and the capture analyser (requires Xcode CLT).
set -e
cd "$(dirname "$0")"
i686-w64-mingw32-gcc -O2 -o glchild_overlay32.exe glchild_overlay.c -ld3d9 -lopengl32 -lgdi32 -luser32
echo "built: $(pwd)/glchild_overlay32.exe"
swiftc -O -o pixstat pixstat.swift
echo "built: $(pwd)/pixstat"
