# Architecture and what is reused from hakios

```
 a Metal program / CoreAnimation
   |  MTLDevice plug-in (HakiosMetal.m style)               userland, MIT, adapted
   v
 hakios-metal core (command.hpp, compute.hpp, render.hpp)   userland, MIT: Metal model -> packets
   |  a5xx register/packet layer  (REWRITE: a6xx.hpp -> a5xx)
   |  AIR -> NIR -> ir3 (Mesa, chip table already has a512)  userland, reused as is
   v
 gpu.hpp   GEM buffers, submit, fences  = the msm DRM requests                  userland, reused
   |  IOKit user client calls (selector = DRM request number)
   v
 HakiosGPU service (hakios.gpu.cpp)                          kernel, APSL, reused (chip independent)
   |  ~14 C functions: hakios_gpu_ready / parameter / address / address_free / map / mapped / unmap /
   |  to_memory / submit / done / last / worked / report / stop
   v
 chip driver (hakios.adreno.c for A642L)                     kernel, REWRITE for the Adreno 512
   |  clocks, GDSC, SMMU, ring, microcode, zap (SCM), CP start, submit, fence
   v
 Adreno 512 (A5xx, no GMU)
```

## The seam

`hakios.gpu.cpp` (the IOKit service and user client) never touches a register. It talks to the chip driver only through the
C functions above, and to userland in the protocol of Linux's msm DRM driver (`DRM_MSM_GET_PARAM`, `GEM_NEW`, `GEM_SUBMIT`,
`WAIT_FENCE`, ... plus three hakios-only requests: user memory, IOSurface as buffer, show). The same service is what Mesa's
freedreno and hakios-metal both use. Consequence: the new kernel work is **one file that implements those functions for
A5xx** (`lavender.adreno.c`), and the service is carried over with small edits (names, the A6xx-specific `GET_PARAM`
values such as chip id and GMEM size).

## What changes for A5xx (the chip driver)

| hakios (A6xx, 642L) | Adreno 512 |
| --- | --- |
| GMU: firmware load, HFI queues, power requests (steps 4-17 of its ladder) | **absent.** The core is powered by the GX GDSC plus clock/rail votes; the GPMU is only for 530 |
| SQE microcode (`a660_sqe.fw`) | PM4 + PFP microcode (`a530_pm4.fw`, `a530_pfp.fw`), loaded into the CP's instruction memory by register writes |
| zap shader through SCM PAS, then "out of secure mode" | the same idea (pas-id 13, `a512_zap`), same SCM sequence |
| SMMU: Qualcomm hypervisor mediates | kgsl SMMU at `0x05040000`, no hypervisor in the way expected on SDM660 (to confirm) |
| ring/CP start (`step_core_start`) | A5xx `hw_init`: `CP_ME_INIT`, `CP_SET_PROTECTED_MODE`, `RBBM` setup; different register set |
| fence/timestamps | `CP_EVENT_WRITE` / `CP_MEM_WRITE` fence; A5xx layout |

The step-ladder method of hakios stays: every step behind a boot-arg (`lavgpu=N`), reads before writes, one line saying what
it is about to touch and what it found, and stop at the first step that does not find what it expects. A wrong access on a
Qualcomm SoC often ends the run silently, so the log ring must flush before each risky access.

## What changes for userland

`a6xx.hpp` (374 KB, checked against Mesa's `a6xx.xml.h`) is replaced by an `a5xx.hpp` generated from Mesa's
`a5xx.xml` and checked the same way (`tools/check-a5xx.py`). `compute.hpp` and `render.hpp` are rewritten against Gallium's
`fd5` sequences (`fd5_compute.c`, `fd5_emit.c`, `fd5_gmem.c`) because turnip never supported A5xx. The fake GPU
(an interpreter as strict as the command processor) is kept: most of the A5xx work is validated on the host.
The ir3 compiler path needs no chip port (Mesa carries `GPUId(512)`); only the compile target is selected.

## Licensing split

| Part | License | Origin |
| --- | --- | --- |
| Kernel service and chip driver | APSL 2.0 (derivative of XNU) | adapted from hakios `drivers/gpu/` |
| Userland Metal layer, a5xx tables | MIT | adapted from `hakios-metal/`, Mesa (MIT) |
| Firmware | not distributed | read from the user's own stock ROM |

GPL code (Linux msm, metal2vulkan) is used only as reading material, never copied.
