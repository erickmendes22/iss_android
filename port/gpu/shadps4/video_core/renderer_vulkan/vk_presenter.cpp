#ifdef __ANDROID__
#include "iss_lsfg.h"
#endif
// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/debug.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "sdl_window.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_ahb_acquire_wait.h"
#include "tlg_overlay.h"
#include "iss_bionic_perf.h"
#include "tlg_settings.h"
#include "tlg_graphics.h"
#include "video_core/renderer_vulkan/vk_temporal_upscaler.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_warmup_inbox.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/texture_cache/image.h"

#include <cstdlib>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <system_error>
#include <vector>
#include <vk_mem_alloc.h>
#ifdef MemoryBarrier
#undef MemoryBarrier // tlg: winnt.h macro (through fmt), clashes with vk::MemoryBarrier
#endif

namespace Vulkan {

bool CanBlitToSwapchain(const vk::PhysicalDevice physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 dst_width,
                                          s32 dst_height, s32 offset_x, s32 offset_y) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = offset_x,
                    .y = offset_y,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = offset_x + dst_width,
                    .y = offset_y + dst_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlitStretch(s32 frame_width, s32 frame_height,
                                                 s32 swapchain_width, s32 swapchain_height) {
    return MakeImageBlit(frame_width, frame_height, swapchain_width, swapchain_height, 0, 0);
}

static vk::Rect2D FitImage(s32 frame_width, s32 frame_height, s32 swapchain_width,
                           s32 swapchain_height) {
    float frame_aspect = static_cast<float>(frame_width) / frame_height;
    float swapchain_aspect = static_cast<float>(swapchain_width) / swapchain_height;

    u32 dst_width = swapchain_width;
    u32 dst_height = swapchain_height;

    if (frame_aspect > swapchain_aspect) {
        dst_height = static_cast<s32>(swapchain_width / frame_aspect);
    } else {
        dst_width = static_cast<s32>(swapchain_height * frame_aspect);
    }

    const s32 offset_x = (swapchain_width - dst_width) / 2;
    const s32 offset_y = (swapchain_height - dst_height) / 2;

    return vk::Rect2D{{offset_x, offset_y}, {dst_width, dst_height}};
}

[[nodiscard]] vk::ImageBlit MakeImageBlitFit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                             s32 swapchain_height) {
    const auto& dst_rect = FitImage(frame_width, frame_height, swapchain_width, swapchain_height);

    return MakeImageBlit(frame_width, frame_height, dst_rect.extent.width, dst_rect.extent.height,
                         dst_rect.offset.x, dst_rect.offset.y);
}

static bool SkipPresentClear() {
    static const bool skip = [] {
        const char* env = std::getenv("TLG_PRESENT_SKIP_CLEAR");
        return !(env && env[0] == '0');
    }();
    return skip;
}

// Clear letterbox bars on every acquired image, including recycled AHB images.
static void BlitPresentedFrame(vk::CommandBuffer cmdbuf, vk::Image source, vk::Image destination,
                               s32 width, s32 height, vk::Extent2D extent,
                               const vk::ImageSubresourceRange& color_range) {
    const auto fit = FitImage(width, height, extent.width, extent.height);
    const bool full = fit.offset.x == 0 && fit.offset.y == 0 &&
        fit.extent.width == extent.width && fit.extent.height == extent.height;
    const bool one_to_one = fit.extent.width == u32(width) && fit.extent.height == u32(height);
    const bool optimize = SkipPresentClear();
    if (!(full && optimize)) {
        const vk::ClearColorValue black{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}};
        cmdbuf.clearColorImage(destination, vk::ImageLayout::eTransferDstOptimal, black, color_range);
        const vk::MemoryBarrier clear_done{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, clear_done, {}, {});
    }
    cmdbuf.blitImage(source, vk::ImageLayout::eTransferSrcOptimal, destination,
                     vk::ImageLayout::eTransferDstOptimal,
                     MakeImageBlit(width, height, fit.extent.width, fit.extent.height,
                                   fit.offset.x, fit.offset.y),
                     optimize && one_to_one ? vk::Filter::eNearest : vk::Filter::eLinear);
}


// tlg: screenshot capture and ImGui overlays removed; the port presents directly.

Presenter::Presenter(Frontend::WindowSDL& window_, AmdGpu::Liverpool* liverpool_)
    : window{window_}, liverpool{liverpool_},
      instance{window, EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled(),
               EmulatorSettings.IsVkCrashDiagnosticEnabled()},
      draw_scheduler{instance, true}, present_scheduler{instance}, flip_scheduler{instance},
#ifdef __ANDROID__
      lsfg{Lsfg::Create(instance, lsfg_requested)},
      swapchain{instance, window, lsfg ? lsfg->Multiplier() : 0},
#else
      swapchain{instance, window},
