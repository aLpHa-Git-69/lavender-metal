// PM4 type 7 packet building for the A5xx command processor (the encoding the device accepted in lavgpu=11/12:
// header = 0x7 << 28 | count | parity(count) << 15 | opcode << 16 | parity(opcode) << 23, odd parity).
#pragma once
#include <cstdint>
#include <initializer_list>
#include <vector>
#include "a5xx.hpp"

namespace lavender::pm4 {

// 1 when v has an even number of set bits, so that v plus the bit is odd.
inline uint32_t parity_bit(uint32_t v) {
    v ^= v >> 16;
    v ^= v >> 8;
    v ^= v >> 4;
    return (0x9669u >> (v & 0xf)) & 1u;
}

inline uint32_t pkt7(uint32_t opcode, uint32_t count) {
    return 0x70000000u | (count & 0x3fff) | (parity_bit(count & 0x3fff) << 15) | ((opcode & 0x7f) << 16) |
           (parity_bit(opcode & 0x7f) << 23);
}

class Stream {
public:
    void put(uint32_t v) { words_.push_back(v); }
    void packet(uint32_t opcode, std::initializer_list<uint32_t> payload) {
        put(pkt7(opcode, static_cast<uint32_t>(payload.size())));
        for (uint32_t w : payload) put(w);
    }
    void me_init() {
        packet(a5xx::CP_ME_INIT, {0x0000002f, 0x00000003, 0x20000000, 0, 0, 0, 0, 0});
    }
    void set_secure_mode(uint32_t on) { packet(a5xx::CP_SET_SECURE_MODE, {on}); }
    void mem_write(uint64_t addr, uint32_t value) {
        packet(a5xx::CP_MEM_WRITE, {static_cast<uint32_t>(addr), static_cast<uint32_t>(addr >> 32), value});
    }
    const std::vector<uint32_t>& words() const { return words_; }
    void clear() { words_.clear(); }

private:
    std::vector<uint32_t> words_;
};

}  // namespace lavender::pm4
