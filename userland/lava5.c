/*
 * lava5: see lava5.h. The stream is a5cs_gen.c's, with the kernel, the bindings and the grid as arguments.
 */
#include <stdint.h>
#include <string.h>
#include "lava5.h"

/* the register headers' float encoders come from Mesa's util; no register this builder writes takes a float */
static inline uint32_t
fui(float f)
{
	uint32_t u;

	memcpy(&u, &f, 4);
	return u;
}

static inline uint16_t
_mesa_float_to_half(float f)
{
	(void)f;
	return 0;
}

#include "a5xx/adreno_common.xml.h"
#include "a5xx/adreno_pm4.xml.h"
#include "a5xx/a3xx.xml.h"
#include "a5xx/a5xx.xml.h"

struct st {
	uint32_t * w;
	size_t n, cap;
	int over;
};

static unsigned
par(unsigned v)
{
	return (__builtin_popcount(v) & 1) ^ 1;
}

static void
out(struct st * s, uint32_t v)
{
	if (s->n < s->cap) {
		s->w[s->n++] = v;
	} else {
		s->over = 1;
	}
}

static void
pkt4(struct st * s, unsigned reg, unsigned cnt)
{
	out(s, 0x40000000 | cnt | par(cnt) << 7 | (reg & 0x3ffff) << 8 | par(reg) << 27);
}

static void
pkt7(struct st * s, unsigned op, unsigned cnt)
{
	out(s, 0x70000000 | cnt | par(cnt) << 15 | (op & 0x7f) << 16 | par(op) << 23);
}

static void
set(struct st * s, unsigned reg, uint32_t v)
{
	pkt4(s, reg, 1);
	out(s, v);
}

#define regid(num, comp) (((num) << 2) | (comp))

/* constants straight into the CS shader block's constant file, in runs the packet can count */
static void
load_consts(struct st * s, unsigned vec4, const uint32_t * d, unsigned dwords)
{
	while (dwords > 0) {
		unsigned k = dwords > 256 ? 256 : dwords;

		pkt7(s, CP_LOAD_STATE4, 3 + k);
		out(s, CP_LOAD_STATE4_0_DST_OFF(vec4) | CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
		    CP_LOAD_STATE4_0_STATE_BLOCK(SB4_CS_SHADER) | CP_LOAD_STATE4_0_NUM_UNIT(k / 4));
		out(s, CP_LOAD_STATE4_1_EXT_SRC_ADDR(0) | CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS));
		out(s, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));
		for (unsigned i = 0; i < k; i++) {
			out(s, d[i]);
		}
		d += k;
		dwords -= k;
		vec4 += k / 4;
	}
}

/* fd5_emit_restore: the mode and ECO registers a compute dispatch also depends on */
static void
restore(struct st * s)
{
	pkt7(s, CP_SET_RENDER_MODE, 5);
	out(s, CP_SET_RENDER_MODE_0_MODE(BYPASS));
	out(s, 0); out(s, 0); out(s, 0); out(s, 0);
	pkt7(s, CP_WAIT_FOR_IDLE, 0);
	pkt4(s, REG_A5XX_UCHE_CACHE_INVALIDATE_MIN_LO, 5);
	out(s, 0); out(s, 0); out(s, 0); out(s, 0); out(s, 0x12);
	pkt7(s, CP_WAIT_FOR_IDLE, 0);
	set(s, REG_A5XX_HLSQ_UPDATE_CNTL, 0xfffff);
	set(s, REG_A5XX_RB_MODE_CNTL, 0x44);
	set(s, REG_A5XX_RB_DBG_ECO_CNTL, 0x00100000);
	set(s, REG_A5XX_VFD_MODE_CNTL, 0);
	set(s, REG_A5XX_PC_MODE_CNTL, 0x1f);
	set(s, REG_A5XX_SP_MODE_CNTL, 0x1e);
	set(s, REG_A5XX_SP_DBG_ECO_CNTL, 0x40000800);
	set(s, REG_A5XX_TPL1_MODE_CNTL, 0x544);
	pkt4(s, REG_A5XX_HLSQ_TIMEOUT_THRESHOLD_0, 2);
	out(s, 0x80); out(s, 0);
	set(s, REG_A5XX_VPC_DBG_ECO_CNTL, 0x400);
	set(s, REG_A5XX_HLSQ_MODE_CNTL, 1);
	set(s, REG_A5XX_VPC_MODE_CNTL, 0);
	pkt4(s, REG_A5XX_TPL1_VS_TEX_COUNT, 4);
	out(s, 0); out(s, 0); out(s, 0); out(s, 0);
	pkt4(s, REG_A5XX_TPL1_FS_TEX_COUNT, 2);
	out(s, 0); out(s, 0);
}

