// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Exact, per-recording-thread memo for push-descriptor *values*.
// Inspired by the Castlesea ISS descriptor replay fast path.  No guest pointers,
// texture contents, synchronization state or GPU completion are memoized.
//
// A hit may omit vkCmdPushDescriptorSetWithTemplateKHR only when the command
// buffer, pipeline layout, bind point, submission tick AND raw-command epoch
// match.  All guest resource lookups, uploads, residency checks, barriers and
// push constants still run for every draw.  Unrecognized descriptor paths
// invalidate the corresponding bind point.
#include <algorithm>
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace Vulkan {

template <class CommandBuffer, class PipelineLayout>
class DescriptorReplayMemo {
public:
    bool NeedsPush(CommandBuffer command, PipelineLayout layout, std::uint64_t tick,
                   std::uint64_t raw_epoch, unsigned bind_point,
                   std::span<const std::uint64_t> payload) {
        // Vulkan only defines graphics and compute bind points for this path.
        if (bind_point > 1 || payload.empty()) {
            Invalidate(bind_point);
            return true;
        }
        auto& entry = entries[bind_point];
        const bool same = entry.valid && entry.command == command && entry.layout == layout &&
                          entry.tick == tick && entry.raw_epoch == raw_epoch &&
                          entry.payload.size() == payload.size() &&
                          std::equal(entry.payload.begin(), entry.payload.end(), payload.begin());
        if (!same) {
            entry.command = command;
            entry.layout = layout;
            entry.tick = tick;
            entry.raw_epoch = raw_epoch;
            entry.payload.assign(payload.begin(), payload.end());
            entry.valid = true;
        }
        return !same;
    }

    void Invalidate(unsigned bind_point) {
        if (bind_point <= 1) {
            entries[bind_point].valid = false;
        } else {
            InvalidateAll();
        }
    }

    void InvalidateAll() {
        for (auto& entry : entries) entry.valid = false;
    }

private:
    struct Entry {
        CommandBuffer command{};
        PipelineLayout layout{};
        std::uint64_t tick{}, raw_epoch{};
        std::vector<std::uint64_t> payload{};
        bool valid = false;
    };
    std::array<Entry, 2> entries{};
};

} // namespace Vulkan
