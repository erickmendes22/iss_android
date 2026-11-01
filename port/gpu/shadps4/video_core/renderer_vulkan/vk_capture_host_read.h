// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <vulkan/vulkan.hpp>

namespace Vulkan {
// Completion waits and cache invalidation do not perform the required
// device-to-host memory-domain operation for these transfer-written bytes.
inline void CaptureHostReadBarrier(vk::CommandBuffer cmd, vk::Buffer buffer, vk::DeviceSize size) {
    const vk::BufferMemoryBarrier2 ready{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer,
        .offset = 0,
        .size = size,
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{
        .bufferMemoryBarrierCount = 1,
        .pBufferMemoryBarriers = &ready,
    });
}
}
