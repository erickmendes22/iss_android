// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <chrono>
#include <cstdio>
#include <mutex>

namespace IssReadCost {
using Clock = std::chrono::steady_clock;
enum Phase { Queue, Owner, GpuWait, Publish, Image, Capture, Fallback, PhaseCount };
struct Totals {
    std::array<unsigned long long, PhaseCount> ns{};
    unsigned long long samples{}, total{}, requests{}, ready{}, copies{}, bytes{};
};
inline thread_local Totals* active{};
inline std::mutex lock;
inline Totals window{};

class Bind {
    Totals* previous = active;
public:
    explicit Bind(Totals* value) { active = value; }
    ~Bind() { active = previous; }
};
inline auto Now(Totals* sample) { return sample ? Clock::now() : Clock::time_point{}; }
inline void Add(Totals* sample, Phase phase, Clock::time_point start) {
    if (sample) sample->ns[phase] += std::chrono::duration_cast<std::chrono::nanoseconds>(
        Clock::now() - start).count();
}
class Timer {
    Totals* sample = active;
    Phase phase;
    Clock::time_point start = Now(sample);
public:
    explicit Timer(Phase p) : phase(p) {}
    ~Timer() { Add(sample, phase, start); }
};
inline void Commit(const Totals& s, unsigned long long total) {
    std::scoped_lock guard{lock};
    ++window.samples;
    window.total += total;
    window.requests += s.requests;
    window.ready += s.ready;
    window.bytes += s.bytes;
    window.copies += s.copies;
    for (unsigned i=0; i<PhaseCount; ++i) window.ns[i] += s.ns[i];
}
inline void Report(unsigned frames) {
    Totals w;
    { std::scoped_lock guard{lock}; w = window; window = {}; }
    const double scale = 32.0 / (1e6 * frames);
    std::printf("READBACK_COST frames=%u sample_1_in=32 samples=%llu reads_per_frame_est=%.2f "
                "sample_owner_requests=%llu sample_ready_at_first_owner_exit=%llu "
                "sample_copy_ranges=%llu sample_snapshot_bytes=%llu "
                "thread_ms_per_frame_est total=%.3f image=%.3f queue=%.3f "
                "owner_incl_publish=%.3f gpu_wait=%.3f publish_incl_capture=%.3f "
                "capture=%.3f fallback=%.3f\n",
                frames,w.samples,w.samples*32.0/frames,w.requests,w.ready,w.copies,w.bytes,
                w.total*scale,w.ns[Image]*scale,w.ns[Queue]*scale,w.ns[Owner]*scale,
                w.ns[GpuWait]*scale,w.ns[Publish]*scale,w.ns[Capture]*scale,w.ns[Fallback]*scale);
}
}
