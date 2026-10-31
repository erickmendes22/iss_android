// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#ifdef ISS_ANDROID_BIONIC
#include <pthread.h>
#include <time.h>
#endif

namespace IssBionicPerf {
enum class Stage : unsigned {
    FrameSlot, AhbAcquire, AhbImport, AhbExport, AhbSend, Submission, DriverSubmit,
    TimelineQuery, TimelineWait, HostCopies, RecorderSync, Count
};
struct Counter { std::atomic<std::uint64_t> ns{0}, calls{0}; };
struct Sample { std::uint64_t ns{}, calls{}; };
inline std::array<Counter, static_cast<unsigned>(Stage::Count)> counters{};
inline std::atomic<std::uint64_t> host_copy_skipped{0}, host_copy_required{0};
inline std::atomic<std::uint64_t> descriptor_template_calls{0}, descriptor_template_bytes{0},
    descriptor_legacy_calls{0};
inline std::atomic<std::uint64_t> fast_draw_key_hits{0};
inline std::atomic<std::uint64_t> fast_pipeline_bind_skipped{0};
inline std::atomic<std::uint64_t> stage_bind_memo_hits{0}, stage_bind_memo_misses{0};
inline std::atomic<std::uint64_t> descriptor_replay_skipped{0};
inline std::atomic<std::uint64_t> descriptor_delta_partial{0}, descriptor_delta_bindings{0};
inline std::atomic<std::uint64_t> vertex_input_issued{0}, vertex_input_skipped{0};
inline std::atomic<std::uint64_t> parallel_batches{0}, parallel_splits{0}, parallel_deferred_host{0};
enum class Thread : unsigned { Command, Draw, Recorder, Recorder2 };
inline std::array<std::atomic<int>, 4> thread_clocks{-1, -1, -1, -1};

inline bool Enabled() {
#ifdef ISS_ANDROID_BIONIC
    static const bool value = [] {
        const char* flag=std::getenv("ISS_COMPARE_STATS");
        return flag && flag[0]=='1';
    }();
    return value;
#else
    return false;
#endif
}

// Sample CPU time only at the five-second report, without per-packet instrumentation.
inline void RegisterThread([[maybe_unused]] Thread thread) {
#ifdef ISS_ANDROID_BIONIC
    if (!Enabled()) return;
    clockid_t clock;
    if (pthread_getcpuclockid(pthread_self(), &clock) == 0) {
        thread_clocks[static_cast<unsigned>(thread)].store(clock, std::memory_order_release);
    }
#endif
}

inline std::array<std::uint64_t, 4> CpuTimes() {
    std::array<std::uint64_t, 4> result{};
#ifdef ISS_ANDROID_BIONIC
    for (unsigned i = 0; i < result.size(); ++i) {
        const auto clock = i == 3 ? CLOCK_PROCESS_CPUTIME_ID
                                 : thread_clocks[i].load(std::memory_order_acquire);
        timespec time{};
        if (clock != -1 && clock_gettime(clock, &time) == 0) {
            result[i] = std::uint64_t(time.tv_sec) * 1000000000ull + time.tv_nsec;
        }
    }
#endif
#ifdef ISS_ANDROID_BIONIC
    const auto second = thread_clocks[static_cast<unsigned>(Thread::Recorder2)].load(std::memory_order_acquire);
    timespec extra{};
    if (second != -1 && clock_gettime(second, &extra) == 0) {
        result[2] += std::uint64_t(extra.tv_sec) * 1000000000ull + extra.tv_nsec;
    }
#endif
    return result;
}

// Only frame/resource waits and actual queue submissions are timed. No per-draw clocks,
// cycle-counter barriers, packet atomics, guest memory reads or changes to GPU synchronization.
class Scope {
    Stage stage;
    bool enabled;
    std::chrono::steady_clock::time_point start;
public:
    explicit Scope(Stage s) : stage{s}, enabled{Enabled()},
        start{enabled ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}} {}
    ~Scope() {
        if (!enabled) return;
        auto& c=counters[static_cast<unsigned>(stage)];
        c.ns.fetch_add(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now()-start).count(), std::memory_order_relaxed);
        c.calls.fetch_add(1,std::memory_order_relaxed);
    }
};

inline Sample Take(Stage stage) {
    auto& c=counters[static_cast<unsigned>(stage)];
    return {c.ns.exchange(0,std::memory_order_relaxed),c.calls.exchange(0,std::memory_order_relaxed)};
}