#endif
      runtime{instance, draw_scheduler},
      rasterizer{std::make_unique<Rasterizer>(instance, draw_scheduler, runtime, liverpool)},
      texture_cache{rasterizer->GetTextureCache()} {
#ifdef __ANDROID__
    ApplyLsfgOutputMode();
#endif
    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();

    // Create presentation frames.
    present_frames.resize(num_images);
    for (u32 i = 0; i < num_images; i++) {
        Frame& frame = present_frames[i];
        frame.id = i;
        auto fence = Check<"create present done fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        frame.present_done = fence;
        free_queue.push(&frame);
    }

#ifndef ISS_ANDROID_BIONIC
    fsr_pass.Create(device, instance.GetAllocator(), num_images);
#endif
    pp_pass.Create(device, swapchain.GetSurfaceFormat().format);
    TlgOverlay::Init(instance, swapchain.GetSurfaceFormat().format, num_images);

    // tlg: first-run shader compilation (vk_precompile.h): the job file the launcher wrote from
    // the game files compiles once per cache, behind the loading screen, before the game starts.
    {
        auto last = std::chrono::steady_clock::now();
        const bool compiled = rasterizer->GetPipelineCache().PrecompileFirstStart(
            [&](u32 total) {
                TlgOverlay::SetLoadingCompile(true);
                TlgOverlay::SetLoading(true, 0, total);
                PresentLoadingFrame();
            },
            [&](u32 done, u32 total) {
                const auto now = std::chrono::steady_clock::now();
                if (now - last < std::chrono::milliseconds(250) && done != total) {
                    return;
                }
                last = now;
                TlgOverlay::SetLoading(true, done, total);
                PresentLoadingFrame();
            });
        if (compiled) {
            TlgOverlay::SetLoadingCompile(false);
            TlgOverlay::SetLoading(false, 0, 0);
        }
    }

    const bool binary_cache = EmulatorSettingsImpl::Flag("TLG_PIPELINE_BINARIES", true) &&
                              EmulatorSettings.IsPipelineCacheEnabled();
    const bool capture_only = EmulatorSettingsImpl::Flag("TLG_PIPELINE_BINARY_ISOLATED", false) &&
                              EmulatorSettingsImpl::Flag("TLG_PIPELINE_CAPTURE_ONLY", false);
    if (UseOnDemandPipelines(binary_cache, instance.GetPipelineBinaryGlobalKey().keySize != 0,
                            capture_only)) {
        rasterizer->GetPipelineCache().StartOnDemand();
        return;
    }

    // tlg: by default the cached pipelines load while the game runs (a corner note shows the
    // progress). Otherwise rebuilding them takes up to minutes before the first game frame:
    // show what is happening instead of a black window.
    const char* background_warmup = std::getenv("TLG_BACKGROUND_WARMUP");
    if ((!background_warmup || *background_warmup != '0') &&
        rasterizer->GetPipelineCache().StartBackgroundWarmUp()) {
        return;
    }
    TlgOverlay::SetLoading(true, 0, 0);
    PresentLoadingFrame();
    auto last = std::chrono::steady_clock::now();
    rasterizer->GetPipelineCache().WarmUp([&](u32 done, u32 total) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last < std::chrono::milliseconds(250)) {
            return;
        }
        last = now;
        TlgOverlay::SetLoading(true, done, total);
        PresentLoadingFrame();
    });
    TlgOverlay::SetLoading(false, 0, 0);

}

Presenter::~Presenter() {

    draw_scheduler.Finish();
    present_scheduler.Finish();
#ifdef __ANDROID__
    lsfg.reset();
#endif
    flip_scheduler.Finish();
    Check(draw_scheduler.CommandBuffer().reset());
    Check(present_scheduler.CommandBuffer().reset());
    Check(flip_scheduler.CommandBuffer().reset());

    const vk::Device device = instance.GetDevice();
    for (auto& frame : present_frames) {
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
        device.destroyImageView(frame.image_view);
        device.destroyFence(frame.present_done);
    }
}

bool Presenter::IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const {
    return std::ranges::find(vo_buffers_addr, color_buffer.Address()) != vo_buffers_addr.cend();
}

void Presenter::RecreateFrame(Frame* frame, u32 width, u32 height) {
    const vk::Device device = instance.GetDevice();
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }

    const vk::Format format = swapchain.GetSurfaceFormat().format;
    const vk::ImageCreateInfo image_info = {
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &frame->allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}",
                     vk::to_string(vk::Result{result}));
        UNREACHABLE();
    }
    frame->image = vk::Image{unsafe_image};
    SetObjectName(device, frame->image, "Frame image #{}", frame->id);

    const vk::ImageViewCreateInfo view_info = {
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto view = Check<"create frame image view">(device.createImageView(view_info));
    frame->image_view = view;
    frame->width = width;
    frame->height = height;

    frame->is_hdr = swapchain.GetHDR();
}

Frame* Presenter::PrepareLastFrame() {
    if (last_submit_frame == nullptr) {
        return nullptr;
    }

    Frame* frame = last_submit_frame;

    while (true) {
        vk::Result result = instance.GetDevice().waitForFences(frame->present_done, false,
                                                               std::numeric_limits<u64>::max());
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eTimeout) {
            continue;
        }
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
    }

    auto& scheduler = flip_scheduler;
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier =
        vk::ImageMemoryBarrier2{.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
                                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                .newLayout = vk::ImageLayout::eGeneral,
                                .image = frame->image,
                                .subresourceRange{frame_subresources}};

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

static vk::Format GetFrameViewFormat(const Libraries::VideoOut::PixelFormat format) {
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A2R10G10B10:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2R10G10B10UnormPack32;
    default:
        break;
    }
    UNREACHABLE_MSG("Unknown format={}", static_cast<u32>(format));
    return {};
}

// tlg: raw copy of an image for TLG_FRAME_DUMP_TRIGGER, written once the GPU is done.
static bool DumpRaw(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmdbuf,
                    vk::Image image, vk::ImageLayout layout, u32 w, u32 h, const char* name,
                    int index, u32 offset_x = 0, u32 offset_y = 0, bool trace = false) {
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = VkDeviceSize(w) * h * 4,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_MAPPED_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST};
    VkBuffer buffer{};
    VmaAllocation allocation{};
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(instance.GetAllocator(), &buffer_ci, &alloc_ci, &buffer, &allocation,
                        &info) != VK_SUCCESS) {
        return false;
    }
    cmdbuf.copyImageToBuffer(image, layout, buffer,
                             vk::BufferImageCopy{.imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
                                                 .imageOffset = {s32(offset_x), s32(offset_y), 0},
                                                 .imageExtent = {w, h, 1}});
    const char* dir = std::getenv("TLG_DUMP_DIR");
    char path[512];
    if (trace)
        std::snprintf(path, sizeof(path), "%s/f%03d_%s_%ux%u_rgba.raw", dir && *dir ? dir : "out/dump", index, name, w, h);
    else
        std::snprintf(path, sizeof(path), "%s/%s_%03d_%ux%u.raw", dir && *dir ? dir : "out/dump", name, index, w, h);
    scheduler.DeferPriorityOperation([allocator = instance.GetAllocator(), buffer, allocation, info,
                                      size = buffer_ci.size, file = std::string{path}] {
        vmaInvalidateAllocation(allocator, allocation, 0, VK_WHOLE_SIZE);
        if (FILE* f = std::fopen(file.c_str(), "wb")) {
            std::fwrite(info.pMappedData, 1, size, f);
            std::fclose(f);
        }
        vmaDestroyBuffer(allocator, buffer, allocation);
    });
    return true;
}

