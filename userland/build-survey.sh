#!/bin/bash
# lavsurvey on the Mac: out/lavsurvey, to run on the phone against a .metallib
set -euo pipefail
cd "$(dirname "$0")"
. ../tools/mesa-ios/flags.sh
mkdir -p out
CC="xcrun -sdk iphoneos clang -arch arm64 -target arm64-apple-ios16.0 -O1 -Wall"
(cd hk && ./build-ios.sh)
$CC -std=gnu11 -c lavsurvey.c -o out/lavsurvey.o
$CC -std=gnu11 -c lava5r.c -o out/lava5r.o
$CXX $CXXF -Ihk/include -c lavcc.cpp -o out/lavcc.o
LIBS="$D/src/compiler/air/libair.a $(for a in $(cat $B/archives.txt); do [ $a = src/compiler/air/libair.a ] || echo -n "$D/$a "; done)"
$CXX -arch arm64 -isysroot "$SDK" -target arm64-apple-ios16.0 -o out/lavsurvey out/lavsurvey.o out/lava5r.o out/lavcc.o hk/out/*.o $LIBS \
	-framework CoreFoundation -lc++ -lz -lm
codesign -s - -f out/lavsurvey
echo built
