/*
 * a5cs_gen: writes the PM4 stream of ONE A5xx compute dispatch, in the order Mesa's gallium freedreno driver gives it
 * (src/gallium/drivers/freedreno/a5xx/fd5_compute.c, fd5_emit.c, ir3/ir3_const.h; MIT, Copyright Rob Clark and the Mesa
 * contributors), with the register offsets and field encoders taken from the headers Mesa generates from its register
 * XML, so no register number is typed by hand. The kernel is the ir3 output for `fill_id` (hakios-metal's fixture, MIT)
 * compiled for gpu 512: out[i] = i * 0x9e3779b1 + seed, i over the whole grid.
 *
 * Output: a C header with the dwords and the places a GPU address goes (the buffer by index, a byte offset).
 *   gcc -I<mesa-build>/src/freedreno/registers/adreno a5cs_gen.c -o a5cs_gen && ./a5cs_gen > ../../userland/a5cs_fill.h
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "adreno_common.xml.h"
#include "adreno_pm4.xml.h"
#include "a3xx.xml.h"
#include "a5xx.xml.h"

#define BUF_CODE 0
#define BUF_OUT  1
#define LOCAL 64
#define GROUPS 4
#define SEED 0x1234u

static uint32_t w[512];
static int n;
static struct { int at, buf; uint32_t off; } rel[16];
static int nrel;

static unsigned par(unsigned v) { return (__builtin_popcount(v) & 1) ^ 1; }
static void out(uint32_t v) { w[n++] = v; }
static void reloc(int buf, uint32_t off) { rel[nrel].at = n; rel[nrel].buf = buf; rel[nrel].off = off; nrel++; out(0); out(0); }
static void pkt4(unsigned reg, unsigned cnt) { out(0x40000000 | cnt | par(cnt) << 7 | (reg & 0x3ffff) << 8 | par(reg) << 27); }
static void pkt7(unsigned op, unsigned cnt) { out(0x70000000 | cnt | par(cnt) << 15 | (op & 0x7f) << 16 | par(op) << 23); }
#define regid(num, comp) (((num) << 2) | (comp))

/* the fixture kernel (fill_id, gpu 512) */
static const uint32_t code[64] = {
#include "fill_id_a5_words.inc"
};
#define INSTRLEN 2
#define CONSTLEN 12
#define MAXREG 2
#define IMM_VEC4 8
#define UBO_VEC4 4

static void load_consts(unsigned vec4, const uint32_t *d, unsigned dwords)
{
	pkt7(CP_LOAD_STATE4, 3 + dwords);
	out(CP_LOAD_STATE4_0_DST_OFF(vec4) | CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
	    CP_LOAD_STATE4_0_STATE_BLOCK(SB4_CS_SHADER) | CP_LOAD_STATE4_0_NUM_UNIT(dwords / 4));
	out(CP_LOAD_STATE4_1_EXT_SRC_ADDR(0) | CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS));
	out(CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));
	for (unsigned i = 0; i < dwords; i++) {
		out(d[i]);
	}
}

