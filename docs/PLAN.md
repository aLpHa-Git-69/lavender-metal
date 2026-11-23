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
Step (c) built 2026-11-22: `tools/mesa-ios/build-mac.sh` cross-builds Mesa 25.0.7 + the front end for arm64 iOS 16 on the Mac with Xcode's clang (it first builds bison 3.8.2, Apple's is 2.3; meson/ninja from pip; the recipe is hakios's mk-mesa.sh), 13 static archives (libair, ir3, nir, util...) in about 25 minutes; `ios-smoke.sh` links `compiler-smoke.cc` (hakios's, plus `--gpu-id`) against them into an iOS Mach-O. RUN on the phone 2026-11-22 (shim 0340, `docs/m53-phone-smoke.txt`): device FD512 gen 5, six of seven hakios compute kernels compile on the phone for gpu 512 (fill_id gives the same 64 words, instrlen 2, constlen 12 as the host); `clz_id` aborts in Mesa's ir3 for gen 5 (a hakios fixture built for an A6xx; it will matter only if a kernel uses `clz`, which is to be looked at when it shows up).
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

### M5.3 step (d) done: Metal compute on the phone (22 Nov 2026)

`userland/LavenderMetal.m` now serves `newLibraryWithData:/URL:/File:`, `newFunctionWithName:`, `newComputePipelineStateWithFunction:` (+ options, descriptor, handler forms) and `computeCommandEncoder` (`setComputePipelineState:`, `setBuffer:`, `setBuffers:`, `setBytes:`, `dispatchThreadgroups:`, `dispatchThreads:`). The compiler (`lavcc.cpp`: hakios AIR front end + Mesa ir3, gpu 512) runs inside the plug-in; `lava5.c` builds the A5xx stream (Mesa fd5 order, headers in `userland/a5xx`), code in a per-pipeline buffer loaded indirect, constants (UBO ranges from a CPU snapshot, immediates, driver params), SSBO table, and a final CACHE_FLUSH_TS. A dispatch is a command-buffer op run synchronously at commit.

Phone result (shim 340, `lavmetal_test lavk.metallib`, log in `docs/m53d-phone-compute.txt`): PASSED. fill_id (1024 words, seed by setBytes), saxpy (dispatchThreads, 512 floats), and two chained `square` dispatches in one command buffer are all exact.

Refused at pipeline creation, by name in the log: textures/images/samplers, threadgroup memory, private memory, constant data tables. `dispatchThreads` rounds the grid up to whole groups. Next: M5.4 render pipeline.

## M5.4 (a) done: A5xx render stream, triangle on the phone

`lavgpu_tri` (userland/lavgpu_tri.c) draws a 64x64 BGRA target with the Mesa-ir3 compiled `tri_vs`/`attr_vs` + `tri_fs`
through `lav5_build_render` (lava5r.c, sysmem/bypass, one draw): both draws PASSED (1352 covered pixels, red/green/blue
corners, y flipped, no hang). Output in docs/m54a-phone-triangle.txt.

Bug found on the way: the vertex-id-only VS reported no vertex id register (hakios looked up SYSTEM_VALUE_VERTEX_ID but ir3
emits VERTEX_ID_ZERO_BASE); `compileStage` now falls back to the zero-base one. Next: textures/render pass API, render
pipeline + encoder in the plug-in (LavenderMetal.m).

## M5.4 (b) done: Metal render through the plug-in

Plug-in classes (userland/LavenderRender.inc, LavenderRenderReg.inc): textures (2D RGBA8/BGRA8, CPU-visible, linear, pitch
aligned to 64), render pipeline states (vertex+fragment compiled and linked by lavcc, attributes from the vertex descriptor:
float..float4, uchar4Normalized), render command encoder (pipeline, vertex/fragment buffers and bytes, viewport, scissor,
cull, winding, drawPrimitives with instances; clear load action by the CPU). Each draw is one lava5r stream at commit.
lavmetal_test stages 12-13 (tri_vs by vertex id; attr_vs through a vertex descriptor) PASSED on the phone: 1352 pixels, correct
corner colours. Output in docs/m54b-phone-metal-render.txt.
Not served yet: sampling textures, depth/stencil, blending, indexed draws, points, MSAA, load/store actions beyond clear, other
pixel formats. Next: M5.5 present (CAMetalLayer drawables), M5.6 CoreAnimation acceptance.

## M5.5 done (2026-11-22): CAMetalLayer drawables and present, in a process

