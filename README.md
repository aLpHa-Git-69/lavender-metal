# lavender-metal

GPU bring-up for the **Redmi Note 7 (lavender, Snapdragon 660, Adreno 512)** running iOS 16 through the
[lavender-ios16](https://github.com/pcorezzola/lavender-ios16) port: an Adreno 512 kernel driver for XNU and a
Metal-compatible layer on top of it, so that iOS stops drawing everything on the CPU.

## Why

The port boots to the iOS 16.7.16 lock screen, but there is no GPU: `MTLCreateSystemDefaultDevice()` returns nil and
QuartzCore falls back to its software OpenGL renderer (`CA::OGL::SW`). On the device `backboardd` uses ~150-170 % of
the four A73 cores with the lock screen idle, frames come out at ~13 fps on average (30 Hz panel), and wallpapers that
need Metal (Astronomy, ...) fail. See `lavender-ios16/README.md`, section *Performance*.

## Starting point

[`oosexgang/hakios-xnu-kernel`](https://github.com/oosexgang/hakios-xnu-kernel) (`hakios-metal/`, MIT, and the kernel
side in `drivers/gpu/`, APSL) is a working Metal driver for **Adreno A6xx** (Adreno 642L, GMU): the Metal command model,
compute, textures, render passes, an AIR -> NIR -> ir3 shader compiler built on Mesa, and the `MTLDevice` plug-in. It is
the architecture we follow, not code that runs on this GPU:

| | hakios (A6xx) | this device (A5xx) |
| --- | --- | --- |
| Power-up | GMU | no GMU: GPMU, CP/PFP microcode, zap shader through TrustZone |
| Registers / packets | `a6xx.hpp`, `CP_LOAD_STATE6` | `a5xx.xml`, `CP_LOAD_STATE4` |
| Reference for sequences | turnip (Vulkan) | none (turnip never supported A5xx): Gallium `fd5` |
| SoC GPU rails | SC7280 | SDM660: `gpucc-sdm660`, CPR `gfx` corner, `gfx_ldo` |

The reference tree is kept (sparse, not committed) under `refs/hakios` at the commit recorded in `docs/REFERENCES.md`.

## Plan

See [`docs/PLAN.md`](docs/PLAN.md). Short version:

1. **M0** Reconnaissance: firmware from the stock ROM, GPU node of the DTB, what Mesa knows about the chip.
2. **M1** Power the GPU from XNU and read its version.
3. **M2** Ring buffer, microcode, zap shader, a CP-written fence.
4. **M3** A5xx register layer + compute, checked against a fake GPU.
5. **M4** Render, `MTLDevice`, CoreAnimation over Metal.

## Layout

```
docs/       plan, hardware notes, references, measurements
refs/       reference trees (not committed; see docs/REFERENCES.md)
firmware/   GPU firmware taken from the phone's own ROM (not committed)
tools/      scripts (firmware extraction, DTB reading, probes)
notes/      working notes per milestone
```

## Legal

Apple binaries and the phone's firmware are not distributed. Firmware is read from the user's own stock ROM. Kernel
code derived from XNU stays under the APSL; user-space code is MIT. See `docs/REFERENCES.md` for upstream licenses.
