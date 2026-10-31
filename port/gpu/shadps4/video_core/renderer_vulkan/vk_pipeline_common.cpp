// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <array>
#include <cstdlib>
#include <span>
#include <thread>

#include <boost/container/small_vector.hpp>
#include <boost/container/static_vector.hpp>

#include "common/logging/log.h"
#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_descriptor_replay_memo.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "common/path_util.h"
#include "video_core/renderer_vulkan/vk_wave_stats.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>

namespace Vulkan {

namespace {
using ReplayCache = DescriptorReplayMemo<vk::CommandBuffer, vk::PipelineLayout>;
ReplayCache& ReplayMemo() {
    // Scheduler's recording workers each own their command buffer state.
    static thread_local ReplayCache memo;
    return memo;
}

bool UseDescriptorReplay() {
#if defined(__ANDROID__) && defined(ISS_ANDROID_BIONIC)
    static const bool enabled = [] {
        const char* value = std::getenv("TLG_DESCRIPTOR_REPLAY");
        return !value || value[0] != '0';
    }();
    return enabled;
#else
    return false;
#endif
}

bool UseDescriptorDelta() {
#if defined(__ANDROID__) && defined(ISS_ANDROID_BIONIC)
    static const bool enabled = [] {
        const char* value = std::getenv("TLG_DESCRIPTOR_DELTA");
        return value && value[0] == '1';
    }();
    return enabled;
#else
    return false;
#endif
}
} // namespace

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      is_compute{is_compute_} {}

Pipeline::~Pipeline() = default;

bool RetryPipelineCreation(vk::Result result, u32 attempt, std::string_view kind,
                           std::string_view name) {
    // With the driver's cache cold, as after a driver update, the compiler threads build hundreds
    // of pipelines at once while the game loads, and the driver failed one of them now and then
    // with an unknown error, which ended the game. Building it again a little later can succeed.
    constexpr u32 MaxAttempts = 5;
    const bool may_recover = result == vk::Result::eErrorUnknown ||
                             result == vk::Result::eErrorOutOfHostMemory ||
                             result == vk::Result::eErrorOutOfDeviceMemory ||
                             result == vk::Result::eErrorInitializationFailed;
    if (!may_recover || attempt >= MaxAttempts) {
        return false;
    }
    LOG_WARNING(Render_Vulkan, "tlg: creating {} pipeline {} failed with {} (attempt {}), trying "
                "again", kind, name, vk::to_string(result), attempt);
    std::this_thread::sleep_for(std::chrono::milliseconds{100} * attempt);
    return true;
}

vk::Pipeline Pipeline::BuiltHandle() const {
    if (const VkPipeline handle = build->handle.load(std::memory_order_acquire)) {
        return vk::Pipeline{handle};
    }
    const auto start = std::chrono::steady_clock::now();
    std::unique_lock lock{build->mutex};
    build->cv.wait(lock, [this] {
        return build->handle.load(std::memory_order_acquire) != VK_NULL_HANDLE;
    });
    LOG_INFO(Render_Vulkan, "tlg: waited {:.1f} ms for compute pipeline {} being built",
             std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                 .count(),
             GetDebugString());
    return vk::Pipeline{build->handle.load(std::memory_order_acquire)};
}

void Pipeline::BuildDescriptorTemplate(std::span<const vk::DescriptorSetLayoutBinding> bindings) {
    static const bool enabled = [] {
#if defined(__ANDROID__) && defined(ISS_ANDROID_BIONIC)
        const char* env = std::getenv("TLG_DESCRIPTOR_TEMPLATES");
        return !(env && *env == '0');
#else
        return false;
#endif
    }();
    const auto push = VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPushDescriptorSetWithTemplateKHR;
    const bool available = push && VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateDescriptorUpdateTemplate &&
                           VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyDescriptorUpdateTemplate;
    static std::once_flag reported;
    std::call_once(reported, [&] {
        std::printf("GPU: descriptor templates policy=%s available=%d\n",
                    enabled ? "on" : "off", available ? 1 : 0);
    });
    if (!enabled || !available || !uses_push_descriptors || bindings.empty()) {
        return;
    }
    DescriptorTemplateData data;
    std::vector<vk::DescriptorUpdateTemplateEntry> entries;
    entries.reserve(bindings.size());
    for (const auto& binding : bindings) {
        DescriptorPayload payload;
        size_t stride, alignment;
        switch (binding.descriptorType) {
        case vk::DescriptorType::eUniformBuffer:
        case vk::DescriptorType::eStorageBuffer:
            payload = DescriptorPayload::Buffer;
            stride = sizeof(vk::DescriptorBufferInfo);
            alignment = alignof(vk::DescriptorBufferInfo);
            break;
        case vk::DescriptorType::eSampler:
        case vk::DescriptorType::eCombinedImageSampler:
        case vk::DescriptorType::eSampledImage:
        case vk::DescriptorType::eStorageImage:
        case vk::DescriptorType::eInputAttachment:
            payload = DescriptorPayload::Image;
            stride = sizeof(vk::DescriptorImageInfo);
            alignment = alignof(vk::DescriptorImageInfo);
            break;
        case vk::DescriptorType::eUniformTexelBuffer:
        case vk::DescriptorType::eStorageTexelBuffer:
            payload = DescriptorPayload::Texel;
            stride = sizeof(vk::BufferView);
            alignment = alignof(vk::BufferView);
            break;
        default:
            return; // unsupported descriptors retain the existing update path
        }
        if (!data.Add(binding.binding, binding.descriptorCount, u32(binding.descriptorType),
                      payload, stride, alignment)) {
            return;
        }
        entries.push_back({
            .dstBinding = binding.binding,
            .dstArrayElement = 0,
            .descriptorCount = binding.descriptorCount,
            .descriptorType = binding.descriptorType,
            .offset = data.slots.back().offset,
            .stride = stride,
        });
    }
    const vk::DescriptorUpdateTemplateCreateInfo info = {
        .descriptorUpdateEntryCount = u32(entries.size()),
        .pDescriptorUpdateEntries = entries.data(),
        .templateType = vk::DescriptorUpdateTemplateType::ePushDescriptorsKHR,
        .pipelineBindPoint = IsCompute() ? vk::PipelineBindPoint::eCompute
                                        : vk::PipelineBindPoint::eGraphics,
        .pipelineLayout = *pipeline_layout,
        .set = 0,
    };
    auto [result, update] = instance.GetDevice().createDescriptorUpdateTemplateUnique(info);
    if (result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "Descriptor template unavailable ({}): keeping descriptor writes",
                    vk::to_string(result));
        return;
    }
    descriptor_template_data = std::move(data);
    descriptor_update_template = std::move(update);
    push_descriptor_template = push;
}


