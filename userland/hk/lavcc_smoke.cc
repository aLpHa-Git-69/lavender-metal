// lavcc_smoke: the compiler of hakios-metal (MIT) used as the plug-in will use it: a function of a .metallib (or one
// module's bitcode, .bc) in, a Kernel (machine code and every layout field) out. Prints what the dispatch will need.
//   lavcc_smoke kernel.bc | lavcc_smoke lib.metallib function
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "hakios-metal/compiler.hpp"
#include "hakios-metal/metallib.hpp"

using namespace hakios::metal;

int main(int argc, char** argv)
{
    if (argc < 2) { fprintf(stderr, "usage: lavcc_smoke kernel.bc | lib.metallib function\n"); return 2; }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    std::vector<uint8_t> file;
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) file.insert(file.end(), buf, buf + n);
    fclose(f);
    printf("compiler in this build: %s\n", hasCompiler() ? "yes" : "no");

    const uint8_t* bc = file.data();
    uint64_t bcSize = file.size();
    MetalLibrary lib;
    if (argc > 2) {
        if (!lib.parse(file.data(), file.size())) { printf("not a metallib: %s\n", lib.why().c_str()); return 1; }
        for (const auto& fn : lib.functions()) printf("  function %s (type %d)\n", fn.name.c_str(), fn.type);
        const MetalFunction* fn = lib.find(argv[2]);
        if (!fn) { printf("no function %s\n", argv[2]); return 1; }
        if (!lib.bitcode(*fn, &bc, &bcSize)) { printf("no bitcode for %s\n", argv[2]); return 1; }
    }
    std::string why;
    auto k = compileAir(bc, bcSize, &why);
    if (!k) { printf("refused: %s\n", why.c_str()); return 1; }
    const Kernel& c = k->kernel;
    printf("kernel %s: %u words, instrlen %u, constlen %u, maxReg %d, localId r%u, wgId r%u, immediates %u at vec4 %u, driver params at vec4 %d, ubos %u ssbos %u ibos %u, ubo ranges %u\n",
           k->name.c_str(), c.numWords, c.instrlen, c.constlen, c.maxReg, c.localIdRegid, c.workGroupIdRegid, c.numImmediates,
           c.immediatesOffsetVec4, c.driverParamsOffsetVec4, c.numUbos, c.numSsbos, c.numIbos, c.numUboRanges);
    printf("  words:");
    for (uint32_t i = 0; i < c.numWords && i < 8; i++) printf(" %08x", c.words[i]);
    putchar(10);
    for (uint32_t i = 0; i < c.numUboRanges; i++)
        printf("  ubo range %u: block %u -> vec4 %u, from dword %u, %u dwords\n", i, c.uboRanges[i].block, c.uboRanges[i].offsetVec4,
               c.uboRanges[i].startDword, c.uboRanges[i].sizeDwords);
    for (auto& r : k->resources)
        printf("  resource: kind %d index %u size %u %s%s\n", (int)r.kind, r.index, r.sizeBytes, r.read ? "r" : "", r.write ? "w" : "");
    return 0;
}
