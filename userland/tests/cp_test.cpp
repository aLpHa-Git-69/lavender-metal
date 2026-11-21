// Host test: the packet encoding and the fake CP, mirroring what lavgpu=11 and lavgpu=12 did on the device.
#include <cstdio>
#include "lavender-metal/fake_cp.hpp"

using namespace lavender;
using namespace lavender::a5xx;

static int fails;
#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);       \
            fails++;                                                      \
        }                                                                 \
    } while (0)

static void write_ring(FakeCp& cp, uint64_t base, uint32_t& wptr, const pm4::Stream& s) {
    for (uint32_t w : s.words()) {
        cp.write32(base + 4ull * wptr, w);
        wptr = (wptr + 1) % 4096;
    }
}

static void setup(FakeCp& cp, uint64_t base) {
    cp.reg_write(REG_A5XX_CP_RB_CNTL, 12u | (2u << 8) | (1u << 27));
    cp.reg_write(REG_A5XX_CP_RB_BASE, static_cast<uint32_t>(base));
    cp.reg_write(REG_A5XX_CP_RB_BASE_HI, 0);
}

static void submit(FakeCp& cp, uint64_t base, uint32_t& wptr, const pm4::Stream& s) {
    write_ring(cp, base, wptr, s);
    cp.reg_write(REG_A5XX_CP_RB_WPTR, wptr);
}

int main() {
    // the registers the device accepted (dword offsets; the log used byte offset = dword * 4)
    CHECK(REG_A5XX_CP_RB_BASE == 0x800 && REG_A5XX_CP_RB_CNTL == 0x802 && REG_A5XX_CP_RB_RPTR == 0x806);
    CHECK(REG_A5XX_CP_PFP_ME_CNTL == 0x832 && REG_A5XX_RBBM_STATUS == 0x4f5 &&
          REG_A5XX_RBBM_SECVID_TRUST_CNTL == 0xf400);
    CHECK(CP_ME_INIT == 72 && CP_MEM_WRITE == 61 && CP_SET_SECURE_MODE == 102 && CP_NOP == 16);
    // golden headers: ME_INIT with 8 dwords is 0x70c80008 (opcode parity 1, count parity 0)
    CHECK(pm4::pkt7(CP_ME_INIT, 8) == 0x70c80008u);
    CHECK(pm4::pkt7(CP_SET_SECURE_MODE, 1) == 0x70e60001u);  // opcode 0x66 has 4 set bits -> parity 1; count 1 -> parity 0

    const uint64_t base = 0x4000;
    {  // the sequence the device ran: ME_INIT, SET_SECURE_MODE(0), MEM_WRITE fence, with the same RPTR values
        FakeCp cp(1 << 20);
        setup(cp, base);
        uint32_t wptr = 0;
        cp.reg_write(REG_A5XX_CP_PFP_ME_CNTL, 0);
        pm4::Stream s;
        s.me_init();
        submit(cp, base, wptr, s);
        CHECK(cp.reg_read(REG_A5XX_CP_RB_RPTR) == 9 && cp.me_inited());
        s.clear();
        s.set_secure_mode(0);
        submit(cp, base, wptr, s);
        CHECK(cp.reg_read(REG_A5XX_CP_RB_RPTR) == 11);
        CHECK(cp.reg_read(REG_A5XX_RBBM_SECVID_TRUST_CNTL) == 0);
        s.clear();
        s.mem_write(0x8100, 0xcafe0001);
        submit(cp, base, wptr, s);
        CHECK(cp.reg_read(REG_A5XX_CP_RB_RPTR) == 15);
        CHECK(cp.read32(0x8100) == 0xcafe0001 && cp.error().empty());
    }
    {  // a halted CP does not consume; MEM_WRITE in secure mode is refused
        FakeCp cp(1 << 20);
        setup(cp, base);
        uint32_t wptr = 0;
        pm4::Stream s;
        s.me_init();
        cp.reg_write(REG_A5XX_CP_PFP_ME_CNTL, 0x303);
        submit(cp, base, wptr, s);
        CHECK(cp.reg_read(REG_A5XX_CP_RB_RPTR) == 0);
        cp.reg_write(REG_A5XX_CP_PFP_ME_CNTL, 0);
        cp.reg_write(REG_A5XX_CP_RB_WPTR, wptr);
        CHECK(cp.reg_read(REG_A5XX_CP_RB_RPTR) == 9);
        s.clear();
        s.mem_write(0x8100, 1);
        submit(cp, base, wptr, s);
        CHECK(!cp.error().empty());
    }
    {  // the deliberate hang and the reset recovery of lavgpu=12
        FakeCp cp(1 << 20);
        setup(cp, base);
        uint32_t wptr = 0;
        cp.reg_write(REG_A5XX_CP_PFP_ME_CNTL, 0);
        pm4::Stream s;
        s.me_init();
        s.set_secure_mode(0);
        submit(cp, base, wptr, s);
        s.clear();
        s.put(pm4::pkt7(CP_NOP, 64));  // claims 64 dwords, only the next packet follows
        s.put(0);
        s.put(0);
        s.mem_write(0x8100, 0xdead0001);
        submit(cp, base, wptr, s);
        CHECK(cp.stuck() && cp.read32(0x8100) == 0);
        cp.reg_write(REG_A5XX_RBBM_SW_RESET_CMD, 1);
        cp.reg_write(REG_A5XX_RBBM_SW_RESET_CMD, 0);
        CHECK(cp.reg_read(REG_A5XX_CP_RB_RPTR) == 0 && cp.reg_read(REG_A5XX_RBBM_SECVID_TRUST_CNTL) == 1);
        setup(cp, base);
        wptr = 0;
        cp.reg_write(REG_A5XX_CP_PFP_ME_CNTL, 0);
        s.clear();
        s.me_init();
        s.set_secure_mode(0);
        s.mem_write(0x8100, 0xcafe0200);
        submit(cp, base, wptr, s);
        CHECK(!cp.stuck() && cp.read32(0x8100) == 0xcafe0200);
    }
    if (fails) {
        std::printf("cp_test: %d failures\n", fails);
        return 1;
    }
    std::printf("cp_test: all passed\n");
    return 0;
}
