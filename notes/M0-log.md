# M0 log

2026-11-21
- Project created. Reference: hakios-xnu-kernel @ 63fc2b4 (refs/hakios, sparse).
- Stock ROM vendor.img (sparse) -> raw with tools/simg2raw.py; firmware dumped with tools/extract-gpu-fw.sh:
  a530_pm4.fw, a530_pfp.fw, a530(v3)_gpmu.fw2, a530v{2,3}_seq.fw2, a512_zap.{mdt,b00,b01,b02,elf}.
- DTB read with tools/dtb_dump.py: kgsl-3d0 @0x05000000, chipid 0x05010200, SPI 300, gpucc @0x05065000,
  GDSC cx @0x05066004 / gx @0x05066094, kgsl SMMU @0x05040000, zap pas-id 13. See docs/HARDWARE-A512.md.
- Open for M0: which a530 firmware files a512 uses; what the port already provides (clocks, SMMU, SCM);
  split of hakios.adreno.c vs hakios.gpu.cpp; Mesa freedreno_devices.py entry for 0x05010200.

2026-11-21 (later)
- Linux a5xx_catalog.c (read-only reference): chip 0x05010200 -> a530_pm4.fw, a530_pfp.fw, zapfw a512_zap.mdt,
  gmem 256K+16K, quirk LMLOADKILL_DISABLE; no GPMU firmware (only 530 has one).
- Mesa freedreno_devices.py: GPUId(512) with 509, num_sp_cores 2, highest_bank_bit 14, reg_size_vec4 96.
- hakios: hakios.gpu.cpp <-> hakios.adreno.c only through hakios_gpu_* (ready, parameter, address, address_free, map,
  mapped, unmap, to_memory, submit, done, last, worked, report, stop).
- Port: TopazModem.cpp has SCM PAS (smc #0, PIL service 2): reusable for the zap shader.
- M0 closed. Next: M1 (read-only power-up and chip id), see docs/PLAN.md.