int main(void)
{
	/* cs_program_emit */
	pkt4(REG_A5XX_SP_SP_CNTL, 1); out(0);
	pkt4(REG_A5XX_HLSQ_CONTROL_0_REG, 1);
	out(A5XX_HLSQ_CONTROL_0_REG_FSTHREADSIZE(TWO_QUADS) | A5XX_HLSQ_CONTROL_0_REG_CSTHREADSIZE(TWO_QUADS) | 0x880);
	pkt4(REG_A5XX_SP_CS_CTRL_REG0, 1);
	out(A5XX_SP_CS_CTRL_REG0_THREADSIZE(TWO_QUADS) | A5XX_SP_CS_CTRL_REG0_HALFREGFOOTPRINT(0) |
	    A5XX_SP_CS_CTRL_REG0_FULLREGFOOTPRINT(MAXREG + 1) | A5XX_SP_CS_CTRL_REG0_BRANCHSTACK(0) |
	    A5XX_SP_CS_CTRL_REG0_BUFFER | 0x2);
	pkt4(REG_A5XX_HLSQ_CS_CONFIG, 1);
	out(A5XX_HLSQ_CS_CONFIG_CONSTOBJECTOFFSET(0) | A5XX_HLSQ_CS_CONFIG_SHADEROBJOFFSET(0) | A5XX_HLSQ_CS_CONFIG_ENABLED);
	pkt4(REG_A5XX_HLSQ_CS_CNTL, 1);
	out(A5XX_HLSQ_CS_CNTL_INSTRLEN(INSTRLEN) | A5XX_HLSQ_CS_CNTL_SSBO_ENABLE);
	pkt4(REG_A5XX_SP_CS_CONFIG, 1);
	out(A5XX_SP_CS_CONFIG_CONSTOBJECTOFFSET(0) | A5XX_SP_CS_CONFIG_SHADEROBJOFFSET(0) | A5XX_SP_CS_CONFIG_ENABLED);
	pkt4(REG_A5XX_HLSQ_CS_CONSTLEN, 2); out(CONSTLEN / 4); out(INSTRLEN);
	/* fd5_emit_shader_obj: SP_CS_OBJ_START_LO/HI, PVT_MEM_PARAM, PVT_MEM_ADDR (2), PVT_MEM_SIZE */
	pkt4(REG_A5XX_SP_CS_OBJ_START_LO, 6);
	reloc(BUF_CODE, 0);
	out(A5XX_SP_VS_PVT_MEM_PARAM_MEMSIZEPERITEM(0) | A5XX_SP_VS_PVT_MEM_PARAM_HWSTACKOFFSET(0));
	out(0); out(0);
	out(A5XX_SP_VS_PVT_MEM_SIZE_TOTALPVTMEMSIZE(0));
	pkt4(REG_A5XX_HLSQ_UPDATE_CNTL, 1); out(0x1f00000);
	pkt4(REG_A5XX_HLSQ_CS_CNTL_0, 2);
	out(A5XX_HLSQ_CS_CNTL_0_WGIDCONSTID(0xc0) | A5XX_HLSQ_CS_CNTL_0_UNK0(regid(63, 0)) |
	    A5XX_HLSQ_CS_CNTL_0_UNK1(regid(63, 0)) | A5XX_HLSQ_CS_CNTL_0_LOCALIDREGID(0x00));
	out(0x1);
	/* fd5_emit_shader: the code, direct, into the CS shader block */
	pkt7(CP_LOAD_STATE4, 3 + 64);
	out(CP_LOAD_STATE4_0_DST_OFF(0) | CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) | CP_LOAD_STATE4_0_STATE_BLOCK(SB4_CS_SHADER) |
	    CP_LOAD_STATE4_0_NUM_UNIT(INSTRLEN));
	out(CP_LOAD_STATE4_1_EXT_SRC_ADDR(0) | CP_LOAD_STATE4_1_STATE_TYPE(ST4_SHADER));
	out(CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));
	for (int i = 0; i < 64; i++) {
		out(code[i]);
	}

	/* fd5_emit_cs_state: no textures, one SSBO (size, then address) */
	pkt4(REG_A5XX_TPL1_CS_TEX_COUNT, 1); out(0);
	pkt7(CP_LOAD_STATE4, 3 + 2);
	out(CP_LOAD_STATE4_0_DST_OFF(0) | CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) | CP_LOAD_STATE4_0_STATE_BLOCK(SB4_CS_SSBO) |
	    CP_LOAD_STATE4_0_NUM_UNIT(1));
	out(CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS) | CP_LOAD_STATE4_1_EXT_SRC_ADDR(0));
	out(CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));
	{
		unsigned sz = LOCAL * GROUPS * 4;
		out(A5XX_SSBO_1_0_WIDTH(sz & 0xffff));
		out(A5XX_SSBO_1_1_HEIGHT(sz >> 16));
	}
	pkt7(CP_LOAD_STATE4, 3 + 2);
	out(CP_LOAD_STATE4_0_DST_OFF(0) | CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) | CP_LOAD_STATE4_0_STATE_BLOCK(SB4_CS_SSBO) |
	    CP_LOAD_STATE4_0_NUM_UNIT(1));
	out(CP_LOAD_STATE4_1_STATE_TYPE(ST4_UBO) | CP_LOAD_STATE4_1_EXT_SRC_ADDR(0));
	out(CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));
	reloc(BUF_OUT, 0);

	/* constants: the UBO range (the seed), the immediates, the driver params (ir3_driver_params_cs) */
	pkt7(CP_WAIT_FOR_IDLE, 0);
	{
		uint32_t ubo[4] = { SEED, 0, 0, 0 };
		uint32_t imm[4] = { 0x9e3779b1, 0, 0, 0 };
		uint32_t dp[16] = { GROUPS, 1, 1, 3, 0, 0, 0, 0, LOCAL, 1, 1, 0, 0, 0, 0, 0 };

		load_consts(UBO_VEC4, ubo, 4);
		load_consts(IMM_VEC4, imm, 4);
		pkt7(CP_WAIT_FOR_IDLE, 0);
		load_consts(0, dp, 16);
	}

	/* fd5_launch_grid */
	pkt4(REG_A5XX_HLSQ_CS_NDRANGE_0, 7);
	out(A5XX_HLSQ_CS_NDRANGE_0_KERNELDIM(3) | A5XX_HLSQ_CS_NDRANGE_0_LOCALSIZEX(LOCAL - 1) |
	    A5XX_HLSQ_CS_NDRANGE_0_LOCALSIZEY(0) | A5XX_HLSQ_CS_NDRANGE_0_LOCALSIZEZ(0));
	out(A5XX_HLSQ_CS_NDRANGE_1_GLOBALSIZE_X(LOCAL * GROUPS)); out(0);
	out(A5XX_HLSQ_CS_NDRANGE_3_GLOBALSIZE_Y(1)); out(0);
	out(A5XX_HLSQ_CS_NDRANGE_5_GLOBALSIZE_Z(1)); out(0);
	pkt4(REG_A5XX_HLSQ_CS_KERNEL_GROUP_X, 3); out(1); out(1); out(1);
	pkt7(CP_EXEC_CS, 4);
	out(0); out(CP_EXEC_CS_1_NGROUPS_X(GROUPS)); out(CP_EXEC_CS_2_NGROUPS_Y(1)); out(CP_EXEC_CS_3_NGROUPS_Z(1));
	/* so that memory holds the result: wait for the shader, flush the caches */
	pkt7(CP_WAIT_FOR_IDLE, 0);
	pkt7(CP_EVENT_WRITE, 1); out(CACHE_FLUSH);
	pkt7(CP_WAIT_FOR_IDLE, 0);

	printf("/* generated by tools/a5cs/a5cs_gen.c from Mesa's A5xx register headers: do not edit */\n");
	printf("#define A5CS_LOCAL %d\n#define A5CS_GROUPS %d\n#define A5CS_SEED 0x%xu\n", LOCAL, GROUPS, SEED);
	printf("static const uint32_t a5cs_stream[%d] = {", n);
	for (int i = 0; i < n; i++) {
		printf("%s0x%08x,", i % 8 ? " " : "\n\t", w[i]);
	}
	printf("\n};\nstatic const struct { int at, buf; uint32_t off; } a5cs_reloc[%d] = {\n", nrel);
	for (int i = 0; i < nrel; i++) {
		printf("\t{ %d, %d, %u },\n", rel[i].at, rel[i].buf, rel[i].off);
	}
	printf("};\nstatic const uint32_t a5cs_code[64] = {");
	for (int i = 0; i < 64; i++) {
		printf("%s0x%08x,", i % 8 ? " " : "\n\t", code[i]);
	}
	printf("\n};\n");
	return 0;
}
