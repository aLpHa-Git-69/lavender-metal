#!/bin/bash
# Build compiler-smoke.cc for iOS arm64 against the archives build-mac.sh made in $B (default /tmp/mi).
set -euo pipefail
B=${B:-/tmp/mi}
S=$B/mesa-25.0.7
D=$B/build
SDK=$(xcrun -sdk iphoneos --show-sdk-path)
CXX=$(xcrun -sdk iphoneos -find clang++)
DEFS=$(grep -m1 -A4 '^build src/freedreno/ir3/libfreedreno_ir3.a.p/ir3.c.o:' $D/build.ninja | grep -m1 ' ARGS = ' | tr ' ' '\n' | grep '^-D' | tr '\n' ' ')
INC="-isystem $S/src -isystem $S/include -isystem $S/src/freedreno -isystem $S/src/freedreno/common -isystem $S/src/compiler -isystem $S/src/compiler/nir -isystem $S/src/compiler/air -isystem $S/src/gallium/include -isystem $D/src -isystem $D/src/compiler -isystem $D/src/compiler/nir -isystem $D/src/freedreno -isystem $D/src/freedreno/common -isystem $D/src/freedreno/isa"
LIBS="$D/src/compiler/air/libair.a $(for a in $(cat $B/archives.txt); do [ $a = src/compiler/air/libair.a ] || echo -n "$D/$a "; done) -lz -lm"
cd "$(dirname "$0")"
$CXX -arch arm64 -isysroot "$SDK" -target arm64-apple-ios16.0 -std=c++17 -O2 -w $DEFS $INC -o ${OUT:-compiler-smoke} compiler-smoke.cc $LIBS
codesign -s - -f ${OUT:-compiler-smoke}
echo built ${OUT:-compiler-smoke}
