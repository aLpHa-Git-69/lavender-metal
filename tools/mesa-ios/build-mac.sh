#!/bin/bash
# Mesa's shader compiler (hakios's AIR front end + ir3) built for arm64 iOS 16 on the Mac, with Xcode's clang and the
# iPhoneOS SDK. The recipe is hakios's tools/mk-mesa.sh (MIT) adapted to this machine: Apple's bison is 2.3, so a newer
# one is built first; meson, ninja, mako and pyyaml come from pip. Everything lives in $B (default /tmp/mi).
#   build-mac.sh PATCHDIR      PATCHDIR = a copy of hakios's components/metal/plugin/tools/mesa
set -euo pipefail
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
P=$(cd "$1" && pwd)
B=${B:-/tmp/mi}
mkdir -p "$B" && cd "$B"
SDK=$(xcrun -sdk iphoneos --show-sdk-path)
CC=$(xcrun -sdk iphoneos -find clang)
CXX=$(xcrun -sdk iphoneos -find clang++)
if [ ! -x tools/bin/bison ]; then
	[ -f bison-3.8.2.tar.xz ] || curl -sL --fail -o bison-3.8.2.tar.xz https://ftp.gnu.org/gnu/bison/bison-3.8.2.tar.xz
	rm -rf bison-3.8.2 && tar xJf bison-3.8.2.tar.xz && (cd bison-3.8.2 && ./configure --prefix="$B/tools" >/dev/null && make -j8 >/dev/null && make install >/dev/null)
fi
export PATH=$B/tools/bin:$B/venv/bin:$PATH
if [ ! -x venv/bin/python3 ]; then
	python3 -m venv venv
	venv/bin/pip -q install meson ninja mako pyyaml packaging setuptools
fi
PATCHES=(air-frontend.patch air-ir3.patch freedreno-darwin.patch darwin-sdk.patch ir3-round-default.patch ir3-shift-split.patch ir3-ssbo-subdword.patch)
STAMP=$(cd "$P" && cat "${PATCHES[@]}" | shasum -a 256 | cut -d' ' -f1)
if [ ! -d mesa-$VERSION ] || [ "$(cat mesa-$VERSION/.hakios-patches 2>/dev/null)" != "$STAMP" ]; then
	[ -f mesa-$VERSION.tar.xz ] || curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
	echo "$SHA256  mesa-$VERSION.tar.xz" | shasum -a 256 -c -s
	rm -rf mesa-$VERSION build && tar xJf mesa-$VERSION.tar.xz
	for p in "${PATCHES[@]}"; do patch -s --no-backup-if-mismatch -d mesa-$VERSION -p1 < "$P/$p"; done
	echo "$STAMP" > mesa-$VERSION/.hakios-patches
fi
rm -rf mesa-$VERSION/src/compiler/air mesa-$VERSION/src/hakios-drm
cp -r "$P/air" mesa-$VERSION/src/compiler/air
cp -r "$P/hakios-drm" mesa-$VERSION/src/hakios-drm
cat > cross.ini <<INI
[binaries]
c = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK', '-target', 'arm64-apple-ios16.0', '-D_DARWIN_C_SOURCE']
cpp = ['$CXX', '-arch', 'arm64', '-isysroot', '$SDK', '-target', 'arm64-apple-ios16.0', '-D_DARWIN_C_SOURCE', '-fuse-cxa-atexit']
objc = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK', '-target', 'arm64-apple-ios16.0', '-D_DARWIN_C_SOURCE']
ar = '/usr/bin/ar'
ranlib = '/usr/bin/ranlib'
strip = '/usr/bin/strip'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[properties]
needs_exe_wrapper = true
INI
[ -f build/build.ninja ] || meson setup build mesa-$VERSION --cross-file cross.ini --wrap-mode=nofallback \
	--prefix=/hakios --libdir=lib --buildtype=release -Db_ndebug=true \
	-Dplatforms= -Dgallium-drivers=softpipe,freedreno -Dfreedreno-kmds=msm -Dvulkan-drivers= -Dglx=disabled \
	-Degl=disabled -Dgbm=disabled -Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled -Dgles1=disabled \
	-Dgles2=enabled -Dopengl=true -Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled \
	-Dvalgrind=disabled -Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-vdpau=disabled \
	-Dgallium-va=disabled -Dgallium-xa=disabled -Dlmsensors=disabled -Dbuild-tests=false > meson-setup.log 2>&1
# hakios's MESA_ARCHIVE_PATHS (tools/kernels.mk) plus libair, spelled out (Apple's awk cannot read the make file's list)
ARCHIVES="src/compiler/air/libair.a src/freedreno/ir3/libfreedreno_ir3.a src/freedreno/isa/libir3encode.a
 src/freedreno/isa/libir3decode.a src/compiler/isaspec/libisaspec.a src/compiler/nir/libnir.a src/compiler/libcompiler.a
 src/freedreno/fdl/libfreedreno_layout.a src/freedreno/common/libfreedreno_common.a src/util/libmesa_util.a
 src/util/libmesa_util_sse41.a src/util/blake3/libblake3.a src/c11/impl/libmesa_util_c11.a"
ARCHIVES=$(echo $ARCHIVES)
echo "$ARCHIVES" > archives.txt
ninja -C build $ARCHIVES > ninja.log 2>&1
for a in $ARCHIVES; do [ -s build/$a ] || { echo "missing build/$a" >&2; exit 1; }; done
echo "built $(echo $ARCHIVES | wc -w) archives in $B/build"