`nextDrawable` of a `CAMetalLayer` whose device is ours asks `newTextureWithDescriptor:iosurface:plane:`; the plug-in gives a
texture with its own contiguous memory (the GPU cannot reach an IOSurface's scattered pages) tied to the IOSurface, and at the end
of the command buffer copies its rows into the IOSurface (Load actions copy the other way); `presentDrawable:` (and `atTime:`,
`afterMinimumDuration:`) call `[drawable present]` once the work and the copy are done. lavmetal_test stage 14 PASSED on the phone:
the IOSurface holds the clear colour and the triangle (1352 pixels, correct corners); no unimplemented message was logged.
Output: docs/m55-phone-present.txt. Cost: one CPU copy of the frame per present (about 4 MB at 720x1440), to be measured in M5.6.
What this does not prove: that backboardd/CoreAnimation's compositor shows such a surface on the panel; that is M5.6.

## M5.6 reconnaissance (2026-11-22): what CoreAnimation's own shaders need

`userland/lavsurvey` (CPU only) compiles every function of a .metallib with the plug-in's compiler and says what it uses.
Against the phone's `/System/Library/Frameworks/QuartzCore.framework/default.metallib` (docs/m56-survey-quartzcore.txt):
**135 of 141 functions compile** (ir3 machine code). 71 use no textures, 43 use one, 16 two, 2 three. The 6 that do not: 4 tile
shaders (`tile_downsample_*`, imageblocks; CoreAnimation only asks for them when the device claims tile-shader support, which
we do not) and 2 fragment functions reading framebuffer attachments through slots past 16 (`fixed_frag_lph_cp*`).
So the compiler is not the gap; the driver is: the render builder (lava5r.c) has no texture/sampler state, no blending, and
the encoder no sampler/texture binding. Order of work: (1) fragment textures + samplers (fd5_texture.c: TEX_CONST/TEX_SAMP,
CP_LOAD_STATE4), `setFragmentTexture:`/`setFragmentSamplerState:`, `newSamplerStateWithDescriptor:`; (2) blending from the
pipeline's colour attachment (blend state proven on hardware in M4.6); (3) the rest CoreAnimation needs found by running its
pipelines through the plug-in (log what is refused); only then (4) enabling the device for backboardd, which changes what
CoreAnimation picks at boot and must be done behind a boot-arg with the CPU compositor as the fallback.

## M5.6 (a) done (2026-11-22): fragment textures, samplers and blending through Metal

lava5r.c emits the fragment stage's sampler and texture state (CP_LOAD_STATE4 into SB4_FS_TEX: TEX_SAMP 4 dwords, TEX_CONST 12 per
unit, linear 8888, BGRA via swizzle; TPL1_FS_TEX_COUNT) and the blend state of target 0 (RB_MRT_CONTROL BLEND/BLEND2,
RB_MRT_BLEND_CONTROL, SP/RB_BLEND_CNTL, constant colour). The plug-in has samplers (`newSamplerStateWithDescriptor:`; nearest/
linear, mip filter, repeat / clamp / mirror), `setFragmentTexture:`, `setFragmentSamplerState:`, `setBlendColor`, and reads the blend
factors/operations and write mask from the pipeline's colour attachment. lavmetal_test stages 15 (a 4x4 RGBA8 texture sampled
nearest into a BGRA target: four quadrant colours exact) and 16 (SRC_ALPHA/ONE_MINUS_SRC_ALPHA over blue: centre 128/0/127) PASSED
on the phone; output docs/m56a-phone-sampling-blend.txt. Texture and sampler indices equal Metal's [[texture(n)]]/[[sampler(n)]].
One bug on the way: TEX_CONST_3 ARRAY_PITCH must be a multiple of 4096 (the generated macro asserts).
Not yet: constexpr (inline) samplers, vertex-stage sampling, mip chains, other pixel formats (CoreAnimation uses many), depth,
indexed draws, MSAA, loads from the target (LoadAction.load on non-surface targets works since memory is kept), the render target as
a sampled texture in the same pass. Next: run QuartzCore's own pipelines through the plug-in and log what is refused.

## M5.6 (b) (2026-11-22): what the driver still refuses of QuartzCore's shaders

lavsurvey now also asks the render builder's per-stage rules (`lav5_refuse_stage`); docs/m56b-survey-driver-refusals.txt.
Of the 135 stage functions that compile, **70 are refused by the driver** (65 are usable as they are):
- 32 vertex functions read device buffers (SSBO): the whole `path_*_vert` family and `fixed_vert_*` (their vertex data is not in a
  vertex descriptor). Needs SSBO in graphics stages: descriptors for the VS (SB4_SSBO, as emit_ssbos does for compute) and the
  address arithmetic ir3 already generated.
