// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/assert.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_occlusion.h"

namespace Vulkan {

OcclusionQueries::OcclusionQueries(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    const vk::QueryPoolCreateInfo pool_info = {
        .queryType = vk::QueryType::eOcclusion,
        .queryCount = NumQueries,
    };
    auto [result, query_pool] = instance.GetDevice().createQueryPoolUnique(pool_info);
    ASSERT_MSG(result == vk::Result::eSuccess, "Failed to create occlusion query pool: {}",
               vk::to_string(result));
    pool = std::move(query_pool);
    if (instance.IsOcclusionQueryPreciseSupported()) {
        control_flags = vk::QueryControlFlagBits::ePrecise;
    }
    scheduler.SetRenderingObserver(this);
}

OcclusionQueries::~OcclusionQueries() {
    scheduler.SetRenderingObserver(nullptr);
}

void OcclusionQueries::OnBeginRendering() {
    if (!open.empty() && !active) {
        BeginSegment();
    }
}

void OcclusionQueries::OnEndRendering() {
    if (active) {
        EndSegment();
    }
}

bool OcclusionQueries::Begin(VAddr address, u32 num_pairs) {
    // A query begun again at the same address starts over.
    for (auto it = open.begin(); it != open.end(); ++it) {
        if (it->address == address) {
            open.erase(it);
            break;
        }
    }
    if (open.size() >= MaxOpenQueries) {
        return false;
    }
    // A new guest query must not begin inside a dynamic-rendering scope
    // whose primary permits only vkCmdExecuteCommands. End that scope first;
    // OnBeginRendering of the next draw will start a normal primary query.
    // This preserves query samples rather than issuing vkCmdBeginQuery in an
    // illegal secondary without inheritedQueries support.
    if (open.empty() && scheduler.IsNativeSecondaryRendering()) {
        scheduler.EndRendering();
    }
    Cut();
    open.push_back(OpenQuery{
        .address = address,
        .max_segments = std::clamp<u32>(num_pairs, 1, MaxCounterPairs),
        .first_segment = static_cast<u32>(segments.size()),
        .submit = submit,
        .incomplete = false,
    });
    Resume();
    return true;
}

bool OcclusionQueries::End(VAddr begin_address, QueryList& out_queries) {
    const auto it = std::find_if(open.begin(), open.end(), [begin_address](const OpenQuery& q) {
        return q.address == begin_address;
    });
    if (it == open.end()) {
        return false;
    }
    Cut();
    const OpenQuery query = *it;
    open.erase(it);
    const u32 num_segments = static_cast<u32>(segments.size()) - query.first_segment;
    const bool counted = !query.incomplete && num_segments <= query.max_segments;
    if (counted) {
        out_queries.assign(segments.begin() + query.first_segment, segments.end());
    }
    DropOldSegments();
    return counted;
}

void OcclusionQueries::DropOldSegments() {
    u32 oldest = static_cast<u32>(segments.size());
    for (const OpenQuery& q : open) {
        oldest = std::min(oldest, q.first_segment);
    }
    if (oldest == 0) {
        return;
    }
    if (active && oldest == segments.size()) {
        // The active segment counts for no open query.
        --oldest;
    }
    segments.erase(segments.begin(), segments.begin() + oldest);
    for (OpenQuery& q : open) {
        q.first_segment -= oldest;
    }
}

void OcclusionQueries::OnSubmit() {
    ++submit;
    const auto old_size = open.size();
    for (auto it = open.begin(); it != open.end();) {
        if (submit - it->submit > MaxOpenSubmits) {
            it = open.erase(it);
        } else {
            ++it;
        }
    }
    if (open.size() == old_size) {
        return;
    }
    if (open.empty()) {
        Cut();
        segments.clear();
        return;
    }
    DropOldSegments();
}

void OcclusionQueries::MarkUsed(std::span<const u32> queries) {
    const u64 tick = scheduler.CurrentTick();
    for (const u32 query : queries) {
        query_ticks[query] = tick;
    }
}

void OcclusionQueries::Cut() {
    if (active) {
        EndSegment();
    }
}

void OcclusionQueries::Resume() {
    if (!open.empty() && !active && scheduler.IsRendering()) {
        BeginSegment();
    }
}

void OcclusionQueries::BeginSegment() {
    const auto not_counted = [this] {
        // The samples of the open queries until the next segment are lost.
        for (OpenQuery& q : open) {
            q.incomplete = true;
        }
        ++dropped;
    };
    if (segments.size() >= MaxSegments || !instance.IsHostQueryResetSupported()) {
        not_counted();
        return;
    }
    const u32 query = next_query;
    const u64 last_use = query_ticks[query];
    if (last_use != 0 && !scheduler.IsFree(last_use)) {
        // Its last use (thousands of queries ago) is still executing.
        not_counted();
        return;
    }
    next_query = (next_query + 1) % NumQueries;
    // Reset from the host, as this is inside a rendering scope; the command buffer that begins
    // it is submitted after this.
    instance.GetDevice().resetQueryPool(*pool, query, 1);
    query_ticks[query] = scheduler.CurrentTick();
    scheduler.Record([pool = *pool, query, flags = control_flags](vk::CommandBuffer cmdbuf) {
        cmdbuf.beginQuery(pool, query, flags);
    });
    segments.push_back(query);
    active = true;
}

void OcclusionQueries::EndSegment() {
    scheduler.Record([pool = *pool, query = segments.back()](vk::CommandBuffer cmdbuf) {
        cmdbuf.endQuery(pool, query);
    });
    active = false;
}

} // namespace Vulkan