#ifdef ISS_ANDROID_BIONIC
// Copy before the existing FOREIGN queue-family handoff, while this queue owns the AHB.
static void CaptureAhb(const Instance& instance, Scheduler& scheduler, vk::CommandBuffer cmd,
                       Swapchain& swapchain, Frame& frame) {
    if (frame.capture_index < 0 || frame.capture_serial == frame.captured_ahb_serial) return;
    frame.captured_ahb_serial = frame.capture_serial;
    const auto extent = swapchain.GetExtent();
    const auto crop = IssTemporalCapture::Center(extent.width, extent.height);
    const char* status = "unsupported";
    if (swapchain.CaptureSupported()) {
        const vk::ImageMemoryBarrier ready{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eTransferRead,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = vk::ImageLayout::eTransferSrcOptimal,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain.Image(),
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, ready);
        const bool copied = DumpRaw(instance, scheduler, cmd, swapchain.Image(),
            vk::ImageLayout::eTransferSrcOptimal, crop.width, crop.height, "trace_ahb",
            frame.capture_index, crop.x, crop.y, true);
        auto restore = ready;
        restore.srcAccessMask = vk::AccessFlagBits::eTransferRead;
        restore.dstAccessMask = vk::AccessFlagBits::eTransferWrite;
        restore.oldLayout = vk::ImageLayout::eTransferSrcOptimal;
        restore.newLayout = vk::ImageLayout::eTransferDstOptimal;
        cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer, {}, {}, {}, restore);
        status = copied ? "copied" : "allocation_failed";
    }
    swapchain.SetCaptureToken(frame.capture_index, frame.capture_serial, status, crop.x, crop.y, crop.width, crop.height, frame.width, frame.height);
}
#endif

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address) {
    // tlg: TLG_FRAME_DUMP_TRIGGER=<file>: when the file exists it is removed and this frame's
    // guest display buffer and presented image are written to TLG_DUMP_DIR (default out/dump)
    // as raw 32-bit pixels (menus and movies included; the upscaler dumps scene frames only).
    // TLG_FRAME_DUMP_COUNT=N: one trigger dumps N consecutive frames (flicker hunting).
    static const char* frame_dump_trigger = std::getenv("TLG_FRAME_DUMP_TRIGGER");
    static const int frame_dump_count = [] {
        const char* env = std::getenv("TLG_FRAME_DUMP_COUNT");
        const int n = env ? std::atoi(env) : 1;
        return n < 1 ? 1 : n > 600 ? 600 : n;
    }();
    static int frame_dump_index = 0;
    static int frame_dump_left = 0;
    if (frame_dump_trigger && frame_dump_left == 0 && std::remove(frame_dump_trigger) == 0) {
        frame_dump_left = frame_dump_count;
    }
    const bool frame_dump = frame_dump_left > 0;
    if (frame_dump) {
        --frame_dump_left;
    }
    // tlg: scaled upscaler presets: the output-size display buffer drawn by the port.
    TemporalUpscaler::Display display{};
    const bool upscaled = rasterizer->GetUpscaler().DisplayOverride(cpu_address, display);
    VideoCore::ImageId image_id{};
    if (!upscaled) {
        auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
        image_id = texture_cache.FindImage(desc);
        texture_cache.UpdateImage(image_id);
    }

    Frame* frame = GetRenderFrame();
#ifdef ISS_ANDROID_BIONIC
    frame->capture_index = upscaled ? display.capture_index : -1;
    frame->capture_serial = upscaled ? display.capture_serial : 0;
#endif

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange{frame_subresources},
    };

    draw_scheduler.EndRendering();
    const auto cmdbuf = draw_scheduler.CommandBuffer();
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    VideoCore::ImageViewInfo view_info{};
    view_info.format = GetFrameViewFormat(attribute.attrib.pixel_format);
    // Exclude alpha from output frame to avoid blending with UI.
    view_info.mapping.a = vk::ComponentSwizzle::eOne;

    vk::ImageView image_view{};
    vk::Extent2D image_size{};
    if (upscaled) {
        const auto device = instance.GetDevice();
        image_view = Check(device.createImageView({
            .image = display.image,
            .viewType = vk::ImageViewType::e2D,
            .format = view_info.format,
            .components = {vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eIdentity,
                           vk::ComponentSwizzle::eIdentity, vk::ComponentSwizzle::eOne},
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        }));
        draw_scheduler.DeferOperation([device, image_view] { device.destroyImageView(image_view); });
        image_size = vk::Extent2D{display.width, display.height};
        const vk::ImageMemoryBarrier2 to_read{
            .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
            .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
            .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
            .oldLayout = vk::ImageLayout::eGeneral,
            .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
            .image = display.image,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1},
        };
        cmdbuf.pipelineBarrier2({.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &to_read});
    } else {
        auto& image = texture_cache.GetImage(image_id);
        if (frame_dump) {
            runtime.Transit(&image, vk::ImageLayout::eTransferSrcOptimal,
                            vk::PipelineStageFlagBits2::eTransfer, vk::AccessFlagBits2::eTransferRead);
            runtime.FlushBarriers();
            DumpRaw(instance, draw_scheduler, cmdbuf, image.GetImage(),
                    vk::ImageLayout::eTransferSrcOptimal, image.info.size.width,
                    image.info.size.height, "display", frame_dump_index);
        }
        image_view = *image.FindView(view_info).image_view;
        image_size = vk::Extent2D{image.info.size.width, image.info.size.height};
        runtime.Transit(&image, vk::ImageLayout::eShaderReadOnlyOptimal,
                        vk::PipelineStageFlagBits2::eFragmentShader,
                        vk::AccessFlagBits2::eShaderRead);
        runtime.FlushBarriers();
    }
    expected_ratio = static_cast<float>(image_size.width) / static_cast<float>(image_size.height);

    // tlg: FSR 1, RCAS and its strength come from the in-game menu, every frame.
    const auto& graphics = TlgSettings::Get();