- 34 fragment functions write 0, 2 or 3 colours (`path_*_frag`, `*_shading_frag`, `inplace_copy`, `attachment_clear_frag`,
  `buffer_image_blit_frag`): multiple render targets (and 0 = depth/stencil-only). Needs MRT in the builder, the pass and
  the encoder, and the other formats those targets use.
- 4 vertex functions (`fixed_vert_*`) write a point size or clip distances.
Usable today: the standard layer pipeline pieces without those (`std_vert0/1`, blur, upsample, downsample, luma, p3 clamp, ...).
Order: SSBO in the VS, then MRT, then point size/clip; each checked with a small Metal test like stages 12-16 before the device
is enabled for backboardd.

## M5.6 (c) done (2026-11-22): device buffers (SSBO) in the vertex stage

lava5r.c loads the shared SB4_SSBO table (sizes, then addresses; slot i = Metal buffer index i, a hole address for an unbound
slot) and sets HLSQ_VS_CNTL/FS_CNTL SSBO_ENABLE when the stage uses them; the refusal is gone (limit 16). Mesa's fd5 does not
advertise SSBOs for the VS ("one state block shared by VS/HS/DS/GS/FS", simplification), the hardware does: lavmetal_test stage 17
(`ssbo_vs` reading positions and colours from a `device float4 *`) PASSED on the phone, 1352 pixels, same corners. docs/m56c-phone-ssbo-vs.txt.
This clears the 32 vertex functions of QuartzCore's path_* and fixed_vert_* families (they still need MRT in their fragment
partners, and the 4 fixed_vert_* with point size/clip). Next: MRT.

## M5.6 (d) done (2026-11-23): multiple render targets (MRT)

lava5r.c now serves fragment stages that write 0..8 colours: per-target RB_MRT_BUF_INFO/pitch/address (swap per format),
SP_FS_MRT_REG, SP_FS_OUTPUT_REG only for written targets, RB/SP_FS_OUTPUT_CNTL.MRT(nrt) with nrt = attachments of the pass,
per-target blend (INDEPENDENT_BLEND when nrt > 1) and RB_RENDER_COMPONENTS RT0..RT(nrt-1) (without it only target 0 is written).
The plug-in takes colour attachments 0..7 (same-size textures of this driver; a clear per attachment) and blend/write mask per
attachment from the pipeline descriptor. lavmetal_test stage 18 (`mrt_fs`: red, green, uv into three targets) PASSED on the phone.
docs/m56d-phone-mrt.txt. Clears the 34 MRT fragment functions of QuartzCore. Next: point size / clip distances (4 fixed_vert_*).

## M5.6 (e) done (2026-11-23): point size (and clip distances, untested)

