/*
 * lavender: Adreno 512 bring-up ladder (M1). Boot-arg gated, off by default.
 *
 *   lavgpu=N        run the ladder up to step N (0 or absent: nothing happens)
 *   lavgpudelay=S   seconds to wait after boot before step 1 (default 25), so
 *                   the log flusher and the USB live log are up
 *
 * Steps, each one announced BEFORE the access, with a 2 s pause so the log
 * reaches the eMMC ring / USB (a wrong access on this SoC usually resets it
 * without a word):
 *   1  reads only: GCC branch state, and (if the GPU config AHB clock is on)
 *      the GPUCC block: PLLs, GDSCs, branches
 *   2  vote gcc_gpu_bimc_gfx_clk and gcc_bimc_gfx_clk, check the halt bits
 *   3  switch the CX GDSC on if it is off
 *   4  switch the GX GDSC on, release the GMEM clamp, enable the GFX3D core
 *      and RBBM timer branches (RCG left as the bootloader set it), then READ
 *      the RBBM / CP registers at 0x05000000
 * Nothing in this file writes a register inside the GPU block (0x05000000);
 * steps 2-4 write only GCC branch votes, GDSC controls and gpucc branches. No rail (CPR/RPM) vote is made:
 * step 4 is exactly the question "is the bootloader's rail state enough?".
 *
 * Offsets: Linux gcc-sdm660.c / gpucc-sdm660.c and Mesa a5xx.xml, read as
 * behaviour reference (values only, no code copied).
 */
#if defined(ARM64_BOARD_CONFIG_LAVENDER)

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);

#define GCC_PA          0x00100000u
#define GCC_SIZE        0x80000u
#define GPUCC_PA        0x05065000u
#define GPUCC_SIZE      0x10000u
#define GPU_PA          0x05000000u
#define GPU_SIZE        0x40000u

/* GCC (byte offsets from GCC_PA) */
#define GCC_GPU_CFG_AHB         0x71004u
#define GCC_GPU_BIMC_GFX        0x71010u
#define GCC_BIMC_GFX            0x7106cu
#define GCC_GPLL0_VOTE          0x5200cu        /* bit 4 = gcc_gpu_gpll0_clk */

/* GPUCC (byte offsets from GPUCC_PA) */
#define CC_GPU_PLL0_MODE        0x0000u
#define CC_GPU_PLL1_MODE        0x0040u
#define CC_CX_GDSCR             0x1004u
#define CC_CX_GDS_HW_CTRL       0x1008u
#define CC_GX_GDSCR             0x1094u
#define CC_GX_CLAMP_IO          0x0130u
#define CC_GX_BCR               0x1090u
#define GX_GMEM_RESET           (1u << 4)
#define CC_GFX3D_CLK            0x1098u
#define CC_GFX3D_RCG_CMD        0x1070u
#define CC_GFX3D_RCG_CFG        0x1074u
#define CC_RBBMTIMER_CLK        0x10d0u
#define CC_RBBMTIMER_RCG_CMD    0x10b0u
#define CC_RBCPR_CLK            0x1054u
#define CC_RBCPR_RCG_CMD        0x1030u
#define CC_CX_GFX3D_CLK         0x1020u

/* GPU block (byte offsets; A5xx dword offsets from a5xx.xml times 4) */
#define A5_CP_RB_BASE           (0x0800u * 4)
#define A5_CP_RB_CNTL           (0x0802u * 4)
#define A5_RBBM_SW_RESET_CMD    (0x0043u * 4)
#define A5_RBBM_CLOCK_CNTL      (0x0067u * 4)
#define A5_RBBM_INT_0_STATUS    (0x04e1u * 4)
#define A5_RBBM_AHB_ERR_STATUS  (0x04f4u * 4)
#define A5_RBBM_STATUS          (0x04f5u * 4)
#define A5_RBBM_STATUS3         (0x0530u * 4)
#define A5_RBBM_SECVID_TRUST    (0xf400u * 4)

