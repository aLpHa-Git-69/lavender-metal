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
