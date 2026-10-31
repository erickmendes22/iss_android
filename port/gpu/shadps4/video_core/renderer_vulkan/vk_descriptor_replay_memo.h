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
#include <cstddef>
#include <cstring>
#include <span>
#include <vector>

namespace Vulkan {

template <class CommandBuffer, class PipelineLayout>
class DescriptorReplayMemo {
public:
    struct ByteRange {
        std::size_t offset{}, size{};
    };
    enum class Mode { Full, Unchanged, Partial };
    struct Decision {
        Mode mode{Mode::Full};
        std::size_t changed{};
    };

    // Optional partial push-descriptor update. Ranges describe complete Vulkan
    // binding values in the packed template; pointer differences outside those
    // ranges are padding, not descriptor changes. Never reuse across layouts.
    Decision Select(CommandBuffer command, PipelineLayout layout, std::uint64_t tick,
                    std::uint64_t raw_epoch, unsigned bind_point,
                    std::span<const std::uint64_t> payload,
                    std::span<const ByteRange> ranges,
                    std::span<std::uint8_t> selected) {
        if (bind_point > 1 || payload.empty()) {
            Invalidate(bind_point);
            return {};
        }
        auto& entry = entries[bind_point];
        const bool compatible = entry.valid && entry.command == command &&
                                entry.layout == layout && entry.tick == tick &&
                                entry.raw_epoch == raw_epoch &&
                                entry.payload.size() == payload.size();
        if (!compatible) {
            (void)NeedsPush(command, layout, tick, raw_epoch, bind_point, payload);
            return {};
        }
        if (std::equal(entry.payload.begin(), entry.payload.end(), payload.begin())) {
            return {Mode::Unchanged, 0};
        }
        const auto* before = reinterpret_cast<const std::byte*>(entry.payload.data());
        const auto* after = reinterpret_cast<const std::byte*>(payload.data());
        const std::size_t bytes = payload.size_bytes();
        std::size_t changed = 0, end = 0;
        bool valid = !ranges.empty() && ranges.size() <= selected.size() &&
                     ranges.size() <= 128;
        if (valid) {
            for (std::size_t i = 0; i < ranges.size(); ++i) {
                const auto [offset, size] = ranges[i];
                if (size == 0 || offset < end || offset > bytes || size > bytes - offset) {
                    valid = false;
                    break;
                }
                end = offset + size;
                if (std::memcmp(before + offset, after + offset, size) != 0) {
                    selected[changed++] = static_cast<std::uint8_t>(i);
                }
            }
        }
        entry.payload.assign(payload.begin(), payload.end());
        if (!valid) return {};
        if (changed == 0) return {Mode::Unchanged, 0};
        // Partial Vulkan update is useful only when most bindings are unchanged.
        // Always fall back to the original template call for a large delta.
        if (changed * 2 <= ranges.size() && changed <= 16) {
            return {Mode::Partial, changed};
        }
        return {Mode::Full, changed};
    }

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
