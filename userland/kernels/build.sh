#!/bin/sh
# lavk.metallib for iOS 16 (needs Xcode's Metal Toolchain: xcodebuild -downloadComponent MetalToolchain)
set -e
cd "$(dirname "$0")"
xcrun -sdk iphoneos metal -std=metal3.0 -mios-version-min=16.0 -c lavk.metal -o lavk.air
xcrun -sdk iphoneos metallib lavk.air -o lavk.metallib
ls -la lavk.metallib
