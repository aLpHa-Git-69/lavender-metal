# Plan

Goal: replace the CPU compositor with the Adreno 512, through a Metal-compatible driver, on the lavender-ios16 port.
Success is measured, not assumed: `backboardd` CPU and presented fps at the idle lock screen (today ~150-170 % of 400 %,
median ~13 fps), and whether wallpapers that need Metal (Astronomy) run.

Each milestone is testable on its own and ends with a number or a log line, not a feeling.

## M0 - Reconnaissance (no flashing) - DONE 2026-11-21

- [x] Extract the GPU firmware from the stock ROM (`vendor.img`): `firmware/` (11 files, SHA256SUMS).
- [x] Decode the DTB GPU node: registers, IRQ, clocks, GDSCs, SMMU context banks, power levels, zap pas-id
      (`docs/HARDWARE-A512.md`, `tools/dtb_dump.py`).
- [x] Which firmware a512 loads: `a530_pm4.fw`, `a530_pfp.fw`, zap `a512_zap.mdt`; **no GPMU firmware**.
- [x] Mesa knows the chip: `GPUId(512)`, 2 SP cores, `highest_bank_bit` 14 (ir3 and `fd5` need no chip port).
- [x] hakios split: the IOKit service (`hakios.gpu.cpp`) is chip independent and talks to the chip driver through ~14 C
      functions; only the chip driver (`hakios.adreno.c`) is rewritten (`docs/ARCHITECTURE.md`).
- [x] What the port provides: SCM PAS code exists (`0113-topaz-modem-pas-boot`), direct GCC/GDSC register access works,
      no SMMU and no RPM/CPR regulator handling yet.
- Result: `docs/HARDWARE-A512.md` and `docs/ARCHITECTURE.md` list every resource and where each one comes from.
- Carried into M1 as questions the hardware must answer: does SDM660 TrustZone take our SCM fast call
  (`IS_SUPPORTED` pas-id 13); are the GX GDSC and the bootloader's rail state enough to read the GPU block; does the
  kgsl SMMU need a TrustZone/secure setup before our context bank is usable.

## M1 - Power and identify (patch 0236 written 2026-11-21, pending device test)

- XNU driver `lavender.gpu` (boot-arg gated, off by default): enable GPU clocks and rails, map the register block,
  read the chip id / CP version / RBBM status. Read-only first.
- Done when: the live log prints the Adreno chip id and revision from the real hardware, with no hang and no change to
  boot time.

## M2 - Ring, microcode, fence

- Load PM4/PFP, set up the ring buffer in the GPU SMMU, load the zap shader through SCM, start the CP.
- A command stream that makes the CP write a value to memory (the fence), polled from the CPU.
- Done when: a CP-written word is read back, repeatably, and a deliberate hang is recovered (reset) without a reboot.

## M3 - A5xx register layer and compute

- `a5xx` register/packet layer generated from Mesa's `a5xx.xml` and checked against it (as hakios checks a6xx).
- Fake GPU (interpreter) for host tests so most work does not need a flash.
- Compute dispatch from the fd5/computerator sequence; one real kernel run and compared to a host reference.
- Done when: a kernel's output on the phone is identical to the host reference run.

## M4 - Render and Metal

- One sysmem draw (fd5 sequence), then textures, blending, depth.
- `MTLDevice` plug-in and the IOKit service the plug-in talks to; `MTLCreateSystemDefaultDevice()` returns a device.
- CoreAnimation over Metal; PosterBoard preview and Astronomy as acceptance tests.
- Done when: the idle lock screen costs a fraction of today's CPU and Astronomy renders.

## Risks (ordered)

1. Zap shader / secure world: the GPU may stay in secure mode without it. Needs an SCM path from XNU.
2. GX power domain on SDM660 (CPR, `gfx_ldo`): wrong sequencing can hang the SoC.
3. SMMU: the GPU context bank must coexist with whatever the port already set up.
4. A5xx has no turnip witness: sequences come from Gallium fd5 and must be validated on hardware.
5. iOS side: which Metal entry points CoreAnimation actually needs (hakios's sweep says 39/39 vertex, 95/95 fragment
   functions of CoreAnimation compile through its front end).

## Rules of work

- Every driver feature is behind a boot-arg and off by default; the stock shim keeps booting.
- One variable per flash; keep the log ring and live log on.
- Prefer the fake GPU and host tests; flash only to answer a question the host cannot.
