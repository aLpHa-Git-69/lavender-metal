# References

| What | Where | License | Used for |
| --- | --- | --- | --- |
| hakios-xnu-kernel | https://github.com/oosexgang/hakios-xnu-kernel (commit `63fc2b4`, 2026-11-06) | `hakios-metal/` MIT; `drivers/gpu/`, `xnu/files/` APSL 2.0 | Architecture: Metal driver on Adreno A6xx, IOKit GPU service, AIR -> NIR -> ir3 front end, fake GPU, probe |
| Mesa freedreno | https://gitlab.freedesktop.org/mesa/mesa (`src/freedreno/`) | MIT | `a5xx.xml`, `adreno_pm4.xml`, ir3 compiler, Gallium `fd5` (A5xx sequences), `freedreno_devices.py` |
| Linux msm DRM / a5xx | linux `drivers/gpu/drm/msm/adreno/a5xx_*.c` | GPL-2.0 | Behaviour reference only (bring-up order, firmware names); no code copied |
| lavender DTB | `lavender-ios16/artifacts/device-audit/lavender_dtb_from_twrp.bin` | n/a (device data) | GPU node, clocks, rails, SMMU |
| lavender-ios16 | https://github.com/pcorezzola/lavender-ios16 | see repo | The port this driver plugs into |

## hakios reference tree

```sh
git clone --depth 1 --filter=blob:none --sparse https://github.com/oosexgang/hakios-xnu-kernel.git refs/hakios
cd refs/hakios
git sparse-checkout set --no-cone hakios-metal drivers/gpu docs/reference/metal.md \
    docs/driver-review/gpu-display.md /LICENSE /LICENSE.APSL /NOTICE /THIRD_PARTY_LICENSES.md xnu/files/iokit/Kernel
```

Files from it that are copied or adapted into this repository must keep their license header and be listed here.

## Reviewed and not adopted

| What | Where | License | Verdict |
| --- | --- | --- | --- |
| ASoftware iOS for Android | https://github.com/alpersamur3/asoft-ios-android | MIT | Runs iOS in QEMU inside an Android app; iOS 16 there also draws on the CPU. Its GPU path is for iOS 26 in Apple's research VM, over Vulkan on Mesa Turnip (Adreno 6xx and newer only). Not usable on a bare-metal A5xx. Only the fps reports in its `DEVICES.md` are useful as a yardstick (iOS 16 on the CPU: 3-51 fps depending on the phone). |
| metal2vulkan (upstream steelbrain/metal2vulkan; fork alpersamur3/metal2vulkan) | https://github.com/steelbrain/metal2vulkan | GPL-2.0-or-later OR LGPL-3.0-or-later | Rust: AIR/metallib -> SPIR-V plus "the Metal API on Vulkan" (`metal2vulkan-runtime`, needs a Vulkan 1.2 device). Adreno 512 has no Vulkan 1.2 driver here (Turnip has no A5xx), so it cannot be the backend. Worth reading as a second AIR front end (`metal2vulkan-air`) next to hakios's; copyleft, so nothing is copied into kernel or MIT code. |
| reims-vgpu | https://github.com/alpersamur3/reims-vgpu | NOASSERTION | Virtual GPU for VM guests; irrelevant to real hardware. |
