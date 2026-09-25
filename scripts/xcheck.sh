#!/bin/bash
# Cross-compile check for the Windows-only components (hook DLL + injector)
# using the local mingw-w64 toolchain. Not part of the product build; used for
# fast verification without a Windows host (CI does the real MSVC build).
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MX=${MX:-$HOME/apt/rootfs/usr/bin/x86_64-w64-mingw32-g++-posix}
OUT=/tmp/xbuild
INC="-I$ROOT/src/hook -I$ROOT/src/common/include \
     -I$HOME/deps/minhook/include -I$HOME/deps/vulkan-headers/include"
mkdir -p $OUT

fail=0
for f in $ROOT/src/hook/*.cpp $ROOT/src/common/src/*.cpp $ROOT/src/injector/*.cpp; do
    obj=$OUT/$(basename $f .cpp).o
    if ! $MX -std=c++17 -Wall -Wextra -c "$f" $INC -o "$obj" 2>$OUT/$(basename $f .cpp).err; then
        echo "FAIL: $f"
        head -25 $OUT/$(basename $f .cpp).err
        fail=1
    fi
done
[ $fail -eq 0 ] && echo "ALL HOOK/INJECTOR/COMMON SOURCES COMPILE OK"
exit $fail
