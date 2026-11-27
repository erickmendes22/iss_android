// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>

namespace IssReadCost {
using Clock = std::chrono::steady_clock;
enum Phase { Queue, Owner, GpuWait, Publish, Image, Capture, Fallback, PhaseCount };
// One ring per Vulkan scheduler. A missing or overwritten tick stays unknown.
struct Submission {
    unsigned long long tick{}, work{}, dispatches{};
    Clock::time_point submitted{};
    unsigned long long prior_batches{}, prior_work{}, prior_dispatches{};
};
class SubmissionHistory {
    std::array<Submission, 64> entries{};
    std::mutex mutex;
public:
    void Record(unsigned long long tick, unsigned work, unsigned dispatches) {
        const Submission entry{tick, work, dispatches, Clock::now()};
        std::scoped_lock guard{mutex};
        entries[tick % entries.size()] = entry;
    }
    Submission Find(unsigned long long tick, unsigned long long completed) {
        std::scoped_lock guard{mutex};
        auto entry = entries[tick % entries.size()];
        if (!tick || entry.tick != tick) return {};
        for (auto prior=tick-1; prior>completed && tick-prior<entries.size(); --prior) {
            const auto& batch = entries[prior % entries.size()];
            if (batch.tick != prior) break;
            ++entry.prior_batches;
            entry.prior_work += batch.work;
            entry.prior_dispatches += batch.dispatches;
        }
        return entry;
    }
};
enum class Origin { Forced, Submitted, Staging, Unknown, Count };
inline Origin Classify(bool mapped, unsigned long long tick, unsigned long long open) {
    if (!mapped) return Origin::Staging;
    if (tick == open) return Origin::Forced;
    return tick < open ? Origin::Submitted : Origin::Unknown;
}
struct WaitCost {
    unsigned long long waits{}, predecessor_ns{}, target_ns{}, work{}, stamps{}, after_fault_ns{};
    unsigned long long dispatches{}, prior_batches{}, prior_work{}, prior_dispatches{};
    void Merge(const WaitCost& v) {
        dispatches += v.dispatches;
        prior_batches += v.prior_batches;
        prior_work += v.prior_work;
        prior_dispatches += v.prior_dispatches;
        waits += v.waits;
        predecessor_ns += v.predecessor_ns;
        target_ns += v.target_ns;
        work += v.work;
        stamps += v.stamps;
        after_fault_ns += v.after_fault_ns;
    }
};
struct Totals {
    Clock::time_point fault_start{};
    std::array<WaitCost, static_cast<size_t>(Origin::Count)> producers{};
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
template<class Semaphore>
inline void MeasureWait(Totals& sample, Semaphore& semaphore, unsigned long long tick,
                        Origin origin, Submission submission) {
    auto& cost = sample.producers[static_cast<size_t>(origin)];
    ++cost.waits;
    if (submission.tick && submission.tick == tick) {
        ++cost.stamps;
        cost.work += submission.work;
        cost.dispatches += submission.dispatches;
        cost.prior_batches += submission.prior_batches;
        cost.prior_work += submission.prior_work;
        cost.prior_dispatches += submission.prior_dispatches;
        if (submission.submitted > sample.fault_start)
            cost.after_fault_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(
                submission.submitted - sample.fault_start).count();
    }
    const auto start = Clock::now();
    auto prior = start;
    if (tick > 1 && !semaphore.IsFree(tick - 1)) {
        semaphore.Wait(tick - 1);
        prior = Clock::now();
    }
    semaphore.Wait(tick);
    const auto end = Clock::now();
    cost.predecessor_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(prior-start).count();
    cost.target_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(end-prior).count();
}
inline void Commit(const Totals& s, unsigned long long total) {
    std::scoped_lock guard{lock};
    for (size_t i=0; i<s.producers.size(); ++i) window.producers[i].Merge(s.producers[i]);
    ++window.samples;
    window.total += total;
    window.requests += s.requests;
    window.ready += s.ready;
    window.bytes += s.bytes;
    window.copies += s.copies;
    for (unsigned i=0; i<PhaseCount; ++i) window.ns[i] += s.ns[i];
}
inline std::atomic<unsigned long long> forced_reads{}, early_submits{}, backlog_skips{};

// Rows describe the open producer at a forced read. Columns describe the
// last safe pass boundary in that same batch, before the read forced a flush.
constexpr unsigned WorkBin(unsigned work) {
    return work < 128 ? 0 : work < 256 ? 1 : work < 512 ? 2 : 3;
}
struct FaultWindow {
    std::array<std::array<unsigned long long, 5>, 4> counts{};
    unsigned long long operations{}, armed{}, backlog{};
};
inline FaultWindow faults;
inline std::mutex fault_lock;

// Only the immediately following batch gets an opportunity. Work is counted
// by the scheduler in draws and dispatches. A normal submission expires it.
class FeedbackSubmit {
    std::atomic<unsigned long long> armed_tick{};
    // Only accessed by the GPU command owner.
    unsigned long long boundary_tick{}, last_armed{}, last_backlog{};
    unsigned boundary_work{};
public:
    void NoteBoundary(unsigned long long tick, unsigned work) {
        boundary_tick = tick;
        boundary_work = work;
    }
    void NoteForced(unsigned long long tick, unsigned work) {
        std::scoped_lock guard{fault_lock};
        const unsigned column = tick == boundary_tick ? 1 + WorkBin(boundary_work) : 0;
        ++faults.counts[WorkBin(work)][column];
        faults.operations += work;
        faults.armed += tick == last_armed;
        faults.backlog += tick == last_backlog;
    }
    void Arm(unsigned long long tick) {
        last_armed = tick;
        armed_tick.store(tick, std::memory_order_relaxed);
        forced_reads.fetch_add(1, std::memory_order_relaxed);
    }
    template<class PreviousDone>
    bool AtBoundary(unsigned long long tick, unsigned work, unsigned normal_limit,
                    PreviousDone previous_done) {
        if (!normal_limit || work >= normal_limit ||
            work < std::max(256u, normal_limit / 2)) return false;
        auto expected = tick;
        if (!tick || !armed_tick.compare_exchange_strong(expected, 0, std::memory_order_relaxed))
            return false;
        // Queue behind one in-flight submission without growing a deeper backlog.
        if (tick > 2 && !previous_done(tick - 2)) {
            last_backlog = tick;
            backlog_skips.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        early_submits.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
};
inline void Report(unsigned frames) {
    std::printf("FEEDBACK_SUBMIT forced_reads=%llu early=%llu backlog_skip=%llu\n",
                forced_reads.exchange(0), early_submits.exchange(0), backlog_skips.exchange(0));
    FaultWindow snapshot;
    { std::scoped_lock guard{fault_lock}; snapshot = faults; faults = {}; }
    constexpr std::array bins{"0_127", "128_255", "256_511", "512_plus"};
    std::printf("FORCED_READ_BATCH work_sum=%llu armed=%llu backlog_rejected=%llu\n",
                snapshot.operations, snapshot.armed, snapshot.backlog);
    for (unsigned bin = 0; bin < bins.size(); ++bin) {
        const auto& row = snapshot.counts[bin];
        std::printf("FORCED_READ_BOUNDARY work=%s none=%llu safe_0_127=%llu "
                    "safe_128_255=%llu safe_256_511=%llu safe_512_plus=%llu\n",
                    bins[bin], row[0], row[1], row[2], row[3], row[4]);
    }
    Totals w;
    { std::scoped_lock guard{lock}; w = window; window = {}; }
    const double scale = 32.0 / (1e6 * frames);
    constexpr std::array names{"forced_mapped","submitted_mapped","staging","unknown"};
    for (size_t i=0; i<w.producers.size(); ++i) {
        const auto& v = w.producers[i];
        std::printf("READBACK_PRODUCER origin=%s sample_waits=%llu stamps=%llu "
                    "target_work=%llu target_dispatches=%llu prior_batches=%llu prior_work=%llu prior_dispatches=%llu submit_after_fault_ms_sum=%.3f "
                    "prior_thread_ms_per_frame_est=%.3f target_thread_ms_per_frame_est=%.3f\n",
                    names[i], v.waits, v.stamps, v.work, v.dispatches, v.prior_batches, v.prior_work, v.prior_dispatches, v.after_fault_ns / 1e6,
                    v.predecessor_ns*scale, v.target_ns*scale);
    }
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