#ifdef ISS_ANDROID_BIONIC
    fsr_settings.enable = false;
    fsr_settings.use_rcas = false;
#else
    fsr_settings.enable = graphics.fsr1;
    fsr_settings.use_rcas = graphics.rcas;
    fsr_settings.rcas_attenuation = TlgGraphics::RcasAttenuation(graphics.rcas_strength);
#endif
    TlgSettings::Get().image_width = int(image_size.width);
    TlgSettings::Get().image_height = int(image_size.height);
    TlgSettings::Get().window_width = int(frame->width);
    TlgSettings::Get().window_height = int(frame->height);
    TlgSettings::Get().rcas_applied =
        fsr_settings.use_rcas &&
        TlgGraphics::FsrPassRuns(fsr_settings.enable, true, int(image_size.width), int(image_size.height),
                                  int(frame->width), int(frame->height));
#ifndef ISS_ANDROID_BIONIC
    image_view = fsr_pass.Render(cmdbuf, image_view, image_size, {frame->width, frame->height},
                                 fsr_settings, frame->is_hdr);
#endif

    // Vulkan has no sRGB variant of the 10-bit format, so an A2R10G10B10Srgb buffer reaches
    // the post process pass still sRGB encoded and has to be decoded there instead.
    pp_settings.srgb_input =
        attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb;
    pp_pass.Render(cmdbuf, image_view, image_size, *frame, pp_settings);
    if (frame_dump) {
        // The presented image as the swapchain blit reads it (General after the pass).
        const vk::MemoryBarrier2 done{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                      .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                                      .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                      .dstAccessMask = vk::AccessFlagBits2::eTransferRead};
        cmdbuf.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &done});
        DumpRaw(instance, draw_scheduler, cmdbuf, frame->image, vk::ImageLayout::eGeneral,
                frame->width, frame->height, "output", frame_dump_index);
        ++frame_dump_index;
    }

    // Flush frame creation commands.
    frame->ready_semaphore = draw_scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = draw_scheduler.CurrentTick();
    SubmitInfo info{};
    draw_scheduler.Flush(info);

    // tlg: the GPU command thread runs at most TLG_FRAMES_AHEAD (default 1) guest frames
    // ahead of the GPU: it waits here for the frame that many flips back. When the GPU is the
    // bottleneck it finishes frames at an even rate; without this bound the command thread ran
    // ahead and then blocked wherever a resource ran out, so flips (and the guest's frame
    // timing) came in bursts: 12.5/25 ms alternation at 80 FPS. 0 turns it off.
    // The in-game menu changes it live (TlgSettings::frames_queued; TLG_FRAMES_AHEAD at start).
    const u32 frames_ahead = u32(std::max(0, TlgSettings::Get().frames_queued.load()));
    if (!frames_ahead) {
        recent_frame_ticks.clear();
    } else {
        recent_frame_ticks.push_back(frame->ready_tick);
        while (recent_frame_ticks.size() > frames_ahead) {
            const u64 tick = recent_frame_ticks.front();
            recent_frame_ticks.pop_front();
            if (recent_frame_ticks.size() == frames_ahead) {
                draw_scheduler.Wait(tick);
            }
        }
    }
    return frame;
}