#define RCG_ROOT_EN     (1u << 1)
#define RCG_ROOT_OFF    (1u << 31)
/* msm_gfx_ldo block, inside the GPUCC window (0x0506e000 - 0x05065000) */
#define CC_LDO                  0x9000u
#define CC_LDO_PWRSWITCH_CTRL   (CC_LDO + 0x1cu)
#define CC_LDO_STATUS1          (CC_LDO + 0x24u)
#define CC_LDO_PWRSWITCH_STATUS (CC_LDO + 0x28u)
#define LDO_BHS_UNDER_SW        (1u << 0)
#define LDO_BHS_EN_FEW          (1u << 1)
#define LDO_BHS_EN_REST         (1u << 2)
#define LDO_UNDER_SW_CTRL       (1u << 4)
#define LDO_PWR_SRC_SEL         (1u << 9)
#define LDO_CPR_BYPASS          (1u << 30)

#define CBCR_OFF        (1u << 31)
#define CBCR_EN         (1u << 0)
#define GDSC_PWR_ON     (1u << 31)
#define GDSC_COLLAPSE   (1u << 0)

static vm_offset_t gcc, gpucc, gpu;
static int lav_chan;

static inline uint32_t
rd(vm_offset_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dsb ld" ::: "memory");
	return v;
}

static inline void
wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
	__asm__ volatile("dsb osh" ::: "memory");
}

static void
lav_wait(unsigned ms)
{
	uint64_t deadline;
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &deadline);
	(void)assert_wait_deadline(&lav_chan, THREAD_UNINT, deadline);
	thread_block(THREAD_CONTINUE_NULL);
}

/* Announce, then give the log 2 s to leave the machine. */
static void
lav_announce(const char *what)
{
	printf("LAVGPU: about to %s\n", what);
	lav_wait(2000);
}

/* Poll a bit until it equals want; returns the last value. */
static uint32_t
lav_poll_n(vm_offset_t base, uint32_t off, uint32_t mask, uint32_t want, int n)
{
	uint32_t v = 0;
	for (int i = 0; i < n; i++) {
		v = rd(base, off);
		if ((v & mask) == want) {
			break;
		}
		lav_wait(10);
	}
	return v;
}

static uint32_t
lav_poll(vm_offset_t base, uint32_t off, uint32_t mask, uint32_t want)
{
	return lav_poll_n(base, off, mask, want, 100);
}

static void dump_ldo(const char *when);

static void
step1_reads(void)
{
	uint32_t ahb;

	lav_announce("step 1: read GCC GPU branches (read only)");
	ahb = rd(gcc, GCC_GPU_CFG_AHB);
	printf("LAVGPU: GCC cfg_ahb %08x bimc_gfx %08x gcc_bimc_gfx %08x "
	    "gpll0_vote %08x\n", ahb, rd(gcc, GCC_GPU_BIMC_GFX),
	    rd(gcc, GCC_BIMC_GFX), rd(gcc, GCC_GPLL0_VOTE));
	if (ahb & CBCR_OFF) {
		printf("LAVGPU: gcc_gpu_cfg_ahb_clk is OFF, GPUCC not read "
		    "(it would be an unclocked access)\n");
		return;
	}
	lav_announce("step 1: read GPUCC (AHB clock is on)");
	printf("LAVGPU: pll0 mode %08x pll1 mode %08x\n",
	    rd(gpucc, CC_GPU_PLL0_MODE), rd(gpucc, CC_GPU_PLL1_MODE));
	printf("LAVGPU: CX gdscr %08x hwctrl %08x GX gdscr %08x clamp %08x\n",
	    rd(gpucc, CC_CX_GDSCR), rd(gpucc, CC_CX_GDS_HW_CTRL),
	    rd(gpucc, CC_GX_GDSCR), rd(gpucc, CC_GX_CLAMP_IO));
	printf("LAVGPU: gfx3d clk %08x rcg cmd %08x cfg %08x; rbbmtimer clk "
	    "%08x rcg %08x; rbcpr clk %08x rcg %08x; cx_gfx3d %08x\n",
	    rd(gpucc, CC_GFX3D_CLK), rd(gpucc, CC_GFX3D_RCG_CMD),
	    rd(gpucc, CC_GFX3D_RCG_CFG), rd(gpucc, CC_RBBMTIMER_CLK),
	    rd(gpucc, CC_RBBMTIMER_RCG_CMD), rd(gpucc, CC_RBCPR_CLK),
	    rd(gpucc, CC_RBCPR_RCG_CMD), rd(gpucc, CC_CX_GFX3D_CLK));
	dump_ldo("step 1");
}

