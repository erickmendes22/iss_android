// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include "tlg_quality.h"

namespace IssSession {
inline std::atomic<int32_t*> guest_level{nullptr};
inline std::atomic<uint64_t> gpu_busy_ns{0};
inline std::atomic<bool> gpu_timing{false};

// The VideoOut thread calls this once per source flip, before frame generation.
class Window {
    uint64_t last{}, start{}, sample{}, draws_at_sample{}, sample_frames{};
    std::array<unsigned, 201> intervals{};
    unsigned frames{}, over33{}, over50{};
    int last_target{};
    TlgQuality::Governor governor;
public:
    void Flip(uint64_t now, uint64_t draws, int target, int limit) {
        if (!last) {
            start = sample = last = now;
            draws_at_sample = draws;
            last_target = target;
            gpu_busy_ns.exchange(0);
            return;
        }
        const auto elapsed = now - last;
        last = now;
        ++intervals[std::min<uint64_t>((elapsed + 999999) / 1000000, 200)];
        ++frames;
        over33 += elapsed > 33000000;
        over50 += elapsed > 50000000;
        ++sample_frames;
        if (target != last_target) {
            governor = {};
            last_target = target;
            sample = now;
            draws_at_sample = draws;
            sample_frames = 0;
        }
        if (now - sample >= 1000000000) {
            governor.Update(target, sample_frames * 1e9 / (now - sample),
                            double(draws - draws_at_sample) / sample_frames);
            draws_at_sample = draws;
            sample_frames = 0;
            sample = now;
        }
        // The game may independently increase the level when its draw lists fill.
        auto* level = guest_level.load(std::memory_order_acquire);
        if (level && governor.Level()) {
            int32_t current = __atomic_load_n(level, __ATOMIC_RELAXED);
            if (current >= 0 && current < governor.Level())
                __atomic_compare_exchange_n(level, &current, governor.Level(), false,
                                            __ATOMIC_RELAXED, __ATOMIC_RELAXED);
        }
        if (now - start < 15000000000ull) return;
        unsigned p99 = 0, cumulative = intervals[0];
        while (p99 < 200 && cumulative * 100 < frames * 99)
            cumulative += intervals[++p99];
        std::printf("FRAME_PACE source=guest_flip p99_ms=%u capped_ms=200 over33=%u over50=%u frames=%u source_fps=%.2f\n",
                    p99, over33, over50, frames, frames * 1e9 / double(now - start));
        std::printf("SESSION_SETTINGS fps_limit=%d quality_target=%d quality_floor=%d supported=%d\n",
                    limit, target, level ? governor.Level() : 0, int(level != nullptr));
        const auto busy = gpu_busy_ns.exchange(0, std::memory_order_relaxed);
        if (gpu_timing.load() && busy)
            std::printf("GPU_FRAME_MS avg=%.2f busy_pct=%.1f frames=%u\n",
                        busy / (1e6 * frames), 100.0 * busy / (now - start), frames);
        start = now;
        frames = over33 = over50 = 0;
        intervals.fill(0);
    }
};
}