void Pipeline::BindResources(DescriptorWrites& set_writes, u32 count,
                             const Shader::PushData& push_data) const {
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;
    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    const vk::PipelineLayout layout = *pipeline_layout;
    // Capture these before enqueuing the Vulkan callback. A recycled command buffer handle
    // is NOT evidence of the same recording when the submission tick/epoch differs.
    const u64 descriptor_tick = scheduler.CurrentTick();
    const u64 descriptor_epoch = scheduler.DescriptorStreamEpoch();
    const unsigned bind_slot = IsCompute() ? 1u : 0u;
    const std::span<const vk::WriteDescriptorSet> active_writes{set_writes.data(), count};
    if (descriptor_update_template && count != 0) {
        // A compact, aligned snapshot replaces WriteDescriptorSet copies and pointer patching.
        boost::container::small_vector<u64, 256> words;
        words.resize((descriptor_template_data.bytes + sizeof(u64) - 1) / sizeof(u64));
        if (descriptor_template_data.Pack(active_writes, std::span<u64>{words.data(), words.size()})) {
            const auto payload = scheduler.RecordData(std::span<const u64>{words.data(), words.size()});
            const VkDescriptorUpdateTemplate update = *descriptor_update_template;
            const auto push = push_descriptor_template;
            const auto* template_data = &descriptor_template_data;
            scheduler.Record([layout, stage_flags, push_data, update, push, payload,
                              descriptor_tick, descriptor_epoch, bind_slot, bind_point,
                              template_data](vk::CommandBuffer cmdbuf) {
                // Resource coherency and image barriers were handled on the CPU thread.
                // This only reduces redundant Vulkan descriptor driver work.
                cmdbuf.pushConstants(layout, stage_flags, 0u, sizeof(push_data), &push_data);
                bool push_full = true;
                bool unchanged = false;
                if (UseDescriptorReplay()) {
                    auto& memo = ReplayMemo();
                    const auto& slots = template_data->slots;
                    if (UseDescriptorDelta() && slots.size() <= 128) {
                        std::array<ReplayCache::ByteRange, 128> ranges{};
                        std::array<std::uint8_t, 128> selected{};
                        for (std::size_t i = 0; i < slots.size(); ++i) {
                            ranges[i] = {slots[i].offset, slots[i].count * slots[i].stride};
                        }
                        const auto decision = memo.Select(
                            cmdbuf, layout, descriptor_tick, descriptor_epoch, bind_slot, payload,
                            std::span<const ReplayCache::ByteRange>{ranges.data(), slots.size()},
                            std::span<std::uint8_t>{selected.data(), slots.size()});
                        if (decision.mode == ReplayCache::Mode::Unchanged) {
                            push_full = false;
                            unchanged = true;
                        } else if (decision.mode == ReplayCache::Mode::Partial) {
                            // vkCmdPushDescriptorSetKHR updates only the listed bindings,
                            // preserving the others in this same push-descriptor layout.
                            std::array<vk::WriteDescriptorSet, 16> changes{};
                            const auto* bytes =
                                reinterpret_cast<const std::byte*>(payload.data());
                            for (std::size_t j = 0; j < decision.changed; ++j) {
                                const auto& slot = slots[selected[j]];
                                auto& write = changes[j];
                                write.dstSet = VK_NULL_HANDLE;
                                write.dstBinding = slot.binding;
                                write.dstArrayElement = 0;
                                write.descriptorCount = slot.count;
                                write.descriptorType = static_cast<vk::DescriptorType>(slot.type);
                                const auto* value = bytes + slot.offset;
                                switch (slot.payload) {
                                case DescriptorPayload::Buffer:
                                    write.pBufferInfo =
                                        reinterpret_cast<const vk::DescriptorBufferInfo*>(value);
                                    break;
                                case DescriptorPayload::Image:
                                    write.pImageInfo =
                                        reinterpret_cast<const vk::DescriptorImageInfo*>(value);
                                    break;
                                case DescriptorPayload::Texel:
                                    write.pTexelBufferView =
                                        reinterpret_cast<const vk::BufferView*>(value);
                                    break;
                                }
                            }
                            cmdbuf.pushDescriptorSetKHR(
                                bind_point, layout, 0,
                                std::span<const vk::WriteDescriptorSet>{changes.data(),
                                                                         decision.changed});
                            push_full = false;
                            if (IssBionicPerf::Enabled()) {
                                IssBionicPerf::descriptor_delta_partial.fetch_add(
                                    1, std::memory_order_relaxed);
                                IssBionicPerf::descriptor_delta_bindings.fetch_add(
                                    decision.changed, std::memory_order_relaxed);
                            }
                        }
                    } else {
                        unchanged = !memo.NeedsPush(
                            cmdbuf, layout, descriptor_tick, descriptor_epoch, bind_slot, payload);
                        push_full = !unchanged;
                    }
                }
                if (push_full) {
                    push(cmdbuf, update, layout, 0, payload.data());
                } else if (unchanged && IssBionicPerf::Enabled()) {
                    IssBionicPerf::descriptor_replay_skipped.fetch_add(1,
                                                                      std::memory_order_relaxed);
                }
            });
            if (IssBionicPerf::Enabled()) {
                IssBionicPerf::descriptor_template_calls.fetch_add(1, std::memory_order_relaxed);
                IssBionicPerf::descriptor_template_bytes.fetch_add(descriptor_template_data.bytes,
                                                                  std::memory_order_relaxed);
            }
            return;
        }
    }
    if (IssBionicPerf::Enabled() && count != 0) {
        IssBionicPerf::descriptor_legacy_calls.fetch_add(1, std::memory_order_relaxed);
    }
    scheduler.Record([layout, stage_flags, push_data, bind_slot](vk::CommandBuffer cmdbuf) {
        // A non-template path can modify descriptor state; prevent false template hits.
        ReplayMemo().Invalidate(bind_slot);
        cmdbuf.pushConstants(layout, stage_flags, 0u, sizeof(push_data), &push_data);
    });

    // Bind descriptor set.
    if (count == 0) {
        return;
    }

    if (uses_push_descriptors) {
        if (!scheduler.IsRecordingDeferred()) {
            scheduler.Record([bind_point, layout, active_writes](vk::CommandBuffer cmdbuf) {
                cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, active_writes);
            });
            return;
        }
        // Writes and the infos they point to are laid out in the recording chunk, with the
        // pointers already aimed at those copies: the command only carries a span.
        size_t bytes = count * sizeof(vk::WriteDescriptorSet) + 64;
        for (const auto& write : active_writes) {
            bytes += write.descriptorCount *
                         (sizeof(vk::DescriptorBufferInfo) + sizeof(vk::DescriptorImageInfo) +
                          sizeof(vk::BufferView)) +
                     32;
        }
        scheduler.ReserveRecordData(bytes);
        const auto writes = scheduler.RecordData(active_writes);
        auto* patched = const_cast<vk::WriteDescriptorSet*>(writes.data());
        for (size_t i = 0; i < writes.size(); ++i) {
            auto& write = patched[i];
            if (write.pBufferInfo) {
                write.pBufferInfo = scheduler.RecordData(std::span{write.pBufferInfo, write.descriptorCount}).data();
            }
            if (write.pImageInfo) {
                write.pImageInfo = scheduler.RecordData(std::span{write.pImageInfo, write.descriptorCount}).data();
            }
            if (write.pTexelBufferView) {
                write.pTexelBufferView =
                    scheduler.RecordData(std::span{write.pTexelBufferView, write.descriptorCount}).data();
            }
        }
        scheduler.Record([bind_point, layout, writes](vk::CommandBuffer cmdbuf) {
            cmdbuf.pushDescriptorSetKHR(bind_point, layout, 0, writes);
        });
        return;
    }

    // Descriptor set updates are device calls: they stay on this thread.
    const auto desc_set = desc_heap.Commit(*desc_layout);
    for (auto& set_write : std::span{set_writes.data(), count}) {
        set_write.dstSet = desc_set;
    }
    instance.GetDevice().updateDescriptorSets(active_writes, {});
    scheduler.Record([bind_point, layout, desc_set](vk::CommandBuffer cmdbuf) {
        cmdbuf.bindDescriptorSets(bind_point, layout, 0, desc_set, {});
    });
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->hw_stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