static void
vote_branch(const char *name, uint32_t off)
{
	uint32_t v = rd(gcc, off);
	printf("LAVGPU: %s before %08x\n", name, v);
	wr(gcc, off, v | CBCR_EN);
	v = lav_poll(gcc, off, CBCR_OFF, 0);
	printf("LAVGPU: %s after %08x (%s)\n", name, v,
	    (v & CBCR_OFF) ? "STILL OFF" : "on");
}

static void
step2_votes(void)
{
	lav_announce("step 2: vote gcc_gpu_bimc_gfx_clk and gcc_bimc_gfx_clk");
	vote_branch("gpu_bimc_gfx", GCC_GPU_BIMC_GFX);
	vote_branch("bimc_gfx", GCC_BIMC_GFX);
}

static int
gdsc_on_n(const char *name, uint32_t off, int polls)
{
	uint32_t v = rd(gpucc, off);

	if (v & GDSC_PWR_ON) {
		printf("LAVGPU: %s GDSC already on (%08x)\n", name, v);
		return 0;
	}
	printf("LAVGPU: %s GDSC off (%08x), clearing SW_COLLAPSE\n", name, v);
	wr(gpucc, off, v & ~GDSC_COLLAPSE);
	v = lav_poll_n(gpucc, off, GDSC_PWR_ON, GDSC_PWR_ON, polls);
	printf("LAVGPU: %s GDSC now %08x (%s)\n", name, v,
	    (v & GDSC_PWR_ON) ? "on" : "STILL OFF");
	return (v & GDSC_PWR_ON) ? 0 : EIO;
}

static int
gdsc_on(const char *name, uint32_t off)
{
	return gdsc_on_n(name, off, 100);
}

static int
step3_cx(void)
{
	if (rd(gcc, GCC_GPU_CFG_AHB) & CBCR_OFF) {
		return EIO;
	}
	lav_announce("step 3: CX GDSC on");
	return gdsc_on("CX", CC_CX_GDSCR);
}

static void
dump_gpu(void)
{
	printf("LAVGPU: RBBM_STATUS %08x STATUS3 %08x INT_0 %08x AHB_ERR %08x\n",
	    rd(gpu, A5_RBBM_STATUS), rd(gpu, A5_RBBM_STATUS3),
	    rd(gpu, A5_RBBM_INT_0_STATUS), rd(gpu, A5_RBBM_AHB_ERR_STATUS));
	printf("LAVGPU: SECVID_TRUST_CNTL %08x CLOCK_CNTL %08x SW_RESET %08x "
	    "CP_RB_BASE %08x CP_RB_CNTL %08x\n", rd(gpu, A5_RBBM_SECVID_TRUST),
	    rd(gpu, A5_RBBM_CLOCK_CNTL), rd(gpu, A5_RBBM_SW_RESET_CMD),
	    rd(gpu, A5_CP_RB_BASE), rd(gpu, A5_CP_RB_CNTL));
}

static void
dump_ldo(const char *when)
{
	printf("LAVGPU: gfx ldo (%s): atest %08x cfg0 %08x cfg1 %08x cfg2 %08x "
	    "ld %08x vreftest %08x vref %08x\n", when, rd(gpucc, CC_LDO),
	    rd(gpucc, CC_LDO + 0x4), rd(gpucc, CC_LDO + 0x8),
	    rd(gpucc, CC_LDO + 0xc), rd(gpucc, CC_LDO + 0x10),
	    rd(gpucc, CC_LDO + 0x14), rd(gpucc, CC_LDO + 0x18));
	printf("LAVGPU: gfx ldo (%s): pwrswitch_ctrl %08x status1 %08x "
	    "pwrswitch_status %08x\n", when, rd(gpucc, CC_LDO_PWRSWITCH_CTRL),
	    rd(gpucc, CC_LDO_STATUS1), rd(gpucc, CC_LDO_PWRSWITCH_STATUS));
}

