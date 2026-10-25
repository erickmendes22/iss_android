// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
//
// Experimental ARM64/FEX adaptation of ISS PC's NATIVE_PASSES_SUBMIT approach.
// This is a read-only qualification stage, not an alternate renderer: it never
// edits PM4, changes synchronization, or claims that a Vulkan secondary can run.
// PC references: HayatoG/iss_pc port/gpu_v2/gpu/native_passes.cpp SubmitMode,
// ScanOnly and the real eboot-specific DrawIndexHook. The hook is deliberately
// NOT used here: FEX cannot call a sysv_abi x86 function pointer on AArch64.
//
// Only a successfully parsed, non-predicated run of direct draws is a possible
// pass. Actual render-target/pipeline identity, Vulkan secondary inheritance,
// guest memory lifetimes and token handoff must be checked before execution.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>

namespace AmdGpu {
struct NativePassSubmitScan {
    struct Result {
        std::uint64_t submissions = 1;
        std::uint64_t packets = 0;
        std::uint64_t direct_draws = 0;
        std::uint64_t candidate_draws = 0;
        std::uint64_t candidate_passes = 0;
        std::uint64_t boundaries = 0;
        std::uint64_t largest_pass = 0;
        bool valid = true;
    };

    static bool Enabled() {
#if defined(__ANDROID__) && defined(ISS_ANDROID_BIONIC) && defined(ARCH_ARM64)
        static const bool enabled = [] {
            const char* e = std::getenv("ISS_NATIVE_PASSES_ARM64");
            return e && e[0] == '1';
        }();
        return enabled;
#else
        return false;
#endif
    }

    static Result Analyze(std::span<const std::uint32_t> stream) {
        Result result;
        std::uint64_t run = 0;
        auto close = [&] {
            if (run >= 2) {
                ++result.candidate_passes;
                result.candidate_draws += run;
                result.largest_pass = std::max(result.largest_pass, run);
            }
            run = 0;
        };
        std::size_t at = 0;
        while (at < stream.size()) {
            const std::uint32_t word = stream[at];
            const std::uint32_t type = word >> 30;
            if (type == 2) {
                ++at; ++result.packets;
                close();
                ++result.boundaries;
                continue;
            }
            if (type != 3) {
                result.valid = false;
                break;
            }
            const std::uint32_t words = (((word >> 16) & 0x3fffU) + 1U) & 0x3fffU;
            if (!words || words >= stream.size() - at) {
                result.valid = false;
                break;
            }
            ++result.packets;
            const auto opcode = (word >> 8) & 0xffU;
            const bool predicated = (word & 1U) != 0;
            const bool is_direct_draw = opcode == 0x27U || opcode == 0x35U || opcode == 0x2dU;
            const bool is_register = opcode == 0x68U || opcode == 0x69U ||
                                     opcode == 0x76U || opcode == 0x79U || opcode == 0x2aU;
            // A small no-op is the trailer of DrawIndex2 (the PC records a token
            // there). Longer NOPs may contain debug markers or flip markers.
            const bool trivial_nop = opcode == 0x10U && words <= 3;
            if (is_direct_draw) {
                ++result.direct_draws;
                if (!predicated) {
                    ++run;
                } else {
                    close();
                    ++result.boundaries;
                }
            } else if (!is_register && !trivial_nop) {
                close();
                ++result.boundaries;
            }
            at += std::size_t(words) + 1;
        }
        close();
        if (!result.valid) {
            result.candidate_draws = 0;
            result.candidate_passes = 0;
            result.largest_pass = 0;
        }
        return result;
    }

    static void Report(const Result& sample) {
        struct Counters {
            std::atomic<std::uint64_t> submissions{0};
            std::atomic<std::uint64_t> draws{0};
            std::atomic<std::uint64_t> candidates{0};
            std::atomic<std::uint64_t> passes{0};
            std::atomic<std::uint64_t> malformed{0};
        };
        static Counters counters;
        counters.draws.fetch_add(sample.direct_draws, std::memory_order_relaxed);
        counters.candidates.fetch_add(sample.candidate_draws, std::memory_order_relaxed);
        counters.passes.fetch_add(sample.candidate_passes, std::memory_order_relaxed);
        if (!sample.valid) counters.malformed.fetch_add(1, std::memory_order_relaxed);
        if ((counters.submissions.fetch_add(1, std::memory_order_relaxed) + 1) % 512 == 0) {
            std::fprintf(stderr,
                "ISS_NATIVE_PASSES_ARM64 scan 512 submissions: draws=%llu candidates=%llu "
                "possible_passes=%llu malformed=%llu execution=original\n",
                static_cast<unsigned long long>(counters.draws.exchange(0)),
                static_cast<unsigned long long>(counters.candidates.exchange(0)),
                static_cast<unsigned long long>(counters.passes.exchange(0)),
                static_cast<unsigned long long>(counters.malformed.exchange(0)));
        }
    }
};
} // namespace AmdGpu
