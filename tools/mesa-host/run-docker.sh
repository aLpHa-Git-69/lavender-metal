#!/bin/sh
# usage: run-docker.sh OUTDIR   (OUTDIR a Docker-visible folder, e.g. F:/port_ios/work/mesa-host)
H=${HAKIOS:-F:/port_ios/refs/hakios/components/metal/plugin/tools/mesa}
MSYS_NO_PATHCONV=1 docker run --rm -v "$H:/h:ro" -v "$1:/out" -v "$(cd "$(dirname "$0")" && pwd -W):/s:ro" debian:12 sh -c \
 'apt-get update -qq && apt-get install -y -qq build-essential bison flex python3 python3-venv python3-dev libdrm-dev curl xz-utils patch pkg-config zlib1g-dev >/dev/null && bash /s/build.sh'