/*
 * The GX power domain is fed through the GFX LDO/head-switch block. Put it in
 * BHS mode under GDSC control, the same bit sequence the vendor regulator
 * runs on enable (msm_gfx_ldo.c: enable_bhs_mode, behaviour reference only).
 */
static void
ldo_bhs_mode(void)
{
	uint32_t ctl = rd(gpucc, CC_LDO_PWRSWITCH_CTRL);

	ctl |= LDO_UNDER_SW_CTRL;
	wr(gpucc, CC_LDO_PWRSWITCH_CTRL, ctl);
	ctl &= ~LDO_PWR_SRC_SEL;
	wr(gpucc, CC_LDO_PWRSWITCH_CTRL, ctl);
	ctl &= ~LDO_CPR_BYPASS;
	wr(gpucc, CC_LDO_PWRSWITCH_CTRL, ctl);
	ctl &= ~LDO_BHS_EN_FEW;
	wr(gpucc, CC_LDO_PWRSWITCH_CTRL, ctl);
	ctl &= ~LDO_BHS_EN_REST;
	wr(gpucc, CC_LDO_PWRSWITCH_CTRL, ctl);
	ctl &= ~LDO_BHS_UNDER_SW;
	wr(gpucc, CC_LDO_PWRSWITCH_CTRL, ctl);
}

