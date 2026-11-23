#!/bin/bash
# Inside the container: Mesa 25.0.7 + hakios's AIR front end (MIT, see docs/REFERENCES.md) -> air2nir and ir3_compiler.
# /h = hakios tools/mesa (read-only), /out = where the sources and the build go.
set -euo pipefail
VERSION=25.0.7
SHA256=592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6
cd /out
[ -f mesa-$VERSION.tar.xz ] || curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
echo "$SHA256  mesa-$VERSION.tar.xz" | sha256sum -c --quiet
if [ ! -d mesa-$VERSION ]; then
	tar xJf mesa-$VERSION.tar.xz
	for p in air-frontend.patch air-ir3.patch ir3-round-default.patch ir3-shift-split.patch ir3-ssbo-subdword.patch; do
		patch -s --no-backup-if-mismatch -d mesa-$VERSION -p1 < /h/$p
	done
	cp -r /h/air mesa-$VERSION/src/compiler/air
fi
[ -x venv/bin/python3 ] || { python3 -m venv venv; venv/bin/pip -q install mako pyyaml packaging meson ninja; }
export PATH=/out/venv/bin:$PATH
[ -f build/build.ninja ] || meson setup build mesa-$VERSION --buildtype=debugoptimized \
	-Dplatforms= -Dgallium-drivers=softpipe,freedreno -Dfreedreno-kmds=msm \
	-Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled -Dllvm=disabled \
	-Dopengl=true -Dgles2=enabled -Dshader-cache=disabled -Dvalgrind=disabled \
	-Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= \
	-Dgallium-vdpau=disabled -Dgallium-va=disabled -Dgallium-xa=disabled \
	-Dlmsensors=disabled -Dbuild-tests=false -Dzstd=disabled -Dxmlconfig=disabled \
	-Dexpat=disabled > meson-setup.log 2>&1
ninja -C build src/compiler/air/air2nir src/gallium/drivers/freedreno/ir3_compiler > ninja.log 2>&1
echo built; ls -la build/src/compiler/air/air2nir build/src/gallium/drivers/freedreno/ir3_compiler
