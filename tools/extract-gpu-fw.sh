#!/usr/bin/env bash
# Extract the Adreno 512 firmware from the stock ROM's vendor.img.
#   tools/extract-gpu-fw.sh <vendor.raw (see simg2raw.py)> <outdir>
# Needs debugfs (e2fsprogs), e.g. in the port-ios-build container.
set -e
RAW=${1:?vendor.raw}; OUT=${2:?outdir}
mkdir -p "$OUT"
for f in a530_pm4.fw a530_pfp.fw a530_gpmu.fw2 a530v3_gpmu.fw2 a530v3_seq.fw2 a530v2_seq.fw2 \
         a512_zap.mdt a512_zap.b00 a512_zap.b01 a512_zap.b02 a512_zap.elf; do
    debugfs -R "dump /firmware/$f $OUT/$f" "$RAW" >/dev/null 2>&1
done
( cd "$OUT" && sha256sum * > SHA256SUMS && ls -l )
