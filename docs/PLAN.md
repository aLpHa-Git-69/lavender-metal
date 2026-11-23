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

## M1 - Power and identify - DONE 2026-11-21 (patches 0236-0240 in the port; GX powers, GPU block read, TZ knows PAS 13)

- XNU driver `lavender.gpu` (boot-arg gated, off by default): enable GPU clocks and rails, map the register block,
  read the chip id / CP version / RBBM status. Read-only first.
- Done when: the live log prints the Adreno chip id and revision from the real hardware, with no hang and no change to
  boot time.

## M2 - Ring, microcode, fence - DONE 2026-11-21 (patches 0241-0248: zap via SCM, SMMU stage 1 identity table, PM4/PFP, ring, ME_INIT, secure mode off, fence x16, hang + reset recovery)

- Load PM4/PFP, set up the ring buffer in the GPU SMMU, load the zap shader through SCM, start the CP.
- A command stream that makes the CP write a value to memory (the fence), polled from the CPU.
- Done when: a CP-written word is read back, repeatably, and a deliberate hang is recovered (reset) without a reboot.

## M3 - A5xx register layer and compute - DONE 2026-11-21 (64 threads, buffers in and out, 64 of 64 outputs match the host value on the device)

- `a5xx` register/packet layer generated from Mesa's `a5xx.xml` and checked against it (as hakios checks a6xx).
- Fake GPU (interpreter) for host tests so most work does not need a flash.
- Compute dispatch from the fd5/computerator sequence; one real kernel run and compared to a host reference.
- Done when: a kernel's output on the phone is identical to the host reference run.

## M4 - Render - DONE 2026-11-22 (patches 0316-0318 in the port; depth parked, optional)

- M4.0 DONE 2026-11-21: 2D blit (CP_BLIT) copies a buffer on the device. M4.1 DONE 2026-11-21: RGBA8 rectangle copy, pixel exact. M4.3 DONE 2026-11-21: first sysmem 3D draw (TRILIST, hand-assembled VS/FS, 64x64 RGBA8 linear target), 351/351 pixels correct; key bug was RB_MRT_PITCH/ARRAY_PITCH being shr 6. 2D solid fill parked. M4.4 DONE 2026-11-21: the FS samples a 4x4 RGBA8 linear texture (cat5 sam, nearest, clamp to edge) and the right texel lands in the target. M4.5 DONE 2026-11-22: interpolated texture coordinates (uv attribute through the VPC, `bary.f` with IJ_PERSP_PIXEL) select the right texel per pixel, 213/213. The bug that cost two flashes: the VS marker tail used r1.x-z and clobbered the uv sitting in r1. M4.6 DONE 2026-11-22: colour blending (SRC_ALPHA/ONE_MINUS_SRC_ALPHA, ONE/ZERO, ONE/ONE) over a known background, 351/351; blend factor enum is SRC_ALPHA 6, ONE_MINUS_SRC_ALPHA 7, DST_COLOR 8, ONE_MINUS_DST_COLOR 9. M4.7 DONE 2026-11-22: the GPU's textured/varying triangle and the blended triangle are on the physical panel (kernel copies the GPU-rendered 64x64 targets 4x into the scanout at 0x9d400000; the RB cannot target it directly because stride 4320 is not a multiple of 64); channel order a8r8g8b8 confirmed.
- Not done in M4: depth (only if CoreAnimation needs it), 2D solid fill.

## M5 - The Metal device

- M5.0 DONE 2026-11-22: `LavenderGPU` IOKit service + `LavenderGPUUserClient` (kernel/LavenderGPU.cpp, started by lavgpu=30/31 after the CP runs), merged as patch 0319. Verified on device twice: the in-kernel selftest (lavgpu=31) and the userland client (userland/lavgpu_test.c, built on the Mac) both PASSED: info, buffer new (contiguous, below 4 GB) + IOConnectMapMemory64, an 8-byte stream and a 6 KB stream written into the buffer by the GPU, read back by the program, buffer free. Selectors: 0 info, 1 buffer new, 2 buffer free, 3 submit (inline), 4 submit by reference from a client buffer. Two findings: the fence must be a CP_MEM_WRITE (CACHE_FLUSH_TS to the rptr page never landed), and IOKit's large struct-input path (over 4 KB, memory descriptor) hung the whole phone, so long streams go by reference (selector 4); that hang is not root-caused. No address validation yet (bring-up only). Next after that: depth (optional), the MTLDevice plug-in.
- Done when: the idle lock screen costs a fraction of today's CPU and Astronomy renders (M5.6).

