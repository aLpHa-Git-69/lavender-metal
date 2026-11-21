# Plan

Goal: replace the CPU compositor with the Adreno 512, through a Metal-compatible driver, on the lavender-ios16 port.
Success is measured, not assumed: `backboardd` CPU and presented fps at the idle lock screen (today ~150-170 % of 400 %,
median ~13 fps), and whether wallpapers that need Metal (Astronomy) run.

Each milestone is testable on its own and ends with a number or a log line, not a feeling.

## M0 - Reconnaissance (no flashing)

- [ ] Extract the GPU firmware from the stock ROM (`vendor.img`, `NON-HLOS.bin`): CP/PFP microcode, GPMU, `a512_zap`.
- [ ] Decode the DTB GPU node (`kgsl@5000000`): registers, IRQ, clocks, regulators, SMMU context banks, power levels.
- [ ] Which firmware files the Linux/Mesa driver loads for a512, and the chip id it reports.
- [ ] Does Mesa's `freedreno_devices.py` carry a512 (needed by the ir3 compiler path)?
- [ ] Read how hakios splits `hakios.adreno.c` (bring-up) from `hakios.gpu.cpp` (IOKit service) and which parts are chip
      independent (buffers, fences, SMMU page tables, submit).
- [ ] What the port already provides: SMMU, clocks, regulators, SCM calls (zap needs a secure call).
- Done when: `docs/HARDWARE-A512.md` lists every resource the GPU needs and where each one comes from.

## M1 - Power and identify

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
