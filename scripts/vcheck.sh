#!/bin/bash
# Compile check for the cross-platform viewer sources (no linking): uses the
# locally extracted X11/GL headers plus GLFW/ImGui checkouts in ~/deps.
# Verification aid only; CI performs the real CMake builds.
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEPS=$HOME/deps
SYS=$HOME/apt/rootfs/usr
OUT=/tmp/vbuild
INC="-I$ROOT/src/viewer/include -I$ROOT/src/common/include \
     -I$SYS/include -I$SYS/include/x86_64-linux-gnu \
     -I$DEPS/imgui -I$DEPS/imgui/backends -I$DEPS/glfw/include"
mkdir -p $OUT

fail=0
for f in $ROOT/src/viewer/src/*.cpp; do
    obj=$OUT/$(basename $f .cpp).o
    if ! g++ -std=c++17 -Wall -Wextra -c "$f" $INC -o "$obj" 2>$OUT/$(basename $f .cpp).err; then
        echo "FAIL: $f"
        head -25 $OUT/$(basename $f .cpp).err
        fail=1
    fi
done
[ $fail -eq 0 ] && echo "ALL VIEWER SOURCES COMPILE OK"
exit $fail