M5.1 DONE 2026-11-22 (patch 0320; shim 0340): `MTLCreateSystemDefaultDevice()` returns our `LavenderMetalDevice` on the phone. Selector 5 of the service sets/removes the plug-in properties on the IOAcceleratorES service (on demand, so other processes never see the device); the bundle `LavenderMetal.bundle` (userland/, built on the Mac, installed in /System/Library/Extensions) makes the class at load time as a subclass of Metal's private `_MTLDevice`. What Metal insisted on, in order: the class is a `_MTLDevice` subclass, `-initWithAcceleratorPort:` answers, and the object conforms to the protocols `MTLDevice` and `MTLDeviceSPI` (each missing one aborts the process with a failed assertion in `MTLAddDevice`). Metal's first questions after that: `-featureProfile`, `-maxBufferLength`; `newCommandQueue` answers nil. The test program takes the properties away even when Metal aborts it (a signal handler).

M5.2 DONE 2026-11-22 (userland only, no new kernel patch; shim 0340): buffers, command queue, command buffer and blit encoder through Metal's own objects, checked byte by byte on the phone by `lavmetal_test` (8 stages). Buffers come from the service (contiguous, mapped; `gpuAddress` is the GPU's address); `fillBuffer:range:value:` is written by the GPU's command processor (CP_MEM_WRITE, an unaligned head/tail by the CPU); `copyFromBuffer:...` runs the 2D core (the step-18 CP_BLIT sequence) over the 64-byte aligned middle in 4 KB blits, 8192 bytes exact, and the CPU copies an unaligned head/tail (59 + 37 bytes in the test; a copy whose two ends are not alike mod 64 is done entirely by the CPU, logged). A command buffer runs at commit, op by op, each op a synchronous submission by reference (selector 4); status, completed/scheduled handlers and operations in order work. Metal's only unanswered question so far: `-featureProfile`. Classes are made at load time on Metal's private bases (`_MTLResource`, `_MTLCommandQueue`, `_MTLCommandBuffer`, `_MTLCommandEncoder`), with the protocols (and their SPI twins) declared, and a protocol-driven stub that answers any other message with zeros of the right size and logs it once. Not yet: textures, samplers, compute and render encoders, libraries, heaps, events, fences; the GPU-side copy needs matching 64-byte alignment of both ends.

### M5.3 compiler decision (2026-11-22): Mesa ir3, with hakios's AIR front end

