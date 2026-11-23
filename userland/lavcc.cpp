// lavcc: the compiler of hakios-metal (MIT) behind a C interface, for the plug-in: a .metallib's bytes in, a function by
// name, a lav5_kernel out (machine code and the fields the A5xx dispatch builds its stream from).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include "hakios-metal/compiler.hpp"
#include "hakios-metal/metallib.hpp"
#include "lava5.h"
#include "lavcc.h"

using namespace hakios::metal;

struct lavcc_lib {
    std::vector<uint8_t> bytes;
    MetalLibrary lib;
};

extern "C" struct lavcc_lib* lavcc_open(const void* data, size_t size, char* why, size_t whysize)
{
    auto* l = new lavcc_lib();
    l->bytes.assign(static_cast<const uint8_t*>(data), static_cast<const uint8_t*>(data) + size);
    if (!l->lib.parse(l->bytes.data(), l->bytes.size())) {
        snprintf(why, whysize, "%s", l->lib.why().c_str());
        delete l;
        return nullptr;
    }
    return l;
}

extern "C" void lavcc_close(struct lavcc_lib* l) { delete l; }

extern "C" unsigned lavcc_function_count(struct lavcc_lib* l) { return unsigned(l->lib.functions().size()); }

extern "C" const char* lavcc_function_name(struct lavcc_lib* l, unsigned i, int* type)
{
    if (i >= l->lib.functions().size()) return nullptr;
    if (type) *type = l->lib.functions()[i].type;
    return l->lib.functions()[i].name.c_str();
}

extern "C" int lavcc_compile(struct lavcc_lib* l, const char* name, struct lav5_kernel* out, char* why, size_t whysize)
{
    const MetalFunction* fn = l->lib.find(name);
    if (!fn) {
        snprintf(why, whysize, "no function %s", name);
        return 0;
    }
    if (fn->type != MetalFunction::kKernel) {
        snprintf(why, whysize, "%s is not a kernel function", name);
        return 0;
    }
    const uint8_t* bc = nullptr;
    uint64_t bcSize = 0;
    if (!l->lib.bitcode(*fn, &bc, &bcSize)) {
        snprintf(why, whysize, "no bitcode for %s", name);
        return 0;
    }
    std::string w;
    auto k = compileAir(bc, bcSize, &w);
    if (!k) {
        snprintf(why, whysize, "%s", w.c_str());
        return 0;
    }
    const Kernel& c = k->kernel;
    memset(out, 0, sizeof *out);
    out->nwords = c.numWords;
    out->words = static_cast<uint32_t*>(malloc(c.numWords * 4));
    memcpy(out->words, c.words, c.numWords * 4);
    out->instrlen = c.instrlen;
    out->constlen = c.constlen;
    out->max_reg = c.maxReg;
    out->max_half_reg = c.maxHalfReg;
    out->double_threadsize = c.doubleThreadsize;
    out->branchstack = c.branchstackHw;
    out->localid_regid = c.localIdRegid;
    out->wgid_regid = c.workGroupIdRegid;
    out->nimm = c.numImmediates;
    if (c.numImmediates) {
        out->imm = static_cast<uint32_t*>(malloc(c.numImmediates * 4));
        memcpy(out->imm, c.immediates, c.numImmediates * 4);
    }
    out->imm_vec4 = c.immediatesOffsetVec4;
    out->dp_vec4 = c.driverParamsOffsetVec4;
    out->num_ubos = c.numUbos;
    out->num_ssbos = c.numSsbos;
    out->num_ibos = c.numIbos;
    out->ssbo_undeclared = c.ssboUndeclared;
    out->shared_size = c.sharedSize > k->threadgroupBytes ? c.sharedSize : k->threadgroupBytes;
    out->pvtmem_size = c.pvtmemSize;
    out->const_data_size = c.constantDataSize;
    out->nranges = c.numUboRanges < LAV5_MAX_RANGES ? c.numUboRanges : LAV5_MAX_RANGES;
    for (uint32_t i = 0; i < out->nranges; i++) {
        out->ranges[i] = { c.uboRanges[i].block, c.uboRanges[i].offsetVec4, c.uboRanges[i].startDword, c.uboRanges[i].sizeDwords };
    }
    for (const auto& r : k->resources) {
        if ((r.kind == Resource::kConstantBuffer || r.kind == Resource::kDeviceBuffer) && r.index < LAV5_MAX_BUFFERS)
            out->buffer_mask |= 1u << r.index;
        else
            out->other_resources++;
    }
    if (k->hasThreadgroupArg) out->other_resources++;
    snprintf(out->name, sizeof out->name, "%s", k->name.c_str());
    return 1;
}

extern "C" void lavcc_kernel_free(struct lav5_kernel* k)
{
    free(k->words);
    free(k->imm);
    k->words = nullptr;
    k->imm = nullptr;
}
