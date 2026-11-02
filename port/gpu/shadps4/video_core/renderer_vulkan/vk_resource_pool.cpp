// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstddef>
#include <optional>

#include "common/assert.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_semaphore.h"

namespace Vulkan {

ResourcePool::ResourcePool(Semaphore* work_semaphore_, std::size_t grow_step_)
    : work_semaphore{work_semaphore_}, grow_step{grow_step_} {}

std::size_t ResourcePool::CommitResource() {
    u64 gpu_tick = work_semaphore->KnownGpuTick();
    // The refreshed GPU tick must be observed by both searches. Capturing it by value
    // kept searching with the pre-refresh tick and grew pools whose resources were already free.
    const auto search = [this, &gpu_tick](std::size_t begin,
                                         std::size_t end) -> std::optional<std::size_t> {
        for (std::size_t iterator = begin; iterator < end; ++iterator) {
            if (gpu_tick >= ticks[iterator]) {
                ticks[iterator] = work_semaphore->CurrentTick();
                return iterator;
            }
        }
        return std::nullopt;
    };
    const auto search_all = [&] {
        auto found = search(hint_iterator, ticks.size());
        return found ? found : search(0, hint_iterator);
    };

    // Search both sides of the hint with the known tick before querying the driver.
    auto found = search_all();
    if (!found && !ticks.empty()) {
        work_semaphore->Refresh();
        gpu_tick = work_semaphore->KnownGpuTick();
        found = search_all();
    }
    if (!found) {
        const std::size_t free_resource = ManageOverflow();
        ticks[free_resource] = work_semaphore->CurrentTick();
        found = free_resource;
    }

    // Free iterator is hinted to the resource after the one that's been commited.
    hint_iterator = (*found + 1) % ticks.size();
    return *found;
}

std::size_t ResourcePool::ManageOverflow() {
    const std::size_t old_capacity = ticks.size();
    ticks.resize(old_capacity + grow_step);
    Allocate(old_capacity, old_capacity + grow_step);
    return old_capacity;
}

#ifdef ISS_ANDROID_BIONIC
// Eight submissions plus the command buffer being recorded must fit without a pool-miss
// timeline query halfway through the batch. Grow in the same units if the GPU falls behind.
constexpr std::size_t COMMAND_BUFFER_POOL_SIZE = 16;
#else
constexpr std::size_t COMMAND_BUFFER_POOL_SIZE = 4;
#endif

CommandPool::CommandPool(const Instance& instance, Semaphore* work_semaphore,
                         vk::CommandBufferLevel level_)
    : ResourcePool{work_semaphore, COMMAND_BUFFER_POOL_SIZE}, instance{instance}, level{level_} {
    const vk::CommandPoolCreateInfo pool_create_info = {
        .flags = vk::CommandPoolCreateFlagBits::eTransient |
                 vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
        .queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex(),
    };
    const vk::Device device = instance.GetDevice();
    auto [pool_result, pool] = device.createCommandPoolUnique(pool_create_info);
    ASSERT_MSG(pool_result == vk::Result::eSuccess, "Failed to create command pool: {}",
               vk::to_string(pool_result));
    cmd_pool = std::move(pool);
    SetObjectName(device, *cmd_pool, "CommandPool");
}

CommandPool::~CommandPool() = default;

void CommandPool::Allocate(std::size_t begin, std::size_t end) {
    cmd_buffers.resize(end);

    const vk::CommandBufferAllocateInfo buffer_alloc_info = {
        .commandPool = *cmd_pool,
        .level = level,
        .commandBufferCount = COMMAND_BUFFER_POOL_SIZE,
    };

    const vk::Device device = instance.GetDevice();
    const auto result =
        device.allocateCommandBuffers(&buffer_alloc_info, cmd_buffers.data() + begin);
    ASSERT(result == vk::Result::eSuccess);

    for (std::size_t i = begin; i < end; ++i) {
        SetObjectName(device, cmd_buffers[i], "CommandPool: Command Buffer {}", i);
    }
}

vk::CommandBuffer CommandPool::Commit() {
    const std::size_t index = CommitResource();
    return cmd_buffers[index];
}

DescriptorHeap::DescriptorHeap(const Instance& instance, Semaphore* work_semaphore_,
                               std::span<const vk::DescriptorPoolSize> pool_sizes_,
                               u32 descriptor_heap_count_)
    : device{instance.GetDevice()}, work_semaphore{work_semaphore_},
      descriptor_heap_count{descriptor_heap_count_}, pool_sizes{pool_sizes_} {
    CreateDescriptorPool();
}

DescriptorHeap::~DescriptorHeap() {
    device.destroyDescriptorPool(curr_pool);
    for (const auto [pool, tick] : pending_pools) {
        work_semaphore->Wait(tick);
        device.destroyDescriptorPool(pool);
    }
}

vk::DescriptorSet DescriptorHeap::Commit(vk::DescriptorSetLayout set_layout) {
    const u64 set_key = std::bit_cast<u64>(set_layout);
    const auto [it, _] = descriptor_sets.try_emplace(set_key);

    // Check if allocated sets exist and pick one.
    if (!it->second.empty()) {
        const auto desc_set = it->second.back();
        it.value().pop_back();
        return desc_set;
    }

    DescSetBatch desc_sets(DescriptorSetBatch);
    std::array<vk::DescriptorSetLayout, DescriptorSetBatch> layouts;
    layouts.fill(set_layout);

    vk::DescriptorSetAllocateInfo alloc_info = {
        .descriptorPool = curr_pool,
        .descriptorSetCount = DescriptorSetBatch,
        .pSetLayouts = layouts.data(),
    };

    // Attempt to allocate the descriptor set batch.
    auto result = device.allocateDescriptorSets(&alloc_info, desc_sets.data());
    if (result == vk::Result::eSuccess) {
        const auto desc_set = desc_sets.back();
        desc_sets.pop_back();
        it.value() = std::move(desc_sets);
        return desc_set;
    }

    // The pool has run out. Record current tick and place it in pending list.
    ASSERT_MSG(result == vk::Result::eErrorOutOfPoolMemory ||
                   result == vk::Result::eErrorFragmentedPool,
               "Unexpected error during descriptor set allocation: {}", vk::to_string(result));
    pending_pools.emplace_back(curr_pool, work_semaphore->CurrentTick());
    // Relaxed readbacks perform fewer timeline refreshes. Refresh only on pool exhaustion,
    // before deciding that a completed descriptor pool must be replaced by a new allocation.
    if (!work_semaphore->IsFree(pending_pools.front().second)) {
        work_semaphore->Refresh();
    }
    if (const auto [pool, tick] = pending_pools.front(); work_semaphore->IsFree(tick)) {
        curr_pool = pool;
        pending_pools.pop_front();

        const auto reset_result = device.resetDescriptorPool(curr_pool);
        ASSERT_MSG(reset_result == vk::Result::eSuccess,
                   "Unexpected error resetting descriptor pool: {}", vk::to_string(reset_result));
    } else {
        CreateDescriptorPool();
    }

    // Attempt to allocate again with fresh pool.
    alloc_info.descriptorPool = curr_pool;
    result = device.allocateDescriptorSets(&alloc_info, desc_sets.data());
    ASSERT_MSG(result == vk::Result::eSuccess,
               "Unexpected error during descriptor set allocation {}", vk::to_string(result));

    // We've changed pool so also reset descriptor batch cache.
    descriptor_sets.clear();
    const auto desc_set = desc_sets.back();
    desc_sets.pop_back();
    descriptor_sets[set_key] = std::move(desc_sets);
    return desc_set;
}

void DescriptorHeap::CreateDescriptorPool() {
    const vk::DescriptorPoolCreateInfo pool_info = {
        .flags = vk::DescriptorPoolCreateFlagBits::eUpdateAfterBind,
        .maxSets = descriptor_heap_count,
        .poolSizeCount = static_cast<u32>(pool_sizes.size()),
        .pPoolSizes = pool_sizes.data(),
    };
    auto [pool_result, pool] = device.createDescriptorPool(pool_info);
    ASSERT_MSG(pool_result == vk::Result::eSuccess, "Failed to create descriptor pool: {}",
               vk::to_string(pool_result));
    curr_pool = pool;
}

} // namespace Vulkan
