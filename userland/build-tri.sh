#!/bin/bash
# lavgpu_tri (M5.4 a) on the Mac: out/lavgpu_tri, to run on the phone with lavk.metallib
set -euo pipefail
cd "$(dirname "$0")"
. ../tools/mesa-ios/flags.sh
mkdir -p out
CC="xcrun -sdk iphoneos clang -arch arm64 -target arm64-apple-ios16.0 -O1 -Wall"
(cd hk && ./build-ios.sh)
$CC -std=gnu11 -c lava5r.c -o out/lava5r.o
$CC -std=gnu11 -c lavgpu_tri.c -o out/lavgpu_tri.o
$CXX $CXXF -Ihk/include -c lavcc.cpp -o out/lavcc.o
LIBS="$D/src/compiler/air/libair.a $(for a in $(cat $B/archives.txt); do [ $a = src/compiler/air/libair.a ] || echo -n "$D/$a "; done)"
$CXX -arch arm64 -isysroot "$SDK" -target arm64-apple-ios16.0 -o out/lavgpu_tri out/lavgpu_tri.o out/lava5r.o out/lavcc.o hk/out/*.o $LIBS \
	-framework IOKit -framework CoreFoundation -lc++ -lz -lm
codesign -s - -f out/lavgpu_tri
echo built