// tlg: one histogram for every pipeline of the process, reported to last_run.log on news and
// every 256 pipelines (vk_wave_stats.h). AMD reports width 0 for graphics executables, so on
// AMD the key also carries the physical VGPR count from VK_AMD_shader_info (it differs between
// wave32 and wave64), and the ISA of the first executables of each stage goes to
// <shaders>/wave_dumps/ (wave32 code manipulates EXEC with the _b32 forms).
void Pipeline::RecordWaveSizes() const {
    if (!instance.IsPipelineExecutableInfoEnabled() || !pipeline) {
        return;
    }
    const auto [result, executables] = instance.GetDevice().getPipelineExecutablePropertiesKHR(
        vk::PipelineInfoKHR{.pipeline = *pipeline});
    if (result != vk::Result::eSuccess) {
        return;
    }
    static std::mutex mutex;
    static WaveSizeHistogram histogram;
    std::scoped_lock lock{mutex};
    bool news = false;
    for (const vk::PipelineExecutablePropertiesKHR& executable : executables) {
        const std::string stages = vk::to_string(executable.stages);
        std::string key = stages;
        // One stage bit per query: the last stage of the executable (vk_wave_stats.h).
        const std::uint32_t stage_bit = AmdShaderInfoStageBit(
            is_compute ? static_cast<std::uint32_t>(vk::ShaderStageFlagBits::eCompute)
                       : static_cast<std::uint32_t>(executable.stages));
        if (instance.IsAmdShaderInfoEnabled() && stage_bit != 0) {
            const auto stage = static_cast<vk::ShaderStageFlagBits>(stage_bit);
            const auto [stats_result, stats] = instance.GetDevice().getShaderInfoAMD(
                *pipeline, stage, vk::ShaderInfoTypeAMD::eStatistics);
            std::string stats_line = "stats unavailable";
            std::optional<std::uint32_t> vgprs;
            if (stats_result == vk::Result::eSuccess &&
                stats.size() >= sizeof(vk::ShaderStatisticsInfoAMD)) {
                vk::ShaderStatisticsInfoAMD info{};
                std::memcpy(&info, stats.data(), sizeof(info));
                vgprs = info.numPhysicalVgprs;
                key += "/vgpr" + std::to_string(info.numPhysicalVgprs);
                stats_line = "physical vgprs " + std::to_string(info.numPhysicalVgprs) +
                             ", available " + std::to_string(info.numAvailableVgprs) +
                             ", used " + std::to_string(info.resourceUsage.numUsedVgprs) +
                             "; physical sgprs " + std::to_string(info.numPhysicalSgprs) +
                             ", used " + std::to_string(info.resourceUsage.numUsedSgprs) +
                             "; lds " + std::to_string(info.resourceUsage.ldsUsageSizeInBytes) +
                             "; workgroup " + std::to_string(info.computeWorkGroupSize[0]) +
                             "x" + std::to_string(info.computeWorkGroupSize[1]) + "x" +
                             std::to_string(info.computeWorkGroupSize[2]);
            }
            if (histogram.ShouldDump(key, 3)) {
                const auto [isa_result, isa] = instance.GetDevice().getShaderInfoAMD(
                    *pipeline, stage, vk::ShaderInfoTypeAMD::eDisassembly);
                if (isa_result == vk::Result::eSuccess && !isa.empty()) {
                    const auto dir =
                        Common::FS::GetUserPath(Common::FS::PathType::ShaderDir) / "wave_dumps";
                    std::error_code ec;
                    std::filesystem::create_directories(dir, ec);
                    const std::string name = WaveDumpFileName(
                        stages, vgprs, histogram.DumpsOf(key), executable.subgroupSize);
                    std::ofstream out(dir / name, std::ios::binary);
                    out << "key " << key << "\n" << stats_line << "\n";
                    out.write(reinterpret_cast<const char*>(isa.data()),
                              static_cast<std::streamsize>(isa.size()));
                }
            }
        }
        news |= histogram.Add(std::move(key), executable.subgroupSize);
    }
    if (histogram.EndPipeline(news)) {
        std::printf("GPU: wave sizes after %u pipelines: %s\n", histogram.Pipelines(),
                    histogram.Summary().c_str());
    }
}

} // namespace Vulkan
