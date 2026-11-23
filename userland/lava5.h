/*
 * lava5: the A5xx (Adreno 512) compute dispatch as a PM4 stream, and the flat description of a compiled kernel it
 * needs. The order of the packets is Mesa's gallium freedreno driver (a5xx/fd5_compute.c, fd5_emit.c, ir3_const.h; MIT,
 * Copyright Rob Clark and the Mesa contributors); the register encoders are the headers Mesa generates (a5xx/).
 * tools/a5cs/a5cs_gen.c is the same stream for one fixed kernel, and was verified on the phone (256 of 256 words).
 */
#ifndef LAVA5_H
#define LAVA5_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LAV5_MAX_BUFFERS 31
#define LAV5_MAX_RANGES  32

struct lav5_range {
	uint32_t block;         /* Metal's [[buffer(n)]] the range is read from */
	uint32_t offset_vec4;   /* where in the constant file it lands */
	uint32_t start_dword;   /* first dword of the buffer that is pushed */
	uint32_t size_dwords;   /* a multiple of 4 */
};

/* what a compiled kernel needs to be dispatched (lavcc.cpp fills it from hakios's Kernel) */
struct lav5_kernel {
	uint32_t * words;       /* the machine code, nwords dwords */
	uint32_t nwords;
	uint32_t instrlen, constlen;
	int32_t max_reg, max_half_reg;
	uint32_t double_threadsize;
	uint32_t branchstack;
	uint32_t localid_regid, wgid_regid;
	uint32_t * imm;         /* constants the kernel carries */
	uint32_t nimm;          /* dwords */
	uint32_t imm_vec4;
	int32_t dp_vec4;        /* driver params (ir3_driver_params_cs), -1 when the kernel reads none */
	uint32_t num_ubos, num_ssbos, num_ibos;
	uint32_t ssbo_undeclared;
	uint32_t shared_size, pvtmem_size;
	uint32_t const_data_size;
	uint32_t nranges;
	struct lav5_range ranges[LAV5_MAX_RANGES];
	uint32_t buffer_mask;   /* bit n: the kernel has a resource at [[buffer(n)]] */
	uint32_t other_resources;   /* a texture, an image, a sampler or a threadgroup argument: not served yet */
	char name[64];
};

/* a buffer bound at one Metal index: where the GPU sees it, where the processor sees it (for the constants it pushes) */
struct lav5_binding {
	uint64_t gpu;
	const void * cpu;
	uint64_t bytes;         /* what is bound, past its offset */
};

struct lav5_dispatch {
	const struct lav5_kernel * k;
	uint64_t code_gpu;                              /* the code, in memory */
	struct lav5_binding buf[LAV5_MAX_BUFFERS];
	uint32_t groups[3], local[3];
	uint64_t flush_gpu;                             /* four bytes the cache flush writes its timestamp to */
	uint64_t hole_gpu;                              /* memory nobody names, for an SSBO slot nothing is bound at */
};

/* the words of the stream; returns how many, 0 with *why set (a string literal) when the dispatch is refused */
size_t lav5_build(uint32_t * out, size_t cap, const struct lav5_dispatch * d, const char ** why);

/* a kernel this builder can run, or the reason it cannot (before a pipeline state exists) */
const char * lav5_refuse_kernel(const struct lav5_kernel * k);

#ifdef __cplusplus
}
#endif

#endif
