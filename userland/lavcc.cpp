// lavcc: the compiler of hakios-metal (MIT) behind a C interface, for the plug-in: a .metallib's bytes in, a function by
// name, a lav5_kernel out (machine code and the fields the A5xx dispatch builds its stream from).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdio>
#include "hakios-metal/compiler.hpp"
#include "hakios-metal/metallib.hpp"
#include "lava5.h"
#include "lavcc.h"

using namespace hakios::metal;

struct lavcc_lib {
    std::vector<uint8_t> bytes;
    MetalLibrary lib;
    std::map<std::string, std::map<std::string, uint32_t>> fc_name_cache;
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

extern "C" int lavcc_resolve_fc_name(struct lavcc_lib* l, const char* fname, const char* cname, uint32_t* out_index)
{
    if (!l || !fname || !cname || !out_index) return 0;
    auto it = l->fc_name_cache.find(fname);
    if (it == l->fc_name_cache.end()) {
        std::map<std::string, uint32_t> m;
        const MetalFunction* fn = l->lib.find(fname);
        if (fn) {
            const uint8_t* bc = nullptr;
            uint64_t n = 0;
            if (l->lib.bitcode(*fn, &bc, &n)) {
                std::vector<std::pair<std::string, uint32_t>> pairs;
                if (functionConstantIndexMap(bc, n, &pairs)) {
                    for (const auto& p : pairs) m[p.first] = p.second;
                }
            }
        }
        it = l->fc_name_cache.emplace(fname, std::move(m)).first;
    }
    auto jt = it->second.find(cname);
    if (jt == it->second.end()) return 0;
    *out_index = jt->second;
    return 1;
}

static FunctionConstants to_fcs(const struct lavcc_fc* fcs, uint32_t nfcs)
{
    FunctionConstants out;
    if (fcs && nfcs) {
        out.reserve(nfcs);
        for (uint32_t i = 0; i < nfcs; i++) out.push_back(FunctionConstant{ fcs[i].index, fcs[i].bits });
    }
    return out;
}

extern "C" int lavcc_compile(struct lavcc_lib* l, const char* name, const struct lavcc_fc* fcs, uint32_t nfcs,
                             struct lav5_kernel* out, char* why, size_t whysize)
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
    auto k = compileAir(bc, bcSize, &w, to_fcs(fcs, nfcs));
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

// ---- the 3D side: a vertex and a fragment function compiled, linked and flattened for lava5r.c ----

static void flatten(const Kernel& c, const std::vector<Resource>& resources, struct lav5_kernel* out)
{
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
    out->shared_size = c.sharedSize;
    out->pvtmem_size = c.pvtmemSize;
    out->const_data_size = c.constantDataSize;
    out->nranges = c.numUboRanges < LAV5_MAX_RANGES ? c.numUboRanges : LAV5_MAX_RANGES;
    for (uint32_t i = 0; i < out->nranges; i++)
        out->ranges[i] = { c.uboRanges[i].block, c.uboRanges[i].offsetVec4, c.uboRanges[i].startDword, c.uboRanges[i].sizeDwords };
    for (const auto& r : resources) {
        if ((r.kind == Resource::kConstantBuffer || r.kind == Resource::kDeviceBuffer) && r.index < LAV5_MAX_BUFFERS)
            out->buffer_mask |= 1u << r.index;
        else
            out->other_resources++;
    }
}

static void flatten_stage(const Stage& s, const std::vector<Resource>& resources, struct lav5_stage* out)
{
    flatten(s.k, resources, &out->k);
    out->is_fragment = s.kind == kStageFragment;
    out->nin = s.numInputs < LAV5_MAX_IO ? s.numInputs : LAV5_MAX_IO;
    for (uint32_t i = 0; i < out->nin; i++)
        out->in[i] = { s.inputs[i].slot, s.inputs[i].regid, s.inputs[i].compmask, s.inputs[i].inloc, s.inputs[i].sysval,
                       s.inputs[i].bary, s.inputs[i].flat, s.inputs[i].half };
    out->nout = s.numOutputs < 32 ? s.numOutputs : 32;
    for (uint32_t i = 0; i < out->nout; i++) {
        out->out[i].slot = s.outputs[i].slot;
        out->out[i].regid = s.outputs[i].regid;
        out->out[i].half = s.outputs[i].half;
    }
    out->nattr = s.numAttributes;
    out->vertex_id_regid = s.vertexIdRegid;
    out->instance_id_regid = s.instanceIdRegid;
    out->position_regid = s.positionRegid;
    out->point_size_regid = s.pointSizeRegid;
    out->clip_mask = s.clipMask;
    out->cull_mask = s.cullMask;
    out->total_in = s.totalIn;
    out->frag_coord_regid = s.fragCoordRegid;
    out->face_regid = s.faceRegid;
    out->frag_coord_compmask = s.fragcoordCompmask;
    for (uint32_t i = 0; i < 7; i++) out->ij_regid[i] = s.ijRegid[i];
    for (uint32_t i = 0; i < 8; i++) {
        out->color_regid[i] = s.colorRegid[i];
        out->color_half[i] = s.colorHalf[i];
    }
    out->ncolor = s.numColorOutputs;
    out->depth_regid = s.depthRegid;
    out->need_pixlod = s.needPixlod;
    out->has_kill = s.hasKill;
    out->writes_pos = s.writesPos;
}

static std::unique_ptr<CompiledStage> stage_of(struct lavcc_lib* l, const char* name, uint8_t type,
                                               const struct lavcc_fc* fcs, uint32_t nfcs, std::string* why)
{
    const MetalFunction* fn = l->lib.find(name);
    if (!fn) { *why = std::string("no function ") + name; return nullptr; }
    if (fn->type != type) { *why = std::string(name) + " is not the right kind of function"; return nullptr; }
    const uint8_t* bc = nullptr;
    uint64_t n = 0;
    if (!l->lib.bitcode(*fn, &bc, &n)) { *why = std::string("no bitcode for ") + name; return nullptr; }
    return compileStage(bc, n, why, to_fcs(fcs, nfcs));
}

extern "C" int lavcc_compile_program(struct lavcc_lib* lv, const char* vname, const struct lavcc_fc* vfcs, uint32_t nvfcs,
                                     struct lavcc_lib* lf, const char* fname, const struct lavcc_fc* ffcs, uint32_t nffcs,
                                     struct lav5_stage* vs, struct lav5_stage* fs, struct lav5_link* link, char* why, size_t whysize)
{
    std::string w;
    auto v = stage_of(lv, vname, MetalFunction::kVertex, vfcs, nvfcs, &w);
    if (!v) { snprintf(why, whysize, "vertex function: %s", w.c_str()); return 0; }
    auto f = stage_of(lf, fname, MetalFunction::kFragment, ffcs, nffcs, &w);
    if (!f) { snprintf(why, whysize, "fragment function: %s", w.c_str()); return 0; }
    Linkage lk{};
    if (!render::link(v->stage, f->stage, &lk, &w)) { snprintf(why, whysize, "link: %s", w.c_str()); return 0; }
    memset(vs, 0, sizeof *vs);
    memset(fs, 0, sizeof *fs);
    memset(link, 0, sizeof *link);
    flatten_stage(v->stage, v->resources, vs);
    flatten_stage(f->stage, f->resources, fs);
    link->cnt = lk.cnt;
    link->max_loc = lk.maxLoc;
    link->position_loc = lk.positionLoc;
    link->psize_loc = lk.psizeLoc;
    for (int i = 0; i < 4; i++) link->varmask[i] = lk.varmask[i];
    for (int i = 0; i < 8; i++) link->interp[i] = lk.interpMode[i];
    for (uint32_t i = 0; i < 32; i++) {
        link->var[i].slot = lk.var[i].slot;
        link->var[i].regid = lk.var[i].regid;
        link->var[i].compmask = lk.var[i].compmask;
        link->var[i].loc = lk.var[i].loc;
    }
    return 1;
}

extern "C" int lavcc_compile_vs(struct lavcc_lib* lv, const char* vname, const struct lavcc_fc* vfcs, uint32_t nvfcs,
                                struct lav5_stage* vs, char* why, size_t whysize)
{
    std::string w;
    auto v = stage_of(lv, vname, MetalFunction::kVertex, vfcs, nvfcs, &w);
    if (!v) { snprintf(why, whysize, "vertex function: %s", w.c_str()); return 0; }
    memset(vs, 0, sizeof *vs);
    flatten_stage(v->stage, v->resources, vs);
    return 1;
}

extern "C" int lavcc_uber_info(struct lavcc_lib* lf, const char* fname, const struct lavcc_fc* ffcs, uint32_t nffcs,
                               struct lavcc_uber_buf* out_bufs, uint32_t* out_nbufs, uint32_t* out_ifs,
                               char* summary, size_t sumsize, char* why, size_t whysize)
{
    *out_nbufs = 0;
    if (out_ifs) *out_ifs = 0;
    const MetalFunction* fn = lf->lib.find(fname);
    if (!fn || fn->type != MetalFunction::kFragment) {
        if (why && whysize) snprintf(why, whysize, "not a fragment function");
        return 0;
    }
    const uint8_t* bc = nullptr;
    uint64_t n = 0;
    if (!lf->lib.bitcode(*fn, &bc, &n)) {
        if (why && whysize) snprintf(why, whysize, "no bitcode");
        return 0;
    }
    std::string sum, w;
    std::vector<UberBufferInfo> ubs;
    uint32_t nifs = 0;
    if (!detectUberControlBuffers(bc, n, to_fcs(ffcs, nffcs), &ubs, &sum, &w, &nifs)) {
        if (out_ifs) *out_ifs = nifs;
        if (summary && sumsize) snprintf(summary, sumsize, "%s", sum.c_str());
        if (why && whysize) snprintf(why, whysize, "%s", w.c_str());
        return 0;
    }
    if (out_ifs) *out_ifs = nifs;
    if (summary && sumsize) snprintf(summary, sumsize, "%s", sum.c_str());
    uint32_t nb = 0;
    for (const auto& ub : ubs) {
        if (nb >= LAVCC_MAX_UBER_BUFS) break;
        if (ub.length == 0 || ub.length > LAVCC_UBER_BUF_CAP || ub.mask.size() < ub.length) continue;
        memset(&out_bufs[nb], 0, sizeof(out_bufs[nb]));
        out_bufs[nb].binding = ub.binding;
        out_bufs[nb].len = ub.length;
        memcpy(out_bufs[nb].mask, ub.mask.data(), ub.length);
        nb++;
    }
    *out_nbufs = nb;
    return nb > 0 ? 1 : 0;
}

extern "C" int lavcc_compile_program_spec(struct lavcc_lib* lv, const char* vname, const struct lavcc_fc* vfcs, uint32_t nvfcs,
                                          struct lavcc_lib* lf, const char* fname, const struct lavcc_fc* ffcs, uint32_t nffcs,
                                          const struct lavcc_uber_val* uvals, uint32_t nuvals,
                                          struct lav5_stage* vs, struct lav5_stage* fs, struct lav5_link* link, char* why, size_t whysize)
{
    std::string w;
    auto v = stage_of(lv, vname, MetalFunction::kVertex, vfcs, nvfcs, &w);
    if (!v) { snprintf(why, whysize, "vertex function: %s", w.c_str()); return 0; }
    const MetalFunction* fn = lf->lib.find(fname);
    if (!fn || fn->type != MetalFunction::kFragment) {
        snprintf(why, whysize, "fragment function %s not found", fname);
        return 0;
    }
    const uint8_t* bc = nullptr;
    uint64_t n = 0;
    if (!lf->lib.bitcode(*fn, &bc, &n)) {
        snprintf(why, whysize, "no bitcode for %s", fname);
        return 0;
    }
    if (!uvals || nuvals == 0) {
        snprintf(why, whysize, "no uber buffers");
        return 0;
    }
    KnownBuffer kb;
    kb.allowSsbo = true;
    kb.binding = uvals[0].binding;
    kb.bytes.assign(uvals[0].bytes, uvals[0].bytes + uvals[0].len);
    if (uvals[0].fold != nullptr) kb.fold.assign(uvals[0].fold, uvals[0].fold + uvals[0].len);
    for (uint32_t i = 1; i < nuvals; i++) {
        KnownBuffer e;
        e.allowSsbo = true;
        e.binding = uvals[i].binding;
        e.bytes.assign(uvals[i].bytes, uvals[i].bytes + uvals[i].len);
        if (uvals[i].fold != nullptr) e.fold.assign(uvals[i].fold, uvals[i].fold + uvals[i].len);
        kb.extra.push_back(std::move(e));
    }
    auto f = compileStage(bc, n, &w, to_fcs(ffcs, nffcs), 0, &kb);
    if (!f) { snprintf(why, whysize, "fragment spec: %s", w.c_str()); return 0; }
    Linkage lk{};
    if (!render::link(v->stage, f->stage, &lk, &w)) { snprintf(why, whysize, "link: %s", w.c_str()); return 0; }
    memset(vs, 0, sizeof *vs);
    memset(fs, 0, sizeof *fs);
    memset(link, 0, sizeof *link);
    flatten_stage(v->stage, v->resources, vs);
    flatten_stage(f->stage, f->resources, fs);
    link->cnt = lk.cnt;
    link->max_loc = lk.maxLoc;
    link->position_loc = lk.positionLoc;
    link->psize_loc = lk.psizeLoc;
    for (int i = 0; i < 4; i++) link->varmask[i] = lk.varmask[i];
    for (int i = 0; i < 8; i++) link->interp[i] = lk.interpMode[i];
    for (uint32_t i = 0; i < 32; i++) {
        link->var[i].slot = lk.var[i].slot;
        link->var[i].regid = lk.var[i].regid;
        link->var[i].compmask = lk.var[i].compmask;
        link->var[i].loc = lk.var[i].loc;
    }
    return 1;
}

extern "C" void lavcc_stage_free(struct lav5_stage* s) { lavcc_kernel_free(&s->k); }

// ---- a survey of a library: what each function needs, and whether this compiler serves it (lavsurvey) ----

extern "C" int lavcc_probe(struct lavcc_lib* l, unsigned i, char* summary, size_t ssize, char* why, size_t whysize)
{
    if (i >= l->lib.functions().size()) return -1;
    const MetalFunction& fn = l->lib.functions()[i];
    const uint8_t* bc = nullptr;
    uint64_t n = 0;
    summary[0] = 0;
    if (!l->lib.bitcode(fn, &bc, &n)) { snprintf(why, whysize, "no bitcode"); return 0; }
    std::string w;
    if (fn.type == MetalFunction::kKernel) {
        auto k = compileAir(bc, n, &w);
        if (!k) { snprintf(why, whysize, "%s", w.c_str()); return 0; }
        snprintf(summary, ssize, "kernel %u words", k->kernel.numWords);
        return 1;
    }
    auto s = compileStage(bc, n, &w);
    if (!s) { snprintf(why, whysize, "%s", w.c_str()); return 0; }
    unsigned buf = 0, tex = 0, smp = 0, img = 0;
    for (const auto& r : s->resources) {
        if (r.kind == Resource::kTexture) tex++;
        else if (r.kind == Resource::kSampler) smp++;
        else if (r.kind == Resource::kImage) img++;
        else buf++;
    }
    lav5_stage ls;
    memset(&ls, 0, sizeof ls);
    flatten_stage(s->stage, s->resources, &ls);
    const char* refuse = lav5_refuse_stage(&ls);
    snprintf(summary, ssize, "%s %u words, %u buffers %u textures %u samplers %u images; ssbos %u ubos %u ncolor %u%s%s", fn.type == MetalFunction::kVertex ? "vertex" : "fragment",
             s->stage.k.numWords, buf, tex, smp, img, ls.k.num_ssbos, ls.k.num_ubos, ls.ncolor, refuse ? " -- DRIVER REFUSES: " : "", refuse ? refuse : "");
    lavcc_stage_free(&ls);
    return 1;
}

// every vertex function with every fragment function of a library, each stage compiled once: which pairs link and which of those the
// driver refuses (lavsurvey pairs). Prints to stdout.
extern "C" void lavcc_survey_pairs(struct lavcc_lib* l)
{
    const auto& fns = l->lib.functions();
    std::vector<std::unique_ptr<CompiledStage>> st(fns.size());
    std::vector<std::string> whyc(fns.size());
    for (size_t i = 0; i < fns.size(); i++) {
        if (fns[i].type != MetalFunction::kVertex && fns[i].type != MetalFunction::kFragment) continue;
        std::string w;
        st[i] = stage_of(l, fns[i].name.c_str(), fns[i].type, nullptr, 0, &w);
        if (!st[i]) printf("NOCOMPILE %s: %s\n", fns[i].name.c_str(), w.c_str());
    }
    unsigned tried = 0, usable = 0, refused = 0, nolink = 0;
    std::map<std::string, unsigned> reasons, refusedBy;
    for (size_t i = 0; i < fns.size(); i++) {
        if (fns[i].type != MetalFunction::kVertex || !st[i]) continue;
        unsigned vusable = 0;
        for (size_t j = 0; j < fns.size(); j++) {
            if (fns[j].type != MetalFunction::kFragment || !st[j]) continue;
            tried++;
            Linkage lk{};
            std::string w;
            if (!render::link(st[i]->stage, st[j]->stage, &lk, &w)) {
                nolink++;
                for (auto& c : w) if (c >= '0' && c <= '9') c = '#';
                // drop the function-specific part before the first colon-free tail
                reasons[w.substr(0, 110)]++;
                continue;
            }
            struct lav5_stage vs, fs;
            memset(&vs, 0, sizeof vs);
            memset(&fs, 0, sizeof fs);
            flatten_stage(st[i]->stage, st[i]->resources, &vs);
            flatten_stage(st[j]->stage, st[j]->resources, &fs);
            const char* e = lav5_refuse_stage(&vs);
            const char* who = fns[i].name.c_str();
            if (!e) { e = lav5_refuse_stage(&fs); who = fns[j].name.c_str(); }
            if (e) {
                refused++;
                refusedBy[std::string(who) + ": " + e]++;
                reasons[std::string("REFUSED: ") + e]++;
            } else {
                usable++;
                vusable++;
            }
            lavcc_stage_free(&vs);
            lavcc_stage_free(&fs);
        }
        printf("VS %-44s usable with %u fragment functions\n", fns[i].name.c_str(), vusable);
        fflush(stdout);
    }
    printf("pairs: %u tried, %u usable, %u refused by the driver, %u do not link\n", tried, usable, refused, nolink);
    for (auto& kv : reasons) printf("  %5u x %s\n", kv.second, kv.first.c_str());
    for (auto& kv : refusedBy) printf("  refused %5u x %s\n", kv.second, kv.first.c_str());
}
