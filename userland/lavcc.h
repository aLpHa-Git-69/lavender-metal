/* lavcc: a .metallib's kernel functions compiled for the Adreno 512, behind a C interface (lavcc.cpp) */
#ifndef LAVCC_H
#define LAVCC_H

#include <stddef.h>
#include "lava5.h"

#ifdef __cplusplus
extern "C" {
#endif

struct lavcc_lib;

struct lavcc_fc {
	uint32_t index;
	uint64_t bits;
};

#define LAVCC_MAX_UBER_BUFS 4
#define LAVCC_UBER_BUF_CAP  256

struct lavcc_uber_buf {
	uint32_t binding;
	uint32_t len;
	uint8_t mask[LAVCC_UBER_BUF_CAP];
};

struct lavcc_uber_val {
	uint32_t binding;
	uint32_t len;
	const uint8_t * bytes;
	const uint8_t * fold;
};

/* the library from its bytes (copied); NULL with the reason in WHY */
struct lavcc_lib * lavcc_open(const void * data, size_t size, char * why, size_t whysize);
void lavcc_close(struct lavcc_lib * l);
unsigned lavcc_function_count(struct lavcc_lib * l);
const char * lavcc_function_name(struct lavcc_lib * l, unsigned i, int * type);
int lavcc_resolve_fc_name(struct lavcc_lib * l, const char * fname, const char * cname, uint32_t * out_index);
/* 1 with OUT filled (free it with lavcc_kernel_free), 0 with the reason in WHY */
int lavcc_compile(struct lavcc_lib * l, const char * name, const struct lavcc_fc * fcs, uint32_t nfcs,
    struct lav5_kernel * out, char * why, size_t whysize);
void lavcc_kernel_free(struct lav5_kernel * k);
/* a vertex and a fragment function compiled and linked (render pipeline); 1 with VS, FS and LINK filled (free each stage with
 * lavcc_stage_free), 0 with the reason in WHY */
int lavcc_compile_program(struct lavcc_lib * lv, const char * vname, const struct lavcc_fc * vfcs, uint32_t nvfcs,
    struct lavcc_lib * lf, const char * fname, const struct lavcc_fc * ffcs, uint32_t nffcs,
    struct lav5_stage * vs, struct lav5_stage * fs, struct lav5_link * link, char * why, size_t whysize);
int lavcc_uber_info(struct lavcc_lib * lf, const char * fname, const struct lavcc_fc * ffcs, uint32_t nffcs,
    struct lavcc_uber_buf * out_bufs, uint32_t * out_nbufs, char * summary, size_t sumsize, char * why, size_t whysize);
int lavcc_compile_program_spec(struct lavcc_lib * lv, const char * vname, const struct lavcc_fc * vfcs, uint32_t nvfcs,
    struct lavcc_lib * lf, const char * fname, const struct lavcc_fc * ffcs, uint32_t nffcs,
    const struct lavcc_uber_val * uvals, uint32_t nuvals,
    struct lav5_stage * vs, struct lav5_stage * fs, struct lav5_link * link, char * why, size_t whysize);
void lavcc_stage_free(struct lav5_stage * s);
/* function I of the library compiled alone: 1 with a SUMMARY of what it uses, 0 with the reason in WHY, -1 past the end */
int lavcc_probe(struct lavcc_lib * l, unsigned i, char * summary, size_t ssize, char * why, size_t whysize);

void lavcc_survey_pairs(struct lavcc_lib * l);

#ifdef __cplusplus
}
#endif

#endif
