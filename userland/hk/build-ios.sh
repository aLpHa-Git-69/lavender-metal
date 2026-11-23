#!/bin/bash
# Build the compiler part (hakios-metal's sources in this folder, against the iOS Mesa archives build-mac.sh made in
# $B) into out/*.o and, when asked, the smoke program: build-ios.sh [smoke]
set -euo pipefail
cd "$(dirname "$0")"
. ../../tools/mesa-ios/flags.sh
mkdir -p out
for f in compiler metallib render texture compute pm4 command gpu link; do
	extra=""; [ $f = compiler ] && extra="-DHAKIOS_METAL_MESA -DLAVENDER_GPU_ID=512"
	$CXX $CXXF $extra -Iinclude -c src/$f.cpp -o out/$f.o
done
if [ "${1:-}" = smoke ]; then
	LIBS="$B/build/src/compiler/air/libair.a $(for a in $(cat $B/archives.txt); do [ $a = src/compiler/air/libair.a ] || echo -n "$B/build/$a "; done) -lz -lm"
	$CXX $CXXF -Iinclude -o out/lavcc_smoke lavcc_smoke.cc out/*.o $LIBS
	codesign -s - -f out/lavcc_smoke
fi
echo built