Chosen route (3 above): AIR -> NIR by hakios's front end (`components/metal/plugin/tools/mesa/air/`, MIT, 7k lines of `air_to_nir.c` plus bitcode reader, tested on 456 system metallib modules), NIR -> Adreno machine code by Mesa's ir3 (MIT), both in one library that runs in the plug-in on the phone. hakios carries it as patches to Mesa 25.0.7 (`air-frontend.patch`, `air-ir3.patch`, `ir3-round-default/shift-split/ssbo-subdword.patch`) so that is what we build; notices stay (docs/REFERENCES.md).
What does NOT transfer: hakios targets an A6xx (7c Gen 3); the dispatch and draw state (`a6xx.hpp`, `compute.cpp`, `render.cpp`) are A6xx register layouts. The A512 is gen 5: ir3 itself supports gen 5 (`compiler->gen = 5`, chip id from the gpu id 512), but the CS/3D state and the descriptors must be written from Mesa's `fd5_*` (gallium) as a second witness, and checked on the phone like the 2D core was.
Step (a) DONE 2026-11-22: `tools/mesa-host/run-docker.sh OUT` builds Mesa 25.0.7 + the front end in a Debian container (about 20 minutes, 16 cores); `ir3_compiler -g 512 -v fill_id.bc` (hakios's fixture bitcode) compiles a compute kernel to A5xx machine code (stgb.untyped store, driver params at c0 and UBO ranges at c4, constants at c8). Next is (b), the A5xx dispatch.
Step (b) DONE 2026-11-22 (shim 0340): `tools/a5cs/a5cs_gen.c` writes an A5xx compute dispatch (Mesa's fd5_compute/fd5_emit order, registers from Mesa's generated headers) for the ir3-compiled `fill_id`; `userland/lavgpu_a5cs` runs it through selector 4: 256 of 256 words correct (out[i] = i*0x9e3779b1 + seed), the shader code loaded either inline or from memory. Finding: the stores sit in the GPU's cache until `CP_EVENT_WRITE` with `CACHE_FLUSH_TS` (4 dwords: event, address lo/hi, value) runs; with only `CP_WAIT_FOR_IDLE` and `CACHE_FLUSH` the output stayed untouched, though every CP_MEM_WRITE marker of the stream landed. So any stream whose shader writes memory must end with that flush before its fence. Next is (c): the compiler library for iOS and the Metal side.
Step (c) built 2026-11-22: `tools/mesa-ios/build-mac.sh` cross-builds Mesa 25.0.7 + the front end for arm64 iOS 16 on the Mac with Xcode's clang (it first builds bison 3.8.2, Apple's is 2.3; meson/ninja from pip; the recipe is hakios's mk-mesa.sh), 13 static archives (libair, ir3, nir, util...) in about 25 minutes; `ios-smoke.sh` links `compiler-smoke.cc` (hakios's, plus `--gpu-id`) against them into an iOS Mach-O. Not yet run on the phone.
Steps: (a) host build of Mesa + front end in Docker (`tools/mesa-host/`), prove `ir3_compiler --gpu-id 512` turns a fixture AIR into A5xx code; (b) a tiny A5xx compute dispatch (CP_EXEC_CS, SP_CS_*) by hand with ir3 code from (a), run on the phone through the service (selector 4), numbers against a host reference; (c) iOS arm64 build of the compiler library, linked into LavenderMetal; (d) `newLibraryWithData:` / `newFunctionWithName:` / `newComputePipelineState` / compute encoder.

## M5.x - the MTLDevice plug-in (plan, 2026-11-22; reference: hakios-metal + hakios docs/metal-plan.md, behaviour only)

How Metal finds a driver (as hakios does it): an IOService carries the properties `MetalPluginName` and `MetalPluginClassName`; Metal.framework loads the bundle `/System/Library/Extensions/<name>.bundle` and instantiates the named subclass of Metal's private `_MTLDevice`; `MTLCreateSystemDefaultDevice()` then returns it. Our `LavenderGPU` service can carry the properties behind a boot-arg (`lavmetal=1`, off by default: a device that exists and cannot draw changes what CoreAnimation picks).

Three parts, as hakios splits them: (A) the shader compiler, AIR (`.metallib`) into the A5xx ISA; (B) the device bundle (ObjC, a `_MTLDevice` subclass and the objects it hands out, over a C core); (C) the kernel side, which is done (M5.0).

| Step | What | Needs the compiler | Checked by |
|---|---|---|---|
| M5.1 | A device that exists: bundle + properties; `MTLCreateSystemDefaultDevice()` returns our class; log every message Metal sends that we do not answer | no | a small test program on the phone (built on the Mac) |
| M5.2 | Buffers and textures over service buffers (`IOConnectMapMemory64`), queue, command buffer, blit encoder (fill/copy done by CP packets) | no | write, copy, read back through Metal objects |
| M5.3 | Libraries (`newLibraryWithData:`), the compiler for compute kernels, compute encoder, commit/wait on the fence | yes | a kernel's numbers against a host reference |
| M5.4 | Render pipeline + render encoder: sysmem pass, our own ladder state (varyings, blend, the RT) | yes (VS+FS) | a triangle through Metal |
| M5.5 | Present: a drawable the compositor can hand to the panel | yes | frames on screen |
| M5.6 | CoreAnimation acceptance: PosterBoard preview, Astronomy | yes | the done-when below |

The open decision is the compiler (part A): (1) our own small AIR-to-A5xx backend for just the shaders CoreAnimation uses (hakios's sweep: 39/39 vertex, 95/95 fragment functions of CoreAnimation compile through its front end; ours would be far smaller than a general one); (2) port Mesa's NIR/ir3 into a userland library (MIT, so it can be reused with its notices kept as docs/REFERENCES.md says; large, and a build of its own; only GPL code such as Linux msm is reading-only); (3) reuse hakios's AIR front end plus Mesa's ir3 as a dependency. M5.1 and M5.2 do not depend on it.

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