static void
step4_gx(void)
{
	uint32_t v, ldo_ctl_saved;

	/*
	 * Run 3 (0236): the GX GDSC left the off state (GDSCR 08222000) but never
	 * reported PWR_ON. The DT marks it qcom,force-enable-root-clk with
	 * core_root_clk = the GFX3D RCG, which was still ROOT_OFF. Turn the root on
	 * first; its source is XO (cfg 0, 19.2 MHz), no PLL and no rail involved.
	 */
	lav_announce("step 4: force the GFX3D RCG root on (XO source)");
	v = rd(gpucc, CC_GFX3D_RCG_CMD);
	printf("LAVGPU: gfx3d rcg cmd before %08x cfg %08x\n", v,
	    rd(gpucc, CC_GFX3D_RCG_CFG));
	wr(gpucc, CC_GFX3D_RCG_CMD, v | RCG_ROOT_EN);
	v = lav_poll(gpucc, CC_GFX3D_RCG_CMD, RCG_ROOT_OFF, 0);
	printf("LAVGPU: gfx3d rcg cmd after %08x (%s)\n", v,
	    (v & RCG_ROOT_OFF) ? "root STILL OFF" : "root on");
	dump_ldo("before");
	lav_announce("step 4: put the GFX LDO block in BHS mode under GDSC control");
	ldo_ctl_saved = rd(gpucc, CC_LDO_PWRSWITCH_CTRL);
	ldo_bhs_mode();
	dump_ldo("after bhs");
	/*
	 * Run 5 (0238): the LDO bits were already in BHS state and the GX GDSC
	 * still stayed at 08222000. The vendor GDSC driver does three more things
	 * BEFORE it clears SW_COLLAPSE (gdsc-regulator.c, behaviour reference):
	 * pulse the GX block reset (sw-reset = GPUCC+0x1090 bit 0), pulse the
	 * AON/GMEM reset (domain-addr = GPUCC+0x130 bit 4, reset-aon-logic) and
	 * release the GMEM clamp (same register, bit 0). Until now the clamp was
	 * only released after the power-up that never completed.
	 */
	lav_announce("step 4: GX block reset pulse, AON reset pulse, release clamp");
	v = rd(gpucc, CC_GX_BCR);
	printf("LAVGPU: GX bcr %08x clamp/domain %08x\n", v,
	    rd(gpucc, CC_GX_CLAMP_IO));
	wr(gpucc, CC_GX_BCR, v | 1u);
	for (int i = 0; i < 20; i++) {
		(void)rd(gpucc, CC_GX_GDSCR);
	}
	wr(gpucc, CC_GX_BCR, v & ~1u);
	v = rd(gpucc, CC_GX_CLAMP_IO);
	wr(gpucc, CC_GX_CLAMP_IO, v | GX_GMEM_RESET);
	for (int i = 0; i < 20; i++) {
		(void)rd(gpucc, CC_GX_GDSCR);
	}
	wr(gpucc, CC_GX_CLAMP_IO, v & ~GX_GMEM_RESET);
	wr(gpucc, CC_GX_CLAMP_IO, rd(gpucc, CC_GX_CLAMP_IO) & ~1u);
	printf("LAVGPU: GX bcr %08x clamp/domain %08x\n", rd(gpucc, CC_GX_BCR),
	    rd(gpucc, CC_GX_CLAMP_IO));
	lav_announce("step 4: GX GDSC on");
	if (gdsc_on_n("GX", CC_GX_GDSCR, 300)) {
		lav_wait(1000);
		printf("LAVGPU: GX after 1 more s: gdscr %08x cfg(0x1098) %08x\n",
		    rd(gpucc, CC_GX_GDSCR), rd(gpucc, CC_GFX3D_CLK));
		/* leave the domain as found: collapse it, drop the forced root,
		 * restore the LDO block */
		wr(gpucc, CC_LDO_PWRSWITCH_CTRL, ldo_ctl_saved);
		wr(gpucc, CC_GX_CLAMP_IO, rd(gpucc, CC_GX_CLAMP_IO) | 1u);
		wr(gpucc, CC_GX_GDSCR, rd(gpucc, CC_GX_GDSCR) | GDSC_COLLAPSE);
		wr(gpucc, CC_GFX3D_RCG_CMD,
		    rd(gpucc, CC_GFX3D_RCG_CMD) & ~RCG_ROOT_EN);
		lav_wait(100);
		printf("LAVGPU: GX collapsed again: gdscr %08x rcg %08x\n",
		    rd(gpucc, CC_GX_GDSCR), rd(gpucc, CC_GFX3D_RCG_CMD));
		return;
	}
	v = rd(gpucc, CC_GX_CLAMP_IO);
	printf("LAVGPU: GMEM clamp before %08x, clearing bit 0\n", v);
	wr(gpucc, CC_GX_CLAMP_IO, v & ~1u);
	lav_announce("step 4: enable gfx3d and rbbmtimer branches");
	for (int i = 0; i < 2; i++) {
		uint32_t off = i ? CC_RBBMTIMER_CLK : CC_GFX3D_CLK;
		v = rd(gpucc, off);
		wr(gpucc, off, v | CBCR_EN);
		v = lav_poll(gpucc, off, CBCR_OFF, 0);
		printf("LAVGPU: %s clk %08x (%s)\n", i ? "rbbmtimer" : "gfx3d",
		    v, (v & CBCR_OFF) ? "STILL OFF" : "on");
	}
	lav_announce("step 4: READ the GPU block at 0x05000000");
	dump_gpu();
	printf("LAVGPU: step 4 done, the GPU block answered\n");
}

/*
 * Step 5: does the SDM660 TrustZone take our SCM call, and does it know the
 * GPU zap shader (PAS id 13)? Questions only, no state change. The calling
 * convention is the one 0113 uses for the modem (SMC64, owner SIP, yielding
 * call, resumed while x0 == 1); Linux qcom_scm-smc.c as behaviour reference.
 */
#define SCM_FN(svc, cmd)        (0x42000000u | ((uint32_t)(svc) << 8) | (uint32_t)(cmd))
#define SCM_SVC_INFO            0x06
#define SCM_INFO_IS_CALL_AVAIL  0x01
#define SCM_SVC_PIL             0x02
#define PIL_IS_SUPPORTED        0x07

