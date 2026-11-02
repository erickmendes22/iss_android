// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <span>
#include <vector>
#include "video_core/renderer_vulkan/vk_semaphore.h"
namespace Vulkan {
class Instance;
// Same graphics family, different VkQueue: exclusive buffers need no ownership transfer.
// Cache owner serializes submissions; only timeline waits run on guest threads.
class ReadbackQueue {
public:
    explicit ReadbackQueue(const Instance& instance);
    ~ReadbackQueue();
    u64 Submit(vk::Buffer source, vk::Buffer destination, std::span<const vk::BufferCopy> copies,
               vk::Semaphore producer, u64 producer_tick, vk::Semaphore residency, u64 residency_tick);
    Semaphore& Timeline() { return timeline; }
    u64 LastTick() const { return last_tick; }
private:
    const Instance& instance;
    Semaphore timeline;
    vk::UniqueCommandPool pool;
    struct Slot { vk::CommandBuffer command; u64 tick{}; };
    std::vector<Slot> slots;
    u64 last_tick{};
};
} // namespace Vulkan