// Called by the single AHB presentation thread. Times are sums of host durations in the
// window; queue_submit is a subset of submission_host, not an additional GPU-time measurement.
inline bool OnPresent(std::uint32_t compiles, std::uint64_t compile_ns) {
    if (!Enabled()) return false;
    using Clock=std::chrono::steady_clock;
    static auto start=Clock::now();
    static auto previous_cpu=CpuTimes();
    static std::uint64_t frames=0, previous_compile_ns=compile_ns;
    static std::uint32_t previous_compiles=compiles;
    ++frames;
    const auto now=Clock::now();
    const double seconds=std::chrono::duration<double>(now-start).count();
    if (seconds<5.0) return false;
    const auto cpu=CpuTimes();
    std::array<double, 4> cpu_ms{};
    for (unsigned i = 0; i < cpu.size(); ++i) {
        if (previous_cpu[i] && cpu[i] >= previous_cpu[i]) {
            cpu_ms[i] = (cpu[i] - previous_cpu[i]) / 1e6;
        }
    }
    previous_cpu=cpu;
    const auto slot=Take(Stage::FrameSlot), acquire=Take(Stage::AhbAcquire),
               acquire_import=Take(Stage::AhbImport), export_=Take(Stage::AhbExport), send=Take(Stage::AhbSend),
               submit=Take(Stage::Submission), driver=Take(Stage::DriverSubmit),
               query=Take(Stage::TimelineQuery), wait=Take(Stage::TimelineWait),
               host=Take(Stage::HostCopies), recorder=Take(Stage::RecorderSync);
    std::printf("BionicPerf: present_fps=%.2f window_s=%.2f frames=%llu submits=%llu "
                "submission_host_ms=%.2f queue_submit_ms=%.2f frame_slot_ms=%.2f "
                "ahb_acquire_ms=%.2f ahb_import_ms=%.2f ahb_export_ms=%.2f ahb_send_ms=%.2f "
                "timeline_queries=%llu timeline_query_ms=%.2f gpu_waits=%llu gpu_wait_ms=%.2f "
                "command_cpu_ms=%.2f draw_cpu_ms=%.2f recorder_cpu_ms=%.2f process_cpu_ms=%.2f "
                "compiles=%u compile_ms=%.2f\n",
        frames/seconds, seconds, static_cast<unsigned long long>(frames),
        static_cast<unsigned long long>(submit.calls), submit.ns/1e6, driver.ns/1e6,
        slot.ns/1e6, acquire.ns/1e6, acquire_import.ns/1e6, export_.ns/1e6, send.ns/1e6,
        static_cast<unsigned long long>(query.calls), query.ns/1e6,
        static_cast<unsigned long long>(wait.calls), wait.ns/1e6,
        cpu_ms[0], cpu_ms[1], cpu_ms[2], cpu_ms[3],
        compiles-previous_compiles,(compile_ns-previous_compile_ns)/1e6);
    std::printf("BionicHostCopies: skipped=%llu required=%llu all_waits=%llu wait_ms=%.2f "
                "recorder_syncs=%llu recorder_sync_ms=%.2f\n",
        static_cast<unsigned long long>(host_copy_skipped.exchange(0)),
        static_cast<unsigned long long>(host_copy_required.exchange(0)),
        static_cast<unsigned long long>(host.calls), host.ns/1e6,
        static_cast<unsigned long long>(recorder.calls), recorder.ns/1e6);
    std::printf("BionicDescriptors: template_attempts=%llu replay_skipped=%llu "
                "partial_updates=%llu partial_bindings=%llu "
                "legacy_calls=%llu payload_bytes=%llu\n",
                static_cast<unsigned long long>(descriptor_template_calls.exchange(0)),
                static_cast<unsigned long long>(descriptor_replay_skipped.exchange(0)),
                static_cast<unsigned long long>(descriptor_delta_partial.exchange(0)),
                static_cast<unsigned long long>(descriptor_delta_bindings.exchange(0)),
                static_cast<unsigned long long>(descriptor_legacy_calls.exchange(0)),
                static_cast<unsigned long long>(descriptor_template_bytes.exchange(0)));
    std::printf("BionicFastDraw: reused_pipeline_selections=%llu redundant_graphics_binds=%llu\n",
                static_cast<unsigned long long>(fast_draw_key_hits.exchange(0)),
                static_cast<unsigned long long>(fast_pipeline_bind_skipped.exchange(0)));
    std::printf("BionicStageMemo: unprepared_hits=%llu misses=%llu\n",
                static_cast<unsigned long long>(stage_bind_memo_hits.exchange(0)),
                static_cast<unsigned long long>(stage_bind_memo_misses.exchange(0)));
    std::printf("BionicVertexInput: issued=%llu skipped=%llu\n",
                static_cast<unsigned long long>(vertex_input_issued.exchange(0)),
                static_cast<unsigned long long>(vertex_input_skipped.exchange(0)));
    const auto batches = parallel_batches.exchange(0);
    const auto splits = parallel_splits.exchange(0);
    if (batches) {
        std::printf("BionicParallelRecording: batches=%llu split=%llu command_buffers=%llu deferred_host=%llu\n",
                    static_cast<unsigned long long>(batches),
                    static_cast<unsigned long long>(splits),
                    static_cast<unsigned long long>(batches + splits),
                    static_cast<unsigned long long>(parallel_deferred_host.exchange(0)));
    }
    std::fflush(stdout);
    start=now;frames=0;previous_compiles=compiles;previous_compile_ns=compile_ns;
    return true;
}
} // namespace IssBionicPerf
