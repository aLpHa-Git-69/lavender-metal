# M1 log

## Run 1 - lavgpu=1 (read only), 2026-11-21, live log live-0232-dmadiag.txt

    GCC cfg_ahb 20008001 bimc_gfx 00000001 gcc_bimc_gfx 80000000 gpll0_vote 00000000
    pll0 mode 00000000 pll1 mode 00000000
    CX gdscr 00222001 hwctrl 00005ff4  GX gdscr 00222001  clamp 00000001
    gfx3d clk 80008620 rcg cmd 80000000 cfg 0; rbbmtimer clk 80000000 rcg 80000000; rbcpr clk 80000000 rcg 80000000

Reading: the GPU is fully off. gcc_gpu_cfg_ahb_clk is on, so GPUCC is readable and the reads did not hang the SoC
(offsets of GCC 0x71004 and GPUCC GDSC/branch registers are the right ones: they decode as CBCR/GDSCR). gpu_bimc_gfx
already on; gcc_bimc_gfx off; both GDSCs collapsed (PWR_ON 0, SW_COLLAPSE 1); PLLs off; gfx3d/rbbmtimer branches off.
Boot unaffected. Next: lavgpu=3 (BIMC votes + CX GDSC).

## Run 2 - lavgpu=3 (BIMC votes + CX GDSC), 2026-11-21

    gpu_bimc_gfx 00000001 -> 00000001 (already on)
    gcc_bimc_gfx 80000000 -> 00000001 (on)
    CX GDSC 00222001 -> a0222000 (PWR_ON, SW_COLLAPSE cleared) in 11 ms
    ladder finished; boot continued normally

Reading: GCC votes and the CX power domain work from XNU with plain register writes; no hang, no reset.
Next: lavgpu=4 (GX GDSC, clamp release, gfx3d/rbbmtimer branches, first read of the GPU block at 0x05000000).
This is the first step that depends on the GX rail the bootloader left.

## Run 3 - lavgpu=4 (first GX attempt), 2026-11-21

    CX GDSC on again (a0222000).
    GX GDSC off (00222001), clearing SW_COLLAPSE
    GX GDSC now 08222000 (STILL OFF) after 1 s; ladder stopped; boot continued normally

Reading: GDSCR bits 27:24 left 0 (state 8) but PWR_ON (bit 31) never came. DT `gdsc_gpu_gx`: `qcom,force-enable-root-clk`,
`core_root_clk = <gpucc 0xc>` (the GFX3D RCG, still ROOT_OFF: rcg cmd 80000000), `qcom,reset-aon-logic`,
`parent-supply` = `gfx_corner` (CPR4 regulator, `/soc/cpr4-ctrl@05061000`). Plausible causes, in order:
1. root clock not running (fix in 0237: set ROOT_EN on the RCG, XO source, before toggling the GDSC);
2. the GX rail (`gfx_corner` / CPR4 + PMIC) is off: needs an RPM/CPR vote the port cannot make yet.
0237 also collapses GX again on failure and prints GDSCR and 0x1098 after 1 more s.
