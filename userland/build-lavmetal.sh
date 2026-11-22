#!/bin/sh
# Build the M5.1 pieces on the Mac (Xcode, iPhoneOS SDK):
#   out/LavenderMetal.bundle   the plug-in, to be copied to /System/Library/Extensions/ on the phone
#   out/lavmetal_test          the program that asks for the device
set -e
cd "$(dirname "$0")"
mkdir -p out/LavenderMetal.bundle
CC="xcrun -sdk iphoneos clang -arch arm64 -target arm64-apple-ios16.0 -O1 -Wall"
$CC -bundle -fobjc-arc -framework Foundation -o out/LavenderMetal.bundle/LavenderMetal LavenderMetal.m
cp LavenderMetal-Info.plist out/LavenderMetal.bundle/Info.plist
codesign -s - -f out/LavenderMetal.bundle
$CC -fobjc-arc -framework Foundation -framework Metal -framework IOKit -framework CoreFoundation -o out/lavmetal_test lavmetal_test.m
codesign -s - -f out/lavmetal_test
echo built
