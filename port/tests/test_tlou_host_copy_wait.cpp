// SPDX-License-Identifier: GPL-2.0-or-later
#include "../gpu/shadps4/video_core/renderer_vulkan/tlou_host_copy_wait.h"
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <time.h>

using namespace std::chrono_literals;
static double ThreadCpuMs() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

int main() {
    std::atomic<std::uint64_t> done{5};
    Vulkan::Detail::WaitForHostCopySequence(done, 5); // already completed
    // A recorder batch can publish several intermediate copy sequences at once.
    std::atomic<unsigned> arrivals{0};
    std::thread first([&] {
        arrivals.fetch_add(1);
        Vulkan::Detail::WaitForHostCopySequence(done, 10);
    });
    std::thread second([&] {
        arrivals.fetch_add(1);
        Vulkan::Detail::WaitForHostCopySequence(done, 12);
    });
    while (arrivals.load() != 2) std::this_thread::yield();
    done.store(11, std::memory_order_release);
    done.notify_all();
    done.store(12, std::memory_order_release);
    done.notify_all();
    first.join();
    second.join();

    std::atomic<std::uint64_t> sequence{0}, request{0};
    constexpr int N = 1200;
    std::thread recorder([&] {
        for (int i = 1; i <= N; ++i) {
            while (request.load(std::memory_order_acquire) <
                   static_cast<std::uint64_t>(i)) std::this_thread::yield();
            std::this_thread::sleep_for(80us);
            sequence.store(i, std::memory_order_release);
            sequence.notify_all();
        }
    });
    const double cpu_begin = ThreadCpuMs();
    const auto wall_begin = std::chrono::steady_clock::now();
    for (int i = 1; i <= N; ++i) {
        request.store(i, std::memory_order_release);
        Vulkan::Detail::WaitForHostCopySequence(sequence, i);
        assert(sequence.load(std::memory_order_acquire) >=
               static_cast<std::uint64_t>(i));
    }
    const double cpu_ms = ThreadCpuMs() - cpu_begin;
    const double wall_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - wall_begin).count();
    recorder.join();
    std::printf("TLOU host-copy waits: %d sequences, CPU %.3f ms / wall %.3f ms, correctness OK\\n",
                N, cpu_ms, wall_ms);
}
