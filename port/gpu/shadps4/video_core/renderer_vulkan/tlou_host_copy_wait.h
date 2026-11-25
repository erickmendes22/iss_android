// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

namespace Vulkan::Detail {

// Wait for the exact recorder-copy sequence requested by the guest-visible
// fence. The recording chunk is already published by KickRecording(true).
// Short completions use a few yields; long waits park instead of burning a
// whole core while the Vulkan recorder or other game workers need CPU time.
// Every release-store that advances completed must notify_all().
inline void WaitForHostCopySequence(std::atomic<std::uint64_t>& completed,
                                    std::uint64_t target) {
    unsigned short_spins = 0;
    while (completed.load(std::memory_order_acquire) < target) {
        if (short_spins++ < 4) {
            std::this_thread::yield();
        } else {
            const auto observed = completed.load(std::memory_order_acquire);
            if (observed < target) {
                completed.wait(observed, std::memory_order_acquire);
            }
        }
    }
}

} // namespace Vulkan::Detail
