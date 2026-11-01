// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// Conservative fast draw pipeline state: vkCmdBindPipeline is redundant for
// consecutive draws on the SAME graphics command buffer when no other graphics
// pipeline command has run. The renderer invalidates the epoch for raw commands,
// host depth-resampling and when a render scope ends (secondary execution).
// This NEVER skips guest resource binding, barriers or the Vulkan draw itself.
#include <cstdint>

namespace Vulkan {
template <typename CommandBuffer, typename Pipeline>
class FastGraphicsPipelineMemo {
public:
    bool NeedsBind(CommandBuffer cmdbuf, Pipeline pipeline, std::uint64_t tick,
                   std::uint64_t stream_epoch) noexcept {
        const bool hit = valid && command == cmdbuf && bound == pipeline &&
                         last_tick == tick && last_epoch == stream_epoch;
        command = cmdbuf;
        bound = pipeline;
        last_tick = tick;
        last_epoch = stream_epoch;
        valid = true;
        return !hit;
    }

    void Invalidate() noexcept { valid = false; }

private:
    CommandBuffer command{};
    Pipeline bound{};
    std::uint64_t last_tick{}, last_epoch{};
    bool valid = false;
};
} // namespace Vulkan
