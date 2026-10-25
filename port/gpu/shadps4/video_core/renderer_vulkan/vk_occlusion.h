// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <span>
#include <boost/container/static_vector.hpp>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

class Instance;

/**
 * tlg: guest occlusion queries counted by the GPU (TLG_OCCLUSION_MODE=4), ported from the
 * weshayoub92 shadPS4 fork (the one the Castlesea inFAMOUS port builds on).
 *
 * The guest dumps the depth block sample counters (ZPASS_DONE) before the draws it tests, at a
 * 16 byte aligned address, and after them, 8 bytes further; the sum of the differences of the
 * counter pairs is the number of samples that passed the depth test. The counters run all the
 * time on the GPU, so queries may overlap or nest. Here Vulkan occlusion queries count the
 * samples of consecutive segments of the command stream, shared by all the open guest queries:
 * a segment ends at each dump and at the end of each rendering scope (Vulkan queries begun in a
 * rendering scope must end in it). A guest query reports the segments between its two dumps,
 * one per counter pair.
 *
 * Used on the thread that owns the scheduler (the draw recording thread when the draw pipe is
 * in use): the dumps run in order with the draws (Rasterizer::RunInOrder).
 */
class OcclusionQueries final : public RenderingObserver {
public:
    static constexpr u32 MaxCounterPairs = 16;
    static constexpr u32 MaxOpenQueries = 16;

    using QueryList = boost::container::static_vector<u32, MaxCounterPairs>;

    explicit OcclusionQueries(const Instance& instance, Scheduler& scheduler);
    ~OcclusionQueries() override;

    void OnBeginRendering() override;
    void OnEndRendering() override;
    bool AllowsSecondaryRendering() const noexcept override {
        return open.empty() && !active;
    }

    /// Begin dump: counts the samples of the following draws for the query at this address.
    /// Returns false when it cannot be counted (too many open queries).
    bool Begin(VAddr address, u32 num_pairs);

    /// End dump of the query that began at begin_address: stops counting it and fills the
    /// Vulkan queries that counted its samples, oldest first. Returns false when no counted
    /// query began there, or when some of its samples were not counted (too many segments).
    bool End(VAddr begin_address, QueryList& out_queries);

    /// Starts counting again for the open queries after an end dump, if a rendering scope is
    /// active (otherwise the next one starts counting).
    void Resume();

    /// A guest submission (Rasterizer::OnSubmit, the draw pipe drained): queries open for many
    /// submissions are forgotten (never ended), so that they do not keep segments and open
    /// slots forever. The fork counts guest frames; submissions are what this port has here.
    void OnSubmit();

    /// The queries are read by the current command buffer: not reused before it completes.
    void MarkUsed(std::span<const u32> queries);

    [[nodiscard]] u32 NumOpen() const noexcept {
        return static_cast<u32>(open.size());
    }

    [[nodiscard]] vk::QueryPool Pool() const noexcept {
        return *pool;
    }

    /// Segments whose samples were lost (no free query, too many segments).
    [[nodiscard]] u64 Dropped() const noexcept {
        return dropped;
    }

private:
    struct OpenQuery {
        VAddr address;
        u32 max_segments;  ///< Counter pairs of the query: one segment each.
        u32 first_segment; ///< Index in segments of its first segment.
        u32 submit;        ///< Guest submission of its begin dump.
        bool incomplete;   ///< Some of its samples were not counted.
    };

    /// Forgets the segments that no open query needs any more.
    void DropOldSegments();

    void BeginSegment();
    void EndSegment();
    /// Ends the active segment: the samples before and after a dump are counted apart.
    void Cut();

    static constexpr u32 NumQueries = 4096;
    static constexpr u32 MaxSegments = 256;
    static constexpr u32 MaxOpenSubmits = 64;

    const Instance& instance;
    Scheduler& scheduler;
    vk::UniqueQueryPool pool;
    vk::QueryControlFlags control_flags{};
    /// GPU tick of the command buffer that last used each query (reused when it completed).
    std::array<u64, NumQueries> query_ticks{};
    u32 next_query{};
    bool active{}; ///< The last segment is counting in the current rendering scope.
    u32 submit{};  ///< Guest submissions.
    u64 dropped{};
    boost::container::static_vector<OpenQuery, MaxOpenQueries> open;
    /// Vulkan queries of the segments since the oldest open query began, in order.
    boost::container::static_vector<u32, MaxSegments> segments;
};

} // namespace Vulkan
