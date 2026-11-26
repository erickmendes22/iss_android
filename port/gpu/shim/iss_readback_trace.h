// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>

namespace IssReadbackTrace {
// Keep recent writable bindings as candidates. Binding a descriptor does not
// prove that a shader wrote the sampled byte. Other producers may also exist.
struct Binding {
    uint64_t base{}, size{}, shader{};
    unsigned slot{};
};
struct Sample {
    uint64_t address{}, guest_pc{};
    int origin{};
    Binding binding{};
};
inline std::array<Binding, 128> bindings{};
inline std::array<Sample, 48> samples{};
inline unsigned cursor{}, count{};
inline std::atomic_flag busy = ATOMIC_FLAG_INIT;

inline void Bind(uint64_t base, uint64_t size, uint64_t shader, unsigned slot) {
    if (!size || busy.test_and_set(std::memory_order_acquire)) return;
    const auto& previous = bindings[(cursor - 1) % bindings.size()];
    if (previous.base != base || previous.size != size ||
        previous.shader != shader || previous.slot != slot) {
        bindings[cursor++ % bindings.size()] = {base, size, shader, slot};
    }
    busy.clear(std::memory_order_release);
}

inline void Read(uint64_t address, uint64_t guest_pc, int origin) {
    // Called from the fault handler. Never allocate, print, or wait for a writer.
    if (busy.test_and_set(std::memory_order_acquire)) return;
    if (count < samples.size()) {
        auto& s = samples[count++];
        s = {address, guest_pc, origin, {}};
        for (unsigned n = 0; n < bindings.size(); ++n) {
            const auto& b = bindings[(cursor - 1 - n) % bindings.size()];
            if (address >= b.base && address - b.base < b.size) {
                s.binding = b;
                break;
            }
        }
    }
    busy.clear(std::memory_order_release);
}

inline void Report() {
    if (busy.test_and_set(std::memory_order_acquire)) return;
    auto batch = samples;
    const unsigned used = count;
    count = 0;
    busy.clear(std::memory_order_release);
    for (unsigned i = 0; i < used; ++i) {
        const auto& s = batch[i];
        std::printf("READBACK_SITE origin=%d pc=%#llx address=%#llx matched=%d shader=%#llx slot=%u base=%#llx size=%llu\n",
                    s.origin, (unsigned long long)s.guest_pc, (unsigned long long)s.address,
                    int(s.binding.size != 0), (unsigned long long)s.binding.shader, s.binding.slot,
                    (unsigned long long)s.binding.base, (unsigned long long)s.binding.size);
    }
}
} // namespace IssReadbackTrace