Frame* Presenter::PrepareBlankFrame(bool present_thread) {
    // Request a free presentation frame.
    Frame* frame = GetRenderFrame();
#ifdef ISS_ANDROID_BIONIC
    frame->capture_index = -1;
    frame->capture_serial = 0;
#endif

    auto& scheduler = present_thread ? present_scheduler : draw_scheduler;
    scheduler.EndRendering();

    const auto cmdbuf = scheduler.CommandBuffer();

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const vk::RenderingAttachmentInfo attachment = {
        .imageView = frame->image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .extent = {frame->width, frame->height},
            },
        .layerCount = 1,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &attachment,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    cmdbuf.beginRendering(rendering_info);
    cmdbuf.endRendering();

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetWorkSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

void Presenter::PresentLoadingFrame() {
    if (window.GetWidth() == 0 || window.GetHeight() == 0) {
        return;
    }
    swapchain.UpdateVsync();
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }
    if (!swapchain.AcquireNextImage()) {
        return;
    }
    const vk::Image image = swapchain.Image();
    const vk::Extent2D extent = swapchain.GetExtent();
    const auto cmdbuf = present_scheduler.CommandBuffer();
    const auto acquire_queues = swapchain.AcquireQueues();
    const auto release_queues = swapchain.ReleaseQueues();
    const auto present_layout = swapchain.PresentLayout();
    const vk::ImageSubresourceRange range{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };
    const auto barrier = [&](vk::ImageLayout from, vk::ImageLayout to, vk::AccessFlags src,
                             vk::AccessFlags dst) {
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eAllCommands, {}, {}, {},
                               vk::ImageMemoryBarrier{
                                   .srcAccessMask = src,
                                   .dstAccessMask = dst,
                                   .oldLayout = from,
                                   .newLayout = to,
                                   .srcQueueFamilyIndex =
                                       from == vk::ImageLayout::eUndefined ? acquire_queues.first
                                       : to == present_layout ? release_queues.first : VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex =
                                       from == vk::ImageLayout::eUndefined ? acquire_queues.second
                                       : to == present_layout ? release_queues.second : VK_QUEUE_FAMILY_IGNORED,
                                   .image = image,
                                   .subresourceRange = range,
                               });
    };
    barrier(vk::ImageLayout::eUndefined, vk::ImageLayout::eTransferDstOptimal, {},
            vk::AccessFlagBits::eTransferWrite);
    cmdbuf.clearColorImage(image, vk::ImageLayout::eTransferDstOptimal,
                           vk::ClearColorValue{std::array<float, 4>{0.0f, 0.0f, 0.0f, 1.0f}},
                           range);
    barrier(vk::ImageLayout::eTransferDstOptimal, vk::ImageLayout::eColorAttachmentOptimal,
            vk::AccessFlagBits::eTransferWrite, vk::AccessFlagBits::eColorAttachmentWrite);
    TlgOverlay::Render(cmdbuf, swapchain.ImageView(), extent);
    barrier(vk::ImageLayout::eColorAttachmentOptimal, present_layout,
            vk::AccessFlagBits::eColorAttachmentWrite, {});
    SubmitInfo info{};
    AddImageAcquiredWait(info, swapchain.GetImageAcquiredSemaphore());
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    present_scheduler.Flush(info);
    {
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        swapchain.Present();
    }
    // A few frames a second during start-up: waiting keeps semaphores and the swapchain simple.
    present_scheduler.Finish();
}

void Presenter::Present(Frame* frame, bool is_reusing_frame, bool is_game_frame) {
    // Free the frame for reuse
    const auto free_frame = [&] {
        if (!is_reusing_frame) {
            last_submit_frame = frame;
            std::scoped_lock fl{free_mutex};
            free_queue.push(frame);
            free_cv.notify_one();
        }
    };

    // tlg: a minimized window has no pixels on Windows (0x0); keep the swapchain and skip
    // presenting until the window is restored.
    if (window.GetWidth() == 0 || window.GetHeight() == 0) {
        free_frame();
        return;
    }

#ifdef __ANDROID__
    PollLsfgState();
#endif
    // Recreate the swapchain if the window was resized.
    swapchain.UpdateVsync();
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }

#ifdef __ANDROID__
    if (lsfg && is_game_frame && !is_reusing_frame && frame->width && frame->height) {
        if (PresentWithLsfg(frame)) {
            free_frame(); DebugState.IncFlipFrameNum(); return;
        }
        if (!lsfg) ApplyLsfgOutputMode();
    }
#endif
    if (!swapchain.AcquireNextImage()) {
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
        if (!swapchain.AcquireNextImage()) {
            // User resizes the window too fast and GPU can't keep up. Skip this frame.
            LOG_WARNING(Render_Vulkan, "Skipping frame!");
            free_frame();
            return;
        }
    }

    // Reset fence for queue submission. Do it here instead of GetRenderFrame() because we may
    // skip frame because of slow swapchain recreation. If a frame skip occurs, we skip signal
    // the frame's present fence and future GetRenderFrame() call will hang waiting for this frame.
    const auto reset_result = instance.GetDevice().resetFences(frame->present_done);
    ASSERT_MSG(reset_result == vk::Result::eSuccess,
               "Unexpected error resetting present done fence: {}", vk::to_string(reset_result));

    // tlg: the game frame is blitted (letterboxed) straight into the swapchain image.
    const vk::Image swapchain_image = swapchain.Image();
    const auto acquire_queues = swapchain.AcquireQueues();
    const auto release_queues = swapchain.ReleaseQueues();
    auto& scheduler = present_scheduler;
    const auto cmdbuf = scheduler.CommandBuffer();

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Present",
        });
    }

    {
        const vk::Extent2D extent = swapchain.GetExtent();
        SetExpectedGameSize(s32(extent.width), s32(extent.height));
        const vk::ImageSubresourceRange color_range{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };
        const std::array pre_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = acquire_queues.first,
                .dstQueueFamilyIndex = acquire_queues.second,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eTransfer, vk::DependencyFlagBits::eByRegion,
                               {}, {}, pre_barriers);
        BlitPresentedFrame(cmdbuf, frame->image, swapchain_image, frame->width, frame->height,
                           extent, color_range);
#ifdef ISS_ANDROID_BIONIC
        if (!is_reusing_frame) CaptureAhb(instance, scheduler, cmdbuf, swapchain, *frame);
#endif
        // tlg: the settings menu / FPS counter over the frame, at display resolution.
        const bool overlay = TlgOverlay::Visible();
        const std::array post_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = overlay ? vk::AccessFlagBits::eColorAttachmentRead |
                                               vk::AccessFlagBits::eColorAttachmentWrite
                                         : vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = overlay ? vk::ImageLayout::eColorAttachmentOptimal
                                     : swapchain.PresentLayout(),
                .srcQueueFamilyIndex = overlay ? VK_QUEUE_FAMILY_IGNORED : release_queues.first,
                .dstQueueFamilyIndex = overlay ? VK_QUEUE_FAMILY_IGNORED : release_queues.second,
                .image = swapchain_image,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eAllCommands,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barriers);
        if (overlay) {
            TlgOverlay::Render(cmdbuf, swapchain.ImageView(), extent);
            const vk::ImageMemoryBarrier to_present{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .newLayout = swapchain.PresentLayout(),
                .srcQueueFamilyIndex = release_queues.first,
                .dstQueueFamilyIndex = release_queues.second,
                .image = swapchain_image,
                .subresourceRange = color_range,
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                   vk::PipelineStageFlagBits::eBottomOfPipe,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, to_present);
        }
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    // Flush vulkan commands.

    SubmitInfo info{};
    AddImageAcquiredWait(info, swapchain.GetImageAcquiredSemaphore());
    info.AddWait(frame->ready_semaphore, frame->ready_tick);
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    info.AddSignal(frame->present_done);
    scheduler.Flush(info);

    // Present to swapchain.
    {
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        if (!swapchain.Present() && window.GetWidth() != 0 && window.GetHeight() != 0) {
            swapchain.Recreate(window.GetWidth(), window.GetHeight());
        }
    }

    free_frame();
    if (!is_reusing_frame && is_game_frame) {
        DebugState.IncFlipFrameNum();
    }
}