static uint64_t
lav_scm(uint32_t fn, uint64_t arginfo, uint64_t a, uint64_t *res1)
{
	register uint64_t x0 __asm__("x0") = fn, x1 __asm__("x1") = arginfo,
	    x2 __asm__("x2") = a, x3 __asm__("x3") = 0, x4 __asm__("x4") = 0,
	    x5 __asm__("x5") = 0, x6 __asm__("x6") = 0, x7 __asm__("x7") = 0;

	for (int n = 0; n < 1000; n++) {
		__asm__ volatile ("smc #0"
		    : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4),
		      "+r"(x5), "+r"(x6), "+r"(x7)
		    :
		    : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15",
		      "x16", "x17", "memory");
		if (x0 != 1) {
			break;
		}
		/* INTERRUPTED: resume with the session id the secure side gave */
		x0 = 1;
		x1 = arginfo;
		x2 = a;
		x3 = 0;
		x4 = 0;
		x5 = 0;
		x7 = 0;
	}
	*res1 = x1;
	return x0;
}

static void
step5_scm(void)
{
	static const uint32_t ids[] = { 13, 4, 99 };
	static const char *const names[] = { "gpu zap", "modem (reference)",
	    "unused (contrast)" };
	uint64_t ret, res = 0;

	lav_announce("step 5: SCM INFO is_call_available(PIL IS_SUPPORTED)");
	ret = lav_scm(SCM_FN(SCM_SVC_INFO, SCM_INFO_IS_CALL_AVAIL), 1,
	    SCM_FN(SCM_SVC_PIL, PIL_IS_SUPPORTED), &res);
	printf("LAVGPU: SCM is_call_available: ret %llx res %llx\n",
	    (unsigned long long)ret, (unsigned long long)res);
	for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
		lav_announce("step 5: SCM PIL IS_SUPPORTED for the next id");
		ret = lav_scm(SCM_FN(SCM_SVC_PIL, PIL_IS_SUPPORTED), 1, ids[i],
		    &res);
		printf("LAVGPU: SCM PAS id %u (%s) supported: ret %llx res %llx\n",
		    ids[i], names[i], (unsigned long long)ret,
		    (unsigned long long)res);
	}
}

static void
lavender_gpu_thread(__unused void *arg, __unused wait_result_t wr_)
{
	uint32_t step = 0, delay_s = 25;

	(void)PE_parse_boot_argn("lavgpu", &step, sizeof(step));
	(void)PE_parse_boot_argn("lavgpudelay", &delay_s, sizeof(delay_s));
	if (delay_s > 300) {
		delay_s = 300;
	}
	printf("LAVGPU: ladder up to step %u in %u s\n", step, delay_s);
	lav_wait(delay_s * 1000);

	gcc = ml_io_map(GCC_PA, GCC_SIZE);
	gpucc = ml_io_map(GPUCC_PA, GPUCC_SIZE);
	if (gcc == 0 || gpucc == 0) {
		printf("LAVGPU: map failed gcc %p gpucc %p\n", (void *)gcc,
		    (void *)gpucc);
		return;
	}
	step1_reads();
	if (step >= 2) {
		step2_votes();
	}
	if (step >= 3 && step3_cx() != 0) {
		printf("LAVGPU: stopped before step 4 (CX or AHB not ready)\n");
		return;
	}
	if (step >= 4) {
		gpu = ml_io_map(GPU_PA, GPU_SIZE);
		if (gpu == 0) {
			printf("LAVGPU: GPU block map failed\n");
			return;
		}
		step4_gx();
	}
	if (step >= 5) {
		step5_scm();
	}
	printf("LAVGPU: ladder finished\n");
}

int
lavender_gpu_probe(void)
{
	static int started;
	uint32_t step = 0;
	thread_t thread;

	if (started || !PE_parse_boot_argn("lavgpu", &step, sizeof(step)) ||
	    step == 0) {
		return 0;
	}
	started = 1;
	if (kernel_thread_start(lavender_gpu_thread, NULL, &thread) ==
	    KERN_SUCCESS) {
		thread_deallocate(thread);
	}
	return 0;
}

#else /* !ARM64_BOARD_CONFIG_LAVENDER */

int lavender_gpu_probe(void);
int
lavender_gpu_probe(void)
{
	return 0;
}

#endif