The vertex stage may now write `[[point_size]]`: PC_PRIM_VTX_CNTL.PSIZE, psize_loc in VPC_PACK (hk's link already placed it), point
size max 4092 (1 without a point size). The plug-in serves `MTLPrimitiveTypePoint` (prim 9). lavmetal_test stage 19 (`point_vs`,
size 8): exactly 64 white pixels at x 28..35, PASSED on the phone (docs/m56e-phone-point-size.txt). Clip/cull distances are wired
in lava5r.c like Mesa's fd5 (slots 17/18 after position and point size, GRAS_VS_CL_CNTL, VPC_CLIP_CNTL, PC_CLIP_CNTL) but UNTESTED:
the hk front end refuses `[[clip_distance]]` arrays ("no varying for an array of N"). QuartzCore's four fixed_vert_* need only the
point size, so clip distances are not on the path.
Loader limit found: with the unstripped binary at 8.28 MB (8,284,704 bytes) Metal returned no device and the plug-in never ran (8,268,352
loaded); `strip -x` (7.9 MB) fixes it, so build-lavmetal.sh strips before signing.
Next: run QuartzCore pipelines through the plug-in, then enable the device for backboardd behind a boot-arg.

## M5.6 (f) done (2026-11-23): pair survey of QuartzCore (CPU: compile, link, driver checks)

`lavsurvey lib pairs` compiles every stage once, links every vertex function with every fragment function and runs the driver's
stage checks (docs/m56f-survey-pairs.txt). QuartzCore default.metallib: 3627 pairs, 2493 usable as they are, 762 do not link (the
fragment reads a varying the vertex does not write: not a pair CoreAnimation would make), 372 refused by the driver, ALL of them
the four `fixed_vert_*` (they use private memory). So every other vertex function (35) and fragment function can be drawn with the
driver today; what is left is private memory for fixed_vert_*, and the two `fixed_frag_*_cp*` (render-target slots past 16).
This survey is CPU only (it needs no GPU: it asks whether a pipeline can be built). The drawing tests that run on the A512 are
lavmetal_test's stages. Next: draw real QuartzCore pipelines on the GPU through the Metal API.

## M5.6 (g) done (2026-11-23): QuartzCore's own pipelines drawn on the A512 through the plug-in

`lavqc_test default.metallib 3` (userland/lavqc_test.m; docs/m56g-phone-quartzcore-draws.txt): for each vertex function up to 3 fragment
functions whose pipeline the plug-in makes, drawn once into three 64x64 targets with every buffer slot on zeroed memory and every texture
slot on a 4x4 one. 117 pipelines made, 105 drawn to completion on the GPU (no hang, no fault: 39 vertex and 7 fragment functions),
12 refused at draw time because they are the fixed_vert_* (private memory), 86 pairs do not link (not pairs CoreAnimation makes).
This runs the shaders, it does not yet check the pixels (the buffers are zero, there is no CoreAnimation data).
Bugs the sweep found in the plug-in: command buffers and encoders were returned retained (ARC callers never released them, so every
pipeline leaked its two service buffers and the service's 64 buffers ran out after ~28 pipelines) -> now autoreleased; and
_MTLCommandQueue's own dealloc crashes (dispatch_release(NULL)) once queues really die -> own dealloc that skips it.
Still open for backboardd: the service's LAV_BUFFERS = 64 (kernel/LavenderGPU.cpp) is far too few for CoreAnimation (textures, buffers and
pipelines all take one), private memory for fixed_vert_*, fixed_frag_*_cp* (RT slots past 16).

## M5.6 (h) done (2026-11-23): buffer suballocator in the plug-in

The service holds 64 buffers for all processes; lavBufNew now carves page-sized (4 KB) pieces out of slabs and only gives a buffer of its
own to a request of half a slab or more (and to the stream buffer). New lavmetal_test stage 21: 300 live buffers, each with its own
bytes, all zero when new, free half and allocate again: PASSED on the phone together with every earlier stage.
FINDING (the real blocker for backboardd): the GPU needs physically contiguous memory below 4 GB, and after boot almost none is
left. The service answers kIOReturnNoMemory (0xe00002bd) for 8 MB and 2 MB at once, then for 512 KB after 5 of them; the pool ends
up with 5 x 512 KB + 20 x 128 KB = 5.1 MB in total, and a 6 MB buffer is not available. A full-screen drawable or layer texture is
~10 MB, so it cannot be allocated like this. The fix is a contiguous region reserved at boot (carve-out in the shim's memory map or an
early IOKit reservation, in kernel/LavenderGPU.cpp / the shim) that the service hands slabs out of; or scatter-gather through the
GPU's SMMU (the page table is currently an identity of the first 4 GB). Until then only small surfaces work.

## M5.6 (i) done (2026-11-23): CoreAnimation's CARenderer draws on the Adreno

The contiguous-memory blocker of (h) is gone: the service now takes its big pool from StartIOKitMatching in its own kernel thread
(lavender-ios16 patches 0345/0346), so 8 MB and larger buffers exist after boot (phone shim 0348, built on the Mac).
userland/lavca_test: a CALayer tree (red root, green and blue children) rendered by CARenderer into a 256x256 BGRA texture through the
plug-in, read back: PASSED (docs/m56h-phone-carenderer.txt). CoreAnimation colour-matches, so red reads ff2600, green 00f900, blue 0433ff.
What it needed: queue completionQueue, newPipelineLibraryWithFilePath: refused (default.pipelinelib), newFunctionWithDescriptor:,
20 texture slots (fixed_frag_lph_cph samples 16 and reads its target as a 17th), half vertex formats, indexed draws, and private memory
for a5xx render stages (per-stage PVT_MEM buffer; fibers per SP is a guess: 4096, 2 SPs).
lavqc_test on QuartzCore's default.metallib: 78 drawn, 0 failed (was 70/8), 83 without a pipeline (varying slots 32-34), docs/m56h-phone-quartzcore-draws.txt.
The 83 "no pipeline" are lavqc_test pairing every vertex function with every fragment function: in each the fragment stage reads a varying slot (32, 33 or 34) the vertex stage never writes, which Metal rejects as well, so they are invalid pairs, not a gap (CoreAnimation only builds valid pairs, as lavca_test shows).
Open for backboardd: clip distance, per-fiber guess for private memory, then enabling the device for
backboardd behind a boot-arg with the CPU compositor fallback and measuring CPU/fps at the lock screen.