#ifdef __ANDROID__
void Presenter::PollLsfgState() {
    IssLsfgState::State next;
    if (!lsfg_watch.Poll(next)) {
        WriteLsfgStats();
        return; // 无变化
    }
    if (next == lsfg_requested && (lsfg || !next.On())) {
        WriteLsfgStats();
        return; // 状态没变（或引擎该在而不在的请求已应用过）
    }
    ApplyLsfgState(next);
}

void Presenter::WriteLsfgStats() {
    const char* state_path = IssLsfgState::FilePath();
    if (!state_path || !*state_path) return; // env 未设（env-only 运行）：无遥测
    const auto now = std::chrono::steady_clock::now();
    const bool active = lsfg != nullptr;
    if (active) {
        if (now - lsfg_stats_ts < std::chrono::seconds(1)) return;
        lsfg_stats_ts = now;
        double src = 0, pres = 0;
        if (!lsfg->Rates(&src, &pres)) return; // 首窗未满：下个周期再写
        std::string path = state_path;
        path = path.substr(0, path.rfind('/') + 1) + "framegen.stats"; // 与 app 侧 FrameGenSettings.STATS_FILE_NAME 一致
        std::string tmp = path + ".tmp";
        std::FILE* f = std::fopen(tmp.c_str(), "w");
        if (!f) return;
        std::fprintf(f, "source_fps=%.1f\npresented_fps=%.1f\nmultiplier=%u\n",
                     src, pres, lsfg->Multiplier());
        std::fclose(f);
        std::rename(tmp.c_str(), path.c_str());
        lsfg_stats_active = true;
        lsfg_stats_cleared = false; // 引擎激活：下次关闭需再清一次
    } else if (!lsfg_stats_cleared) {
        // 关闭/引擎失败/从未激活：清零一次，HUD 回单一帧率（spec 语义）。不依赖本会话
        // 是否曾激活——上一会话残留的 framegen.stats（倍率≥2）也必须在此被清掉，
        // 否则 HUD 会把它当成实时插帧遥测显示「LSFG Nx」。
        std::string path = state_path;
        path = path.substr(0, path.rfind('/') + 1) + "framegen.stats";
        std::string tmp = path + ".tmp";
        std::FILE* f = std::fopen(tmp.c_str(), "w");
        if (!f) return;
        std::fprintf(f, "source_fps=0\npresented_fps=0\nmultiplier=0\n");
        std::fclose(f);
        std::rename(tmp.c_str(), path.c_str());
        lsfg_stats_active = false;
        lsfg_stats_cleared = true; // 本会话只清一次：之后每帧只有布尔判断，无 I/O
    }
}

void Presenter::ApplyLsfgState(const IssLsfgState::State& next) {
    lsfg_requested = next;
    // 帧边界：本函数跑在 present 线程两帧之间，没有循环持有的缓冲。引擎的 flow
    // scale/模型/生成帧数在启动时固定——任何“开”态变化都是重建 Lsfg：旧的等最后
    // 一批完成并析构，新的在下一个真实帧建上下文（头几帧只填时域历史）。
    lsfg.reset();
    if (next.On()) {
        lsfg = Lsfg::Create(instance, next);
    }
    // Frame generation controls buffer count; FIFO follows the user VSync setting.
    ApplyLsfgOutputMode();
    if (lsfg) {
        LOG_INFO(Render_Vulkan, "LSFG: now x{}, flow {:.2f}, performance {}, fp16 {}",
                 next.multiplier, next.flow, next.performance ? 1 : 0, next.fp16 ? 1 : 0);
    } else {
        LOG_INFO(Render_Vulkan, "LSFG: off");
    }
}

void Presenter::ApplyLsfgOutputMode() {
    swapchain.SetFrameGeneration(lsfg ? lsfg->Multiplier() : 0);
    ApplyFramesCap();
}

void Presenter::ApplyFramesCap() {
    frames_cap.store(lsfg ? lsfg->FramesCap() : 0, std::memory_order_relaxed);
    free_cv.notify_all();
}

