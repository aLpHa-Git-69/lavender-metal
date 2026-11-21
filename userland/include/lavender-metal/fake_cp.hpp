// A fake A5xx command processor for host tests. It walks a ring of type 7 packets as strictly as the real one proved
// to be on the device: parity checked, ME_INIT required first, MEM_WRITE refused while in secure mode, and a packet
// that runs past the write pointer leaves the CP stuck until a software reset. Memory is a flat byte array addressed
// by GPU virtual address (the device ran with an identity SMMU table, so VA = PA).
#pragma once
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
#include "pm4.hpp"

namespace lavender {

class FakeCp {
public:
    explicit FakeCp(size_t mem_bytes) : mem_(mem_bytes, 0) {}

    uint32_t read32(uint64_t addr) const {
        uint32_t v;
        std::memcpy(&v, &mem_.at(addr), 4);
        return v;
    }
    void write32(uint64_t addr, uint32_t v) { std::memcpy(&mem_.at(addr), &v, 4); }

    // Register writes the driver does; only the ones the CP model cares about are acted on.
    void reg_write(uint32_t reg, uint32_t v) {
        regs_[reg] = v;
        if (reg == a5xx::REG_A5XX_CP_RB_BASE) rb_base_ = (rb_base_ & ~0xffffffffull) | v;
        if (reg == a5xx::REG_A5XX_CP_RB_BASE_HI) rb_base_ = (rb_base_ & 0xffffffffull) | (uint64_t(v) << 32);
        if (reg == a5xx::REG_A5XX_CP_RB_CNTL) rb_dwords_ = 8u << (v & 0x3f);  // BUFSZ = log2(bytes / 8), in dwords
        if (reg == a5xx::REG_A5XX_RBBM_SW_RESET_CMD) {
            if (v == 1) reset_pending_ = true;
            if (v == 0 && reset_pending_) hard_reset();
        }
        if (reg == a5xx::REG_A5XX_CP_RB_WPTR) run(v);
    }
    uint32_t reg_read(uint32_t reg) const {
        if (reg == a5xx::REG_A5XX_CP_RB_RPTR) return rptr_;
        if (reg == a5xx::REG_A5XX_RBBM_SECVID_TRUST_CNTL) return secure_ ? 1 : 0;
        auto it = regs_.find(reg);
        return it == regs_.end() ? 0 : it->second;
    }

    bool stuck() const { return stuck_; }
    const std::string& error() const { return error_; }
    bool me_inited() const { return me_inited_; }

private:
    void hard_reset() {
        reset_pending_ = false;
        rptr_ = 0;
        secure_ = true;
        me_inited_ = false;
        stuck_ = false;
        regs_.clear();
    }

    void fail(const std::string& why) {
        if (error_.empty()) error_ = why;
    }

    uint32_t ring(uint32_t idx) const { return read32(rb_base_ + 4ull * (idx % rb_dwords_)); }

    bool unhalted() const {
        auto it = regs_.find(a5xx::REG_A5XX_CP_PFP_ME_CNTL);
        return it != regs_.end() && it->second == 0;
    }

    void run(uint32_t wptr) {
        if (!unhalted() || stuck_) return;  // a halted CP does not fetch; a stuck one stays stuck until reset
        while (rptr_ != wptr) {
            uint32_t hdr = ring(rptr_);
            if ((hdr >> 28) != 0x7) {
                fail("not a type 7 packet");
                rptr_ = wptr;
                return;
            }
            uint32_t cnt = hdr & 0x3fff, opc = (hdr >> 16) & 0x7f;
            if (((hdr >> 15) & 1) != pm4::parity_bit(cnt) || ((hdr >> 23) & 1) != pm4::parity_bit(opc)) {
                fail("bad packet parity");
                rptr_ = wptr;
                return;
            }
            uint32_t avail = (wptr + rb_dwords_ - rptr_ - 1) % rb_dwords_;
            if (cnt > avail) {  // the packet runs past the write pointer: the CP waits for the rest
                rptr_ = wptr;
                stuck_ = true;
                return;
            }
            uint32_t p[16] = {0};
            for (uint32_t i = 0; i < cnt && i < 16; i++) p[i] = ring(rptr_ + 1 + i);
            if (!exec(opc, cnt, p)) {
                rptr_ = wptr;
                return;
            }
            rptr_ = (rptr_ + 1 + cnt) % rb_dwords_;
        }
    }

    bool exec(uint32_t opc, uint32_t cnt, const uint32_t* p) {
        if (!me_inited_ && opc != a5xx::CP_ME_INIT) {
            fail("packet before ME_INIT");
            return false;
        }
        switch (opc) {
        case a5xx::CP_ME_INIT:
            if (cnt != 8) {
                fail("ME_INIT needs 8 dwords");
                return false;
            }
            me_inited_ = true;
            return true;
        case a5xx::CP_NOP:
            return true;
        case a5xx::CP_SET_SECURE_MODE:
            if (cnt != 1) {
                fail("SET_SECURE_MODE needs 1 dword");
                return false;
            }
            secure_ = p[0] != 0;
            return true;
        case a5xx::CP_MEM_WRITE:
            if (cnt != 3) {
                fail("MEM_WRITE count");
                return false;
            }
            if (secure_) {
                fail("MEM_WRITE while in secure mode");
                return false;
            }
            write32(p[0] | (uint64_t(p[1]) << 32), p[2]);
            return true;
        default:
            fail("unknown opcode " + std::to_string(opc));
            return false;
        }
    }

    std::vector<uint8_t> mem_;
    std::map<uint32_t, uint32_t> regs_;
    uint64_t rb_base_ = 0;
    uint32_t rb_dwords_ = 4096, rptr_ = 0;
    bool me_inited_ = false, secure_ = true, stuck_ = false, reset_pending_ = false;
    std::string error_;
};

}  // namespace lavender
