/* lavcc: a .metallib's kernel functions compiled for the Adreno 512, behind a C interface (lavcc.cpp) */
#ifndef LAVCC_H
#define LAVCC_H

#include <stddef.h>
#include "lava5.h"

#ifdef __cplusplus
extern "C" {
#endif

struct lavcc_lib;

/* the library from its bytes (copied); NULL with the reason in WHY */
struct lavcc_lib * lavcc_open(const void * data, size_t size, char * why, size_t whysize);
void lavcc_close(struct lavcc_lib * l);
unsigned lavcc_function_count(struct lavcc_lib * l);
const char * lavcc_function_name(struct lavcc_lib * l, unsigned i, int * type);
/* 1 with OUT filled (free it with lavcc_kernel_free), 0 with the reason in WHY */
int lavcc_compile(struct lavcc_lib * l, const char * name, struct lav5_kernel * out, char * why, size_t whysize);
void lavcc_kernel_free(struct lav5_kernel * k);

#ifdef __cplusplus
}
#endif

#endif