bool Presenter::PresentWithLsfg(Frame* frame) {
    // Keep the present_done fence SIGNALED until we have actually acquired
    // a buffer for the real frame. If Bloodframe fails, or the compositor
    // offers no buffer, GetRenderFrame must not wait forever on an unsignaled
    // fence that was never submitted.
    using Clock = std::chrono::steady_clock;
    // iss：Flush 在本线程同步提交（vkQueueSubmit 返回后才返回），无需 BB 的
    // WaitSubmitted(tick) 守卫（其 recording 线程异步提交才有此需求；iss 的无参
    // WaitSubmitted 是 GPU 完成等待，会无谓卡流水线——见 vk_presenter 旧注释）。
    const int generated =
        lsfg->ProcessFrame(present_scheduler, frame->image, frame->width, frame->height,
                           frame->ready_semaphore, frame->ready_tick);
    if (generated < 0) {
        lsfg.reset(); // 本次运行内关闭（日志有原因）：本帧走常规路径
        return false;
    }
    // 每帧一次性全部入队，各写各的缓冲：生成帧按序，然后真实帧。只有交付给 app
    // 的节奏被拉开。
    const u32 buffer_set = swapchain.Generation();
    const auto base = Clock::now(); // 突发交付的节拍起点
    u32 handed = 0;
    for (int i = 0; i < generated; ++i) {
        const Lsfg::Output& output = lsfg->GetOutput(i);
        // 生成帧来自外部（EXTERNAL）队列族、GENERAL 布局：blit 进交换链缓冲时按
        // 外部来源取所有权，并等待该帧的 sync file（经 AddWait 的时间线信号量）。
        if (!swapchain.AcquireNextImage()) {
            break; // 没有缓冲：本循环其余生成帧不显示
        }
        const vk::Image dst = swapchain.Image();
        const vk::Extent2D extent = swapchain.GetExtent();
        auto& scheduler = present_scheduler;
        const auto cmdbuf = scheduler.CommandBuffer();
        const vk::ImageSubresourceRange color_range{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };
        const auto [acquire_src, acquire_dst] = swapchain.AcquireQueues();
        // 引擎输出图的所有权在 VK_QUEUE_FAMILY_EXTERNAL（引擎侧交接，见 iss_lsfg.cpp）；
        // FOREIGN 是 app↔game 的 AHB 交接语义，别混用。
        const std::array pre_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = swapchain.PresentLayout(),
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = acquire_src,
                .dstQueueFamilyIndex = acquire_dst,
                .image = dst,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
                .dstQueueFamilyIndex = instance.GetGraphicsQueueFamilyIndex(),
                .image = output.image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, {}, {}, pre_barriers);
        cmdbuf.blitImage(output.image, vk::ImageLayout::eTransferSrcOptimal, dst,
                         vk::ImageLayout::eTransferDstOptimal,
                         MakeImageBlitFit(frame->width, frame->height, extent.width, extent.height),
                         vk::Filter::eLinear);
        const auto [release_src, release_dst] = swapchain.ReleaseQueues();
        const vk::ImageMemoryBarrier to_present{
            .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
            .dstAccessMask = vk::AccessFlagBits::eNone,
            .oldLayout = vk::ImageLayout::eTransferDstOptimal,
            .newLayout = swapchain.PresentLayout(),
            .srcQueueFamilyIndex = release_src,
            .dstQueueFamilyIndex = release_dst,
            .image = dst,
            .subresourceRange = color_range,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eBottomOfPipe,
                               vk::DependencyFlagBits::eByRegion, {}, {}, to_present);
        // Return the generated image to Bloodframe in GENERAL layout.
        // It was transitioned from GENERAL -> TRANSFER_SRC in pre_barriers.
        // Releasing it with oldLayout=GENERAL was INVALID and could cause
        // Adreno rendering corruption or a VK_ERROR_DEVICE_LOST.
        const vk::ImageMemoryBarrier output_release{
            .srcAccessMask = vk::AccessFlagBits::eTransferRead,
            .dstAccessMask = vk::AccessFlagBits::eNone,
            .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
            .newLayout = vk::ImageLayout::eGeneral,
            .srcQueueFamilyIndex = instance.GetGraphicsQueueFamilyIndex(),
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
            .image = output.image,
            .subresourceRange = color_range,
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eBottomOfPipe,
                               vk::DependencyFlagBits::eByRegion, {}, {}, output_release);
        // 交付节奏：第一个生成帧立即上，后续生成帧按 spacing 依次拉开（真实帧最后）。
        // 放在 present 之前而非循环末尾——睡在交付前只影响展示节拍；堆在末尾会
        // 在 frames_cap 下白白压低源帧率。
        if (lsfg->Paced() && i > 0) {
            const double spacing_ms =
                std::clamp(lsfg->FrameIntervalMs() / double(generated + 1), 2.0, 60.0);
            std::this_thread::sleep_until(base + std::chrono::microseconds(
                                                        s64(spacing_ms * 1000.0 * u64(i))));
        }
        SubmitInfo info{};
        // app 的 release fence（导入为本缓冲的 acquire 信号量）：合成器可能仍在读
        // 该缓冲——不等待就是覆写竞态。null（首用无 fence）被 AddWait 丢弃。
        AddImageAcquiredWait(info, swapchain.GetImageAcquiredSemaphore());
        if (output.wait) {
            info.AddWait(output.wait);
        }
        info.AddSignal(swapchain.GetPresentReadySemaphore());
        if (output.done) {
            info.AddSignal(output.done);
        }
        scheduler.Flush(info);
        lsfg->OutputSubmitted(u32(i), scheduler.CurrentTick() - 1);
        {
            std::scoped_lock submit_lock{Scheduler::submit_mutex};
            swapchain.Present(); // 交给 app（该缓冲被扣住直到 RELEASE）
        }
        ++handed;
    }
    // 真实帧：走常规 blit 路径进它已 acquire 的缓冲（Present() 主体的剩余部分在此
    // 不可复用——它已过 acquire 点，因此这里把真实帧也按同一路径交付）。
    if (!swapchain.AcquireNextImage()) {
        // The real frame was copied to the input AHB by ProcessFrame.
        // Wait for that copy before returning this frame to the free pool,
        // since there will be no present_done fence submission this cycle.
        present_scheduler.Wait(present_scheduler.CurrentTick() - 1);
        std::printf("Bloodframe: no output buffer for real frame; source copy drained\n");
        return true;
    }
    {
        const vk::Image dst = swapchain.Image();
        const vk::Extent2D extent = swapchain.GetExtent();
        auto& scheduler = present_scheduler;
        const auto cmdbuf = scheduler.CommandBuffer();
        const vk::ImageSubresourceRange color_range{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = VK_REMAINING_ARRAY_LAYERS,
        };
        const auto [acquire_src, acquire_dst] = swapchain.AcquireQueues();
        const std::array pre_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                .oldLayout = swapchain.PresentLayout(),
                .newLayout = vk::ImageLayout::eTransferDstOptimal,
                .srcQueueFamilyIndex = acquire_src,
                .dstQueueFamilyIndex = acquire_dst,
                .image = dst,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eTransfer,
                               vk::DependencyFlagBits::eByRegion, {}, {}, pre_barriers);
        BlitPresentedFrame(cmdbuf, frame->image, dst, frame->width, frame->height,
                           extent, color_range);
