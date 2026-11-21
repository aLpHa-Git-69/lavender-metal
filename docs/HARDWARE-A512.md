# Adreno 512 on the Redmi Note 7 (SDM660): what the hardware needs

Source: the phone's own DTB (`lavender-ios16/artifacts/device-audit/lavender_dtb_from_twrp.bin`, read with
`tools/dtb_dump.py`) and the stock ROM (MIUI V12.5.1.0.QFGEUXM). Nothing here is measured on the running iOS port yet:
it is what the Android kernel says the GPU is made of. Linux/Mesa knowledge is behavior reference only.

## The GPU node (`/soc/qcom,kgsl-3d0@5000000`)

| Item | Value |
| --- | --- |
| compatible | `qcom,kgsl-3d0`, `qcom,kgsl-3d` |
| Chip id | `0x05010200` (Adreno 5xx, **512**, rev 1.2) |
| Registers | `0x05000000`, size `0x40000` (`kgsl_3d0_reg_memory`) |
| Speed bin fuses | `qfprom` at `0x00780000`, size `0x6220` (`qcom,gpu-speed-bin = <0x41a0 0x1fe00000 0x15>`) |
| Interrupt | SPI 300 (`interrupts = <0 0x12c 0>`, GIC intid 332), `kgsl_3d0_irq` |
| Initial power level | 6 (266 MHz); idle timeout 80 ms; highest bank bit 14 |
| Quirks | `gpu-quirk-dp2clockgating-disable`, `gpu-quirk-lmloadkill-disable` |
| Memory pools | 4 KiB and 64 KiB pages |

### Frequencies (`qcom,gpu-pwrlevels-*`, bin 0 and others)

750, 700, 647, 588, 465, 370, 266, 160 and 19.2 MHz (level 0 .. 8). The speed bin (fuse) selects which table applies;
bin 0 reaches 750 MHz. Bring-up should start at 266 MHz (level 6) as Android does.

### Clocks (`clock-names` -> provider)

| Name | Provider |
| --- | --- |
| `core_clk` | `gpucc-sdm660` (`/soc/gfx@5065000`), index 0xd (the GFX3D RCG) |
| `iface_clk` | GCC (`/soc/clock-controller@100000`), index 0x42 |
| `rbbmtimer_clk` | `gpucc-sdm660`, index 0xe |
| `mem_clk` | GCC, index 0x40 |
| `alt_mem_iface_clk` | GCC, index 0x20 |
| `rbcpr_clk` | `gpu-sdm660` clock controller (`/soc/clock-controller@5065000`), index 1 |

The GPU clock controller sits at `0x05065000` (size `0x10000`). Two DT nodes share that range: `qcom,gpucc-sdm660` (the
GFX3D clock with the frequency-to-CPR-corner table) and `qcom,gpu-sdm660` (the helper clocks).

### Power rails

- `vddcx-supply` -> GDSC `gdsc_gpu_cx` at `0x05066004` (HW control, `no-status-check-on-disable`, timeout 2000 us).
- `vdd-supply` -> GDSC `gdsc_gpu_gx` at `0x05066094` (`domain-addr`, `sw-reset`, `retain-periph`).
- The GFX corner supplies (`vdd_gfx`, `vdd_dig_gfx`, `vdd_mx_gfx`) are RPM/CPR regulators. `qcom,gfxfreq-corner` maps
  frequency to corner: 19.2 -> 0, 160 -> 1, 266 -> 2, 370 -> 3, 465 -> 4, 588 -> 5, 647 -> 6, 700/750 -> 7.
  Raising the clock without raising the corner first is the classic way to hang the SoC.
- Also `qcom,gpu-cx-ipeak` (a shared CX current limiter, 700 MHz) and the `grp3d` bus votes (`gpubw`, 14 cases).

### IOMMU (`/soc/qcom,kgsl-iommu`, `qcom,kgsl-smmu-v2`)

- Register block `0x05040000`, size `0x10000`; `qcom,micro-mmu-control`; `secure_align_mask 0xfff`; retention.
- Context banks: `gfx3d_user` (SID 0, `gpu-offset 0x48000`) and `gfx3d_secure` (SID 2). The kgsl SMMU is the GPU's own,
  separate from the system SMMU (`iommus = <0x16e ...>`).
- Needs the same three bus clocks as the core (`iface_clk`, `mem_clk`, `alt_mem_iface_clk`).

### Zap shader (secure world)

`/soc/qcom,kgsl-hyp`: `qcom,pil-tz-generic`, **`qcom,pas-id = 13`**, `qcom,firmware-name = "a512_zap"`. The zap shader
is loaded through TrustZone (SCM PAS calls), which switches the GPU out of secure mode so the CP can run. This is the
biggest open question: it needs an SCM channel from XNU, and it is not yet known whether the port can already make SCM
calls from the kernel (it can reach the SoC through PSCI at boot, but PAS init/auth is a different service).

## Firmware (extracted from the stock ROM `vendor.img`, `/firmware`)

`tools/extract-gpu-fw.sh` puts these in `firmware/` (not committed, SHA256SUMS kept):

| File | Size | Role |
| --- | --- | --- |
| `a530_pm4.fw` | 19652 | CP micro-engine (PM4) microcode; Linux loads it for a512 too |
| `a530_pfp.fw` | 17108 | prefetch parser microcode |
| `a530v3_gpmu.fw2` | 8184 | GPMU (power management unit) firmware, v3 |
| `a530_gpmu.fw2` | 8052 | GPMU, earlier version |
| `a530v3_seq.fw2`, `a530v2_seq.fw2` | 2604 | GPMU sequencer data |
| `a512_zap.mdt` + `.b00/.b01/.b02` (and `.elf`) | 6684 + 148 + 6536 + 4916 (17204) | secure-world zap shader (split PIL form) |

Which of the `a530*` files a512 really uses (pm4/pfp: the a530 pair; GPMU: the v3 one) must be confirmed against the
Android kernel's table for chip `0x05010200` before M2. The `a508_zap.*` and `a540_gpmu.fw2` files also on the image are
for other chips and are ignored.

## What the port already has / lacks (to verify in M0)

Has (from lavender-ios16): eMMC/ADMA2, USB, touch, GIC, timers, SPMI keys, framebuffer through the MDP (display
subsystem at `0x0c900000`), an empty `IOAcceleratorES`.
To find out: whether the port enables clocks/regulators generically (GCC/RPM), whether it owns any SMMU, and whether
SCM (SMC calls) from XNU works at all. The first M1 patch should be read-only: map `0x05000000` and read registers
(`RBBM_*` chip/hw id) with the clocks left as the bootloader (ABL) leaves them, to learn whether the block is even
powered.