const char *
lav5_refuse_kernel(const struct lav5_kernel * k)
{
	if (k->other_resources != 0) {
		return "binds a texture, an image, a sampler or threadgroup memory: not served yet";
	}
	if (k->shared_size != 0) {
		return "uses threadgroup memory: not served yet";
	}
	if (k->pvtmem_size != 0) {
		return "uses private memory: not served yet";
	}
	if (k->const_data_size != 0) {
		return "has a constant data table: not served yet";
	}
	if (k->nwords == 0 || k->instrlen == 0) {
		return "has no code";
	}
	if (k->num_ssbos > LAV5_MAX_BUFFERS) {
		return "has more device buffers than the table holds";
	}
	return NULL;
}

size_t
lav5_build(uint32_t * words, size_t cap, const struct lav5_dispatch * d, const char ** why)
{
	const struct lav5_kernel * k = d->k;
	struct st st = { words, 0, cap, 0 }, * s = &st;
	unsigned threads = d->local[0] * d->local[1] * d->local[2];
	unsigned subgroup = k->double_threadsize ? 64 : 32;
	unsigned groups_x = d->groups[0], groups_y = d->groups[1], groups_z = d->groups[2];
	enum a3xx_threadsize tsize = k->double_threadsize ? FOUR_QUADS : TWO_QUADS;
	unsigned instrlen = k->instrlen;
	unsigned preload = instrlen < 32 ? instrlen : 32;
	unsigned half = k->max_half_reg < 0 ? 0 : (unsigned)k->max_half_reg + 1;
	unsigned full = k->max_reg < 0 ? 0 : (unsigned)k->max_reg + 1;
	uint32_t dp[16];

	*why = NULL;
	if (threads == 0 || threads > 1024 || groups_x == 0 || groups_y == 0 || groups_z == 0) {
		*why = "a grid or a threadgroup of zero threads, or more than 1024 in a group";
		return 0;
	}
	if (k->dp_vec4 >= 0 && (uint32_t)k->dp_vec4 + 4 > k->constlen) {
		*why = "the driver params run past the constant file";
		return 0;
	}

	restore(s);

	/* fd5 cs_program_emit */
	pkt4(s, REG_A5XX_SP_SP_CNTL, 1);
	out(s, 0);
	pkt4(s, REG_A5XX_HLSQ_CONTROL_0_REG, 1);
	out(s, A5XX_HLSQ_CONTROL_0_REG_FSTHREADSIZE(TWO_QUADS) | A5XX_HLSQ_CONTROL_0_REG_CSTHREADSIZE(tsize) | 0x880);
	pkt4(s, REG_A5XX_SP_CS_CTRL_REG0, 1);
	out(s, A5XX_SP_CS_CTRL_REG0_THREADSIZE(tsize) | A5XX_SP_CS_CTRL_REG0_HALFREGFOOTPRINT(half) |
	    A5XX_SP_CS_CTRL_REG0_FULLREGFOOTPRINT(full) | A5XX_SP_CS_CTRL_REG0_BRANCHSTACK(k->branchstack) |
	    A5XX_SP_CS_CTRL_REG0_BUFFER | 0x2);
	pkt4(s, REG_A5XX_HLSQ_CS_CONFIG, 1);
	out(s, A5XX_HLSQ_CS_CONFIG_CONSTOBJECTOFFSET(0) | A5XX_HLSQ_CS_CONFIG_SHADEROBJOFFSET(0) | A5XX_HLSQ_CS_CONFIG_ENABLED);
	pkt4(s, REG_A5XX_HLSQ_CS_CNTL, 1);
	out(s, A5XX_HLSQ_CS_CNTL_INSTRLEN(instrlen) | A5XX_HLSQ_CS_CNTL_SSBO_ENABLE);
	pkt4(s, REG_A5XX_SP_CS_CONFIG, 1);
	out(s, A5XX_SP_CS_CONFIG_CONSTOBJECTOFFSET(0) | A5XX_SP_CS_CONFIG_SHADEROBJOFFSET(0) | A5XX_SP_CS_CONFIG_ENABLED);
	pkt4(s, REG_A5XX_HLSQ_CS_CONSTLEN, 2);
	out(s, k->constlen / 4);
	out(s, instrlen);
	pkt4(s, REG_A5XX_SP_CS_OBJ_START_LO, 6);
	out(s, (uint32_t)d->code_gpu);
	out(s, (uint32_t)(d->code_gpu >> 32));
	out(s, A5XX_SP_VS_PVT_MEM_PARAM_MEMSIZEPERITEM(0) | A5XX_SP_VS_PVT_MEM_PARAM_HWSTACKOFFSET(0));
	out(s, 0);
	out(s, 0);
	out(s, A5XX_SP_VS_PVT_MEM_SIZE_TOTALPVTMEMSIZE(0));
	pkt4(s, REG_A5XX_HLSQ_UPDATE_CNTL, 1);
	out(s, 0x1f00000);
	pkt4(s, REG_A5XX_HLSQ_CS_CNTL_0, 2);
	out(s, A5XX_HLSQ_CS_CNTL_0_WGIDCONSTID(0xc0) | A5XX_HLSQ_CS_CNTL_0_UNK0(regid(63, 0)) |
	    A5XX_HLSQ_CS_CNTL_0_UNK1(regid(63, 0)) | A5XX_HLSQ_CS_CNTL_0_LOCALIDREGID(k->localid_regid));
	out(s, 0x1);

	/* fd5_emit_shader: the code into the CS shader block, from memory */
	pkt7(s, CP_LOAD_STATE4, 3);
	out(s, CP_LOAD_STATE4_0_DST_OFF(0) | CP_LOAD_STATE4_0_STATE_SRC(SS4_INDIRECT) |
	    CP_LOAD_STATE4_0_STATE_BLOCK(SB4_CS_SHADER) | CP_LOAD_STATE4_0_NUM_UNIT(preload));
	out(s, (uint32_t)d->code_gpu | ST4_SHADER);
	out(s, (uint32_t)(d->code_gpu >> 32));

	/* fd5_emit_cs_state: no textures; the device buffers as SSBOs (sizes, then addresses) */
	pkt4(s, REG_A5XX_TPL1_CS_TEX_COUNT, 1);
	out(s, 0);
	if (k->num_ssbos > 0) {
		unsigned n = k->num_ssbos;

		pkt7(s, CP_LOAD_STATE4, 3 + 2 * n);
		out(s, CP_LOAD_STATE4_0_DST_OFF(0) | CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
		    CP_LOAD_STATE4_0_STATE_BLOCK(SB4_CS_SSBO) | CP_LOAD_STATE4_0_NUM_UNIT(n));
		out(s, CP_LOAD_STATE4_1_STATE_TYPE(ST4_CONSTANTS) | CP_LOAD_STATE4_1_EXT_SRC_ADDR(0));
		out(s, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));
		for (unsigned i = 0; i < n; i++) {
			uint64_t sz = d->buf[i].gpu != 0 ? d->buf[i].bytes : 4;

			out(s, A5XX_SSBO_1_0_WIDTH((uint32_t)sz & 0xffff));
			out(s, A5XX_SSBO_1_1_HEIGHT((uint32_t)(sz >> 16)));
		}
		pkt7(s, CP_LOAD_STATE4, 3 + 2 * n);
		out(s, CP_LOAD_STATE4_0_DST_OFF(0) | CP_LOAD_STATE4_0_STATE_SRC(SS4_DIRECT) |
		    CP_LOAD_STATE4_0_STATE_BLOCK(SB4_CS_SSBO) | CP_LOAD_STATE4_0_NUM_UNIT(n));
		out(s, CP_LOAD_STATE4_1_STATE_TYPE(ST4_UBO) | CP_LOAD_STATE4_1_EXT_SRC_ADDR(0));
		out(s, CP_LOAD_STATE4_2_EXT_SRC_ADDR_HI(0));
		for (unsigned i = 0; i < n; i++) {
			uint64_t a = d->buf[i].gpu != 0 ? d->buf[i].gpu : d->hole_gpu;

			out(s, (uint32_t)a);
			out(s, (uint32_t)(a >> 32));
		}
	}

	/* constants: the pushed UBO ranges, the immediates, the driver params (ir3_driver_params_cs) */
	pkt7(s, CP_WAIT_FOR_IDLE, 0);
	for (unsigned i = 0; i < k->nranges; i++) {
		const struct lav5_range * r = &k->ranges[i];
		const struct lav5_binding * b = r->block < LAV5_MAX_BUFFERS ? &d->buf[r->block] : NULL;
		uint32_t tmp[256];

		if (r->size_dwords == 0 || r->size_dwords % 4 != 0 || r->size_dwords > 256) {
			*why = "a constant range that is not a whole number of vec4 or longer than 256 dwords";
			return 0;
		}
		if (r->offset_vec4 + r->size_dwords / 4 > k->constlen) {
			*why = "a constant range runs past the constant file";
			return 0;
		}
		if (b == NULL || b->cpu == NULL) {
			*why = "a constant range reads a buffer nothing is bound at";
			return 0;
		}
		memset(tmp, 0, r->size_dwords * 4);
		for (unsigned j = 0; j < r->size_dwords; j++) {
			uint64_t byte = (uint64_t)(r->start_dword + j) * 4;

			if (byte + 4 <= b->bytes) {
				memcpy(&tmp[j], (const uint8_t *)b->cpu + byte, 4);
			}
		}
		load_consts(s, r->offset_vec4, tmp, r->size_dwords);
	}
	if (k->nimm != 0) {
		unsigned n = (k->nimm + 3) & ~3u;
		uint32_t tmp[1024];

		if (k->imm_vec4 + n / 4 > k->constlen || n > 1024) {
			*why = "the immediates run past the constant file";
			return 0;
		}
		memset(tmp, 0, n * 4);
		memcpy(tmp, k->imm, k->nimm * 4);
		load_consts(s, k->imm_vec4, tmp, n);
	}
	if (k->dp_vec4 >= 0) {
		memset(dp, 0, sizeof(dp));
		dp[0] = groups_x; dp[1] = groups_y; dp[2] = groups_z;
		dp[3] = 3;
		dp[7] = subgroup;
		dp[8] = d->local[0]; dp[9] = d->local[1]; dp[10] = d->local[2];
		dp[11] = subgroup == 64 ? 6 : 5;
		pkt7(s, CP_WAIT_FOR_IDLE, 0);
		load_consts(s, (uint32_t)k->dp_vec4, dp, 16);
	}

	/* fd5_launch_grid */
	pkt4(s, REG_A5XX_HLSQ_CS_NDRANGE_0, 7);
	out(s, A5XX_HLSQ_CS_NDRANGE_0_KERNELDIM(3) | A5XX_HLSQ_CS_NDRANGE_0_LOCALSIZEX(d->local[0] - 1) |
	    A5XX_HLSQ_CS_NDRANGE_0_LOCALSIZEY(d->local[1] - 1) | A5XX_HLSQ_CS_NDRANGE_0_LOCALSIZEZ(d->local[2] - 1));
	out(s, A5XX_HLSQ_CS_NDRANGE_1_GLOBALSIZE_X(d->local[0] * groups_x));
	out(s, 0);
	out(s, A5XX_HLSQ_CS_NDRANGE_3_GLOBALSIZE_Y(d->local[1] * groups_y));
	out(s, 0);
	out(s, A5XX_HLSQ_CS_NDRANGE_5_GLOBALSIZE_Z(d->local[2] * groups_z));
	out(s, 0);
	pkt4(s, REG_A5XX_HLSQ_CS_KERNEL_GROUP_X, 3);
	out(s, 1); out(s, 1); out(s, 1);
	pkt7(s, CP_EXEC_CS, 4);
	out(s, 0);
	out(s, CP_EXEC_CS_1_NGROUPS_X(groups_x));
	out(s, CP_EXEC_CS_2_NGROUPS_Y(groups_y));
	out(s, CP_EXEC_CS_3_NGROUPS_Z(groups_z));

	/* so that memory holds the result: the shader's stores are visible only after a cache flush with a timestamp */
	pkt7(s, CP_WAIT_FOR_IDLE, 0);
	pkt7(s, CP_EVENT_WRITE, 1);
	out(s, CACHE_FLUSH);
	pkt7(s, CP_WAIT_FOR_IDLE, 0);
	pkt7(s, CP_EVENT_WRITE, 4);
	out(s, CACHE_FLUSH_TS);
	out(s, (uint32_t)d->flush_gpu);
	out(s, (uint32_t)(d->flush_gpu >> 32));
	out(s, 0xa4a4a4a4);
	pkt7(s, CP_WAIT_MEM_WRITES, 0);
	pkt7(s, CP_WAIT_FOR_IDLE, 0);

	if (s->over) {
		*why = "the stream is longer than the service takes";
		return 0;
	}
	return s->n;
}
