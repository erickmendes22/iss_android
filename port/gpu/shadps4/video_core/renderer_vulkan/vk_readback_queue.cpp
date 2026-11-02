// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/renderer_vulkan/vk_readback_queue.h"
#include <array>
#include "common/assert.h"
#include "video_core/renderer_vulkan/vk_instance.h"
namespace Vulkan {
ReadbackQueue::ReadbackQueue(const Instance& instance_) : instance{instance_}, timeline{instance_} {
    pool = Check(instance.GetDevice().createCommandPoolUnique(vk::CommandPoolCreateInfo{
        .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        .queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex(),
    }));
}
ReadbackQueue::~ReadbackQueue() {
    // Arenas/staging outlive this explicit drain in BufferCache's destructor.
    if (last_tick) timeline.Wait(last_tick);
}
u64 ReadbackQueue::Submit(vk::Buffer source, vk::Buffer destination,
                         std::span<const vk::BufferCopy> copies,
                         vk::Semaphore producer, u64 producer_tick,
                         vk::Semaphore residency, u64 residency_tick) {
    timeline.Refresh();
    Slot* slot = nullptr;
    for (auto& item : slots) if (timeline.IsFree(item.tick)) { slot = &item; break; }
    // Do not stall the cache owner when every private command is in flight.
    // The caller retains the existing graphics copy/Flush fallback instead.
    if (!slot && slots.size() == 64) return 0;
    if (!slot) {
        const auto commands = Check(instance.GetDevice().allocateCommandBuffers(
            vk::CommandBufferAllocateInfo{.commandPool = *pool,
                .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1}));
        slots.push_back({commands.front(), 0});
        slot = &slots.back();
    } else {
        Check(slot->command.reset());
    }
    const auto command = slot->command;
    Check(command.begin(vk::CommandBufferBeginInfo{.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}));
    command.copyBuffer(source, destination, copies);
    const vk::MemoryBarrier2 host{
        .srcStageMask = vk::PipelineStageFlagBits2::eCopy,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    command.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &host});
    Check(command.end());
    std::array<vk::SemaphoreSubmitInfo, 2> waits{};
    u32 count = 0;
    if (producer_tick) waits[count++] = vk::SemaphoreSubmitInfo{
        .semaphore = producer, .value = producer_tick, .stageMask = vk::PipelineStageFlagBits2::eCopy};
    if (residency_tick) waits[count++] = vk::SemaphoreSubmitInfo{
        .semaphore = residency, .value = residency_tick, .stageMask = vk::PipelineStageFlagBits2::eCopy};
    const u64 tick = timeline.NextTick();
    const vk::SemaphoreSubmitInfo signal{
        .semaphore = timeline.Handle(), .value = tick, .stageMask = vk::PipelineStageFlagBits2::eAllCommands};
    const vk::CommandBufferSubmitInfo buffer{.commandBuffer = command};
    const vk::SubmitInfo2 submit{
        .waitSemaphoreInfoCount = count, .pWaitSemaphoreInfos = waits.data(),
        .commandBufferInfoCount = 1, .pCommandBufferInfos = &buffer,
        .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &signal};
    Check(instance.GetReadbackQueue().submit2(submit));
    slot->tick = last_tick = tick;
    return tick;
}
} // namespace Vulkan
