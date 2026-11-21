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
