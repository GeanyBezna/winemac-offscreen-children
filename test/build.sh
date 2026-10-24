#!/bin/sh
# Build the 32-bit test app (requires mingw-w64).
set -e
cd "$(dirname "$0")"
i686-w64-mingw32-gcc -O2 -o glchild_overlay32.exe glchild_overlay.c -ld3d9 -lgdi32 -luser32
echo "built: $(pwd)/glchild_overlay32.exe"