#ifdef ISS_ANDROID_BIONIC
        CaptureAhb(instance, scheduler, cmdbuf, swapchain, *frame);
#endif
        // 游戏内设置菜单叠加层与直通路径一致（显示分辨率）。
        const bool overlay = TlgOverlay::Visible();
        const std::array post_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = overlay ? vk::AccessFlagBits::eColorAttachmentRead |
                                               vk::AccessFlagBits::eColorAttachmentWrite
                                         : vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eTransferDstOptimal,
                .newLayout = overlay ? vk::ImageLayout::eColorAttachmentOptimal
                                     : swapchain.PresentLayout(),
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = dst,
                .subresourceRange = color_range,
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eTransferRead,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eTransferSrcOptimal,
                .newLayout = vk::ImageLayout::eGeneral,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange = color_range,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer,
                               vk::PipelineStageFlagBits::eAllCommands,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barriers);
        if (overlay) {
            TlgOverlay::Render(cmdbuf, swapchain.ImageView(), extent);
            const auto [release_src, release_dst] = swapchain.ReleaseQueues();
            const vk::ImageMemoryBarrier to_present{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eNone,
                .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .newLayout = swapchain.PresentLayout(),
                .srcQueueFamilyIndex = release_src,
                .dstQueueFamilyIndex = release_dst,
                .image = dst,
                .subresourceRange = color_range,
            };
            cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                   vk::PipelineStageFlagBits::eBottomOfPipe,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, to_present);
        } else {
            const auto [release_src, release_dst] = swapchain.ReleaseQueues();
            if (release_src != release_dst) {
                const vk::ImageMemoryBarrier hand_over{
                    .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                    .dstAccessMask = vk::AccessFlagBits::eNone,
                    .oldLayout = swapchain.PresentLayout(),
                    .newLayout = swapchain.PresentLayout(),
                    .srcQueueFamilyIndex = release_src,
                    .dstQueueFamilyIndex = release_dst,
                    .image = dst,
                    .subresourceRange = color_range,
                };
                cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                                       vk::PipelineStageFlagBits::eAllCommands,
                                       vk::DependencyFlagBits::eByRegion, {}, {}, hand_over);
            }
        }
        SubmitInfo info{};
        AddImageAcquiredWait(info, swapchain.GetImageAcquiredSemaphore());
        info.AddWait(frame->ready_semaphore, frame->ready_tick);
        info.AddSignal(swapchain.GetPresentReadySemaphore());
        // Only reset the fence once its signaling submission is guaranteed
        // to happen. The prior frame is already completed before reuse.
        const auto reset_result = instance.GetDevice().resetFences(frame->present_done);
        ASSERT_MSG(reset_result == vk::Result::eSuccess,
                   "Unexpected reset of LSFG present fence: {}", vk::to_string(reset_result));
        info.AddSignal(frame->present_done);
        scheduler.Flush(info);
        // Preserve Daedalus Bionic sync-fd submission semantics; no CPU-side
        // completion wait here (unlike upstream's older path).
        swapchain.Present();
        ++handed;
    }
    if (swapchain.Generation() != buffer_set) {
        return true; // 中途缓冲换代：本循环已交付的帧随旧代作废
    }
    return true;
}
#endif

Frame* Presenter::GetRenderFrame() {
    const IssBionicPerf::Scope slot_timer{IssBionicPerf::Stage::FrameSlot};
    // Wait for free presentation frames
    Frame* frame;
    {
        std::unique_lock lock{free_mutex};
        free_cv.wait(lock, [this] {
#ifdef __ANDROID__
            const u32 cap = frames_cap.load(std::memory_order_relaxed);
            return !free_queue.empty() && (!cap || present_frames.size() - free_queue.size() < cap);
#else
            return !free_queue.empty();
#endif
        });
        LOG_DEBUG(Render_Vulkan, "Got render frame, remaining {}", free_queue.size() - 1);

        // Take the frame from the queue
        frame = free_queue.front();
        free_queue.pop();
    }

    const vk::Device device = instance.GetDevice();
    vk::Result result{};

    const auto wait = [&]() {
        result = device.waitForFences(frame->present_done, false, std::numeric_limits<u64>::max());
        return result;
    };

    // Wait for the presentation to be finished so all frame resources are free
    while (wait() != vk::Result::eSuccess) {
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
        // Retry if the waiting times out
        if (result == vk::Result::eTimeout) {
            continue;
        }
    }

    if (frame->width != expected_frame_width || frame->height != expected_frame_height ||
        frame->is_hdr != swapchain.GetHDR()) {
        RecreateFrame(frame, expected_frame_width, expected_frame_height);
    }

    return frame;
}

void Presenter::SetExpectedGameSize(s32 width, s32 height) {
    if (width <= 0 || height <= 0) {
        return; // no surface (minimized window): keep the last frame size
    }
    const float ratio = (float)width / (float)height;

    expected_frame_height = height;
    expected_frame_width = width;
    if (ratio > expected_ratio) {
        expected_frame_width = static_cast<s32>(height * expected_ratio);
    } else {
        expected_frame_height = static_cast<s32>(width / expected_ratio);
    }
}

} // namespace Vulkan
