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

## Bisect of the stalled boot (userdata of 2026-11-21 08:3x, widgets work by another agent)

- lavgpu=4 (0236): ladder ended at 53 s, SpringBoard blocked afterwards (CPU time frozen at 12.28 s for >5 min, compositor stopped at swap 3,
  no PosterBoard). Photo: Apple logo with console text.
- Control, shim 0235 (no GPU code), same userdata: swaps keep going, PosterBoard at 129.6 s. So the stall comes from the GPU ladder, not from userdata.
- lavgpu=1 (read only): fine (earlier). lavgpu=2 (BIMC votes, kernel 0237): fine, PosterBoard at 134 s.
- lavgpu=3 (earlier, 0236): log stops at 96 s (probably reset by hand). To retest with kernel 0237.

- lavgpu=3 (kernel 0237): fine, PosterBoard at 134.7 s. So BIMC votes and the CX GDSC are harmless; the stall of run 3 came from step 4
  (GX GDSC left with SW_COLLAPSE cleared and the power-up never completing, GDSCR 08222000). 0237 forces the RCG root first and collapses GX again on failure.

## Run 4 - lavgpu=4 with 0237 (root clock first), 2026-11-21

    gfx3d rcg cmd 80000000 -> 00000002 (root on)
    GX GDSC off (00222001), clearing SW_COLLAPSE
    GX GDSC now 08222000 (STILL OFF) after 3 s; after 1 more s the same, cfg(0x1098) 80008620 (POWER_UP_COMPLETE bit 16 = 0)
    GX collapsed again: gdscr 08222001 rcg 80000000; boot reached PosterBoard at 134.8 s (no stall: the clean collapse fixed it)

So the root clock was not the missing piece. DT: the GX GDSC parent supply is `gfx_corner` (CPR4 `qcom,cpr4-sdm660-mmss-ldo-regulator`), whose
`vdd-supply` is a *stub* regulator and whose `vdd-thread0-ldo-supply` is `msm_gfx_ldo` (`qcom,sdm660-gfx-ldo`, `0x0506e000`, 0x34 bytes). On this
SoC the GX rail goes through that LDO / head-switch block, which the GDSC drives via `PWRSWITCH_CTRL (+0x1c)`. Next: read it (0238), then BHS mode.

## Run 5 - lavgpu=4 with 0238 (LDO dump + BHS), 2026-11-21

    gfx ldo: atest/cfg0/cfg1/cfg2/ld/vreftest/vref all 0; pwrswitch_ctrl 80000038 (CLAMP_IO, PDN, UNDER_SW_CTRL set; PWR_SRC_SEL 0 = BHS);
             status1 0000000d; pwrswitch_status 0
    after the BHS sequence: identical (the bits were already in BHS state)
    GX GDSC now 08222000 (STILL OFF); collapsed again; boot reached the lock screen path normally

The BHS write sequence is a no-op here. Remaining difference with the vendor gdsc driver (gdsc-regulator.c, gdsc_enable): before clearing
SW_COLLAPSE it pulses the GX BCR (GPUCC+0x1090 bit 0), pulses GMEM_RESET (GPUCC+0x130 bit 4) and clears the clamp (GPUCC+0x130 bit 0).
We cleared the clamp only after a power-up that never happened (the clamp was still 1). 0239 does the three steps first.

## Run 6 - lavgpu=4 with 0239: THE GPU BLOCK ANSWERED, 2026-11-21 (M1 done)

    LAVGPU: GX bcr 00000000 clamp/domain 00000001   (before)  ->  GX bcr 00000000 clamp/domain 00000000
    LAVGPU: GX GDSC off (00222001), clearing SW_COLLAPSE
    LAVGPU: GX GDSC now a0222000 (on)               <- 11 ms
    LAVGPU: GMEM clamp before 003c0000, clearing bit 0
    LAVGPU: gfx3d clk 20008621 (on)  rbbmtimer clk 00000001 (on)
    LAVGPU: RBBM_STATUS 00000001 STATUS3 00000000 INT_0 00000000 AHB_ERR 00000000
    LAVGPU: SECVID_TRUST_CNTL 00000001 CLOCK_CNTL 00000000 SW_RESET 00000000 CP_RB_BASE 00000000 CP_RB_CNTL 00000000
    LAVGPU: step 4 done, the GPU block answered

What made the difference: the vendor order before SW_COLLAPSE: release the GMEM clamp (GPUCC+0x130 bit 0) (and the BCR / GMEM_RESET pulses). The LDO/BHS
sequence of 0238 was a no-op here and the forced root clock of 0237 was not what blocked it.
Reading: no hang, no AHB error, the block decodes registers (bus access fine, rails sufficient at the bootloader's corner and XO clock).
SECVID_TRUST_CNTL = 1: the secure-mode switch the zap shader must clear. Nothing else is configured yet (ring base 0, clock cntl 0).
