// Adapted from Bloodborne-Android 2e472653; Android presentation infrastructure.
// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: see vk_swapchain_ahb.h.
#ifdef __ANDROID__
#include <android/hardware_buffer.h>
#include <poll.h>
#include <unistd.h>
#include <algorithm>
#include <utility>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include "common/assert.h"
#include "tlg_settings.h"
#include "iss_bionic_perf.h"
#include "video_core/renderer_vulkan/vk_wait_diagnostics.h"
#include "common/logging/log.h"
#include "imgui/renderer/imgui_core.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_swapchain_ahb.h"

namespace Vulkan {

extern std::atomic<u32> g_tlg_compiles;
extern std::atomic<u64> g_tlg_compile_ns;

namespace {

constexpr u32 AhbFormat = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
constexpr u64 AhbUsage = AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT |
                         AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                         AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY;

/// ISS_PRESENT_BUFFERS: the number of buffers (3 by default: one shown, one pending, one rendered).
/// With Lossless Scaling frame generation (multiplier 2-4) a cycle holds one buffer per frame it shows
/// (the multiplier) while the app still has the last ones: multiplier + 3.
u32 BufferCount(u32 lsfg_multiplier) {
    if (const char* value = std::getenv("ISS_PRESENT_BUFFERS")) {
        const long count = std::strtol(value, nullptr, 10);
        if (count >= 2 && count <= static_cast<long>(8)) {
            return static_cast<u32>(count);
        }
    }
    if (lsfg_multiplier >= 2 && lsfg_multiplier <= 4) {
        return lsfg_multiplier + 3;
    }
    return 3;
}

/// Waits (up to timeout_ms) for a sync file to signal. True when signaled.
bool WaitSyncFile(int fd, int timeout_ms) {
    pollfd pfd{fd, POLLIN, 0};
    int ready;
    do {
        ready = poll(&pfd, 1, timeout_ms);
    } while (ready < 0 && errno == EINTR);
    return ready > 0;
}

} // namespace

u32 Swapchain::MaxImageCount() {
    return std::max(BufferCount(0), BufferCount(4)); // covers live 2x/3x/4x switches
}

Swapchain::Swapchain(const Instance& instance_, const Frontend::WindowSDL& window_, u32 lsfg_multiplier)
    : instance{instance_}, window{window_} {
    framegen_multiplier = lsfg_multiplier;
    image_count = BufferCount(lsfg_multiplier);
    const vk::PhysicalDeviceExternalSemaphoreInfo semaphore_info{
        .handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eSyncFd,
    };
    const auto semaphore_props = instance.GetPhysicalDevice().getExternalSemaphoreProperties(semaphore_info);
    ASSERT_MSG(
        (semaphore_props.externalSemaphoreFeatures & vk::ExternalSemaphoreFeatureFlagBits::eExportable) &&
            (semaphore_props.externalSemaphoreFeatures & vk::ExternalSemaphoreFeatureFlagBits::eImportable),
        "The Vulkan driver cannot export and import sync-file semaphores (SYNC_FD): the AHB "
        "swapchain cannot synchronize with the app");
    if (IssPresent::Client::Configured()) {
        if (!client.Connect(window.GetWidth(), window.GetHeight(), AhbFormat, image_count,
                            TlgSettings::Get().vsync.load())) {
            std::printf("Present: no connection to the app: frames are rendered but not shown\n");
        }
    } else {
        std::printf("Present: ISS_PRESENT_SOCKET is not set: frames are rendered but not shown\n");
    }
    Create(window.GetWidth(), window.GetHeight());
}

// Bloodframe must not force FIFO: on a 60 Hz compositor that backpressures
// real frames to 30 FPS at 2x or 15 FPS at 4x, even with VSync disabled.
void Swapchain::UpdateVsync() {
    const bool fifo = TlgSettings::Get().vsync.load();
    if (client.Connected()) client.SetFifo(fifo);
}

Swapchain::~Swapchain() {
    Destroy();
    client.Disconnect();
}

std::pair<u32, u32> Swapchain::AcquireQueues() const {
    return {VK_QUEUE_FAMILY_FOREIGN_EXT, instance.GetGraphicsQueueFamilyIndex()};
}

std::pair<u32, u32> Swapchain::ReleaseQueues() const {
    return {instance.GetGraphicsQueueFamilyIndex(), VK_QUEUE_FAMILY_FOREIGN_EXT};
}

void Swapchain::Create(u32 width_, u32 height_) {
    width = width_;
    height = height_;
    needs_recreation = false;
    // A 0x0 output (no screen size) has no buffers; they come when the size is back.
    if (width == 0 || height == 0) {
        zero_extent = true;
        needs_recreation = true;
        return;
    }
    zero_extent = false;
    Destroy();
    extent = vk::Extent2D{width, height};
    capture_supported = true;
    ++generation;
    if (client.Connected()) {
        if (!client.BeginGeneration(generation, width, height, AhbFormat, image_count)) {
            PeerGone();
        }
    }
    const vk::Device device = instance.GetDevice();
    buffers.resize(image_count);
    image_acquired.resize(image_count);
    present_ready.resize(image_count);
    for (u32 i = 0; i < image_count; ++i) {
        CreateBuffer(i);
        auto [acquired_result, acquired] = device.createSemaphore({});
        ASSERT_MSG(acquired_result == vk::Result::eSuccess, "Failed to create semaphore: {}",
                   vk::to_string(acquired_result));
        image_acquired[i] = acquired;
        // The present-ready semaphore is exported as a sync file after its signal is submitted.
        const vk::ExportSemaphoreCreateInfo export_info{
            .handleTypes = vk::ExternalSemaphoreHandleTypeFlagBits::eSyncFd,
        };
        auto [ready_result, ready] = device.createSemaphore({.pNext = &export_info});
        ASSERT_MSG(ready_result == vk::Result::eSuccess, "Failed to create exportable semaphore: {}",
                   vk::to_string(ready_result));
        present_ready[i] = ready;
        SetObjectName(device, image_acquired[i], "AHB swapchain image_acquired {}", i);
        SetObjectName(device, present_ready[i], "AHB swapchain present_ready {}", i);
        free_queue.push_back(i);
        if (client.Connected() &&
            !client.SendBuffer(generation, i, width, height, AhbFormat, buffers[i].ahb)) {
            PeerGone();
        }
    }
    image_index = 0;
    LOG_INFO(Render_Vulkan, "AHB swapchain: {} buffers {}x{} (generation {}, {})", image_count, width,
             height, generation, client.Connected() ? "app connected" : "no app");
}

void Swapchain::CreateBuffer(u32 index) {
    const vk::Device device = instance.GetDevice();
    Buffer& buffer = buffers[index];

    const AHardwareBuffer_Desc desc{
        .width = width,
        .height = height,
        .layers = 1,
        .format = AhbFormat,
        .usage = AhbUsage,
    };
    const int allocated = AHardwareBuffer_allocate(&desc, &buffer.ahb);
    ASSERT_MSG(allocated == 0 && buffer.ahb, "AHardwareBuffer_allocate {}x{} failed: {}", width,
               height, allocated);

    auto [props_result, props_chain] = device.getAndroidHardwareBufferPropertiesANDROID<
        vk::AndroidHardwareBufferPropertiesANDROID, vk::AndroidHardwareBufferFormatPropertiesANDROID>(
        *buffer.ahb);
    ASSERT_MSG(props_result == vk::Result::eSuccess,
               "vkGetAndroidHardwareBufferPropertiesANDROID failed: {}", vk::to_string(props_result));
    const auto& props = props_chain.get<vk::AndroidHardwareBufferPropertiesANDROID>();
    const auto& format_props = props_chain.get<vk::AndroidHardwareBufferFormatPropertiesANDROID>();
    ASSERT_MSG(format_props.format != vk::Format::eUndefined,
               "The driver maps the AHardwareBuffer to no Vulkan format (external format {:#x})",
               format_props.externalFormat);
    const auto required = vk::FormatFeatureFlagBits::eColorAttachment | vk::FormatFeatureFlagBits::eBlitDst;
    ASSERT_MSG((format_props.formatFeatures & required) == required,
               "The driver cannot render/blit into the AHardwareBuffer format {} (features {})",
               vk::to_string(format_props.format), vk::to_string(format_props.formatFeatures));
    surface_format.format = format_props.format;

    vk::ImageUsageFlags usage =
        vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst;
    if (format_props.formatFeatures & vk::FormatFeatureFlagBits::eTransferSrc) {
        usage |= vk::ImageUsageFlagBits::eTransferSrc;
    }
    if (format_props.formatFeatures & vk::FormatFeatureFlagBits::eSampledImage) {
        usage |= vk::ImageUsageFlagBits::eSampled;
    }
    capture_supported = capture_supported && bool(usage & vk::ImageUsageFlagBits::eTransferSrc);
    const vk::ExternalMemoryImageCreateInfo external_info{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eAndroidHardwareBufferANDROID,
    };
    const vk::ImageCreateInfo image_info{
        .pNext = &external_info,
        .imageType = vk::ImageType::e2D,
        .format = format_props.format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = usage,
        .sharingMode = vk::SharingMode::eExclusive,
        .initialLayout = vk::ImageLayout::eUndefined,
    };
    auto [image_result, image] = device.createImage(image_info);
    ASSERT_MSG(image_result == vk::Result::eSuccess, "Failed to create the AHB image: {}",
               vk::to_string(image_result));
    buffer.image = image;

    const auto memory_properties = instance.GetPhysicalDevice().getMemoryProperties();
    u32 memory_type = ~0u;
    for (u32 i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if (!(props.memoryTypeBits & (1u << i))) {
            continue;
        }
        if (memory_type == ~0u) {
            memory_type = i;
        }
        if (memory_properties.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal) {
            memory_type = i;
            break;
        }
    }
    ASSERT_MSG(memory_type != ~0u, "No memory type for the AHardwareBuffer (bits {:#x})",
               props.memoryTypeBits);
    const vk::MemoryDedicatedAllocateInfo dedicated{.image = image};
    const vk::ImportAndroidHardwareBufferInfoANDROID import_info{
        .pNext = &dedicated,
        .buffer = buffer.ahb,
    };
    auto [memory_result, memory] = device.allocateMemory({
        .pNext = &import_info,
        .allocationSize = props.allocationSize,
        .memoryTypeIndex = memory_type,
    });
    ASSERT_MSG(memory_result == vk::Result::eSuccess, "Failed to import the AHardwareBuffer: {}",
               vk::to_string(memory_result));
    buffer.memory = memory;
    const auto bind_result = device.bindImageMemory(image, memory, 0);
    ASSERT_MSG(bind_result == vk::Result::eSuccess, "Failed to bind the AHB image: {}",
               vk::to_string(bind_result));

    auto [view_result, view] = device.createImageView({
        .image = image,
        .viewType = vk::ImageViewType::e2D,
        .format = format_props.format,
        .subresourceRange = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                             .levelCount = 1,
                             .layerCount = 1},
    });
    ASSERT_MSG(view_result == vk::Result::eSuccess, "Failed to create the AHB image view: {}",
               vk::to_string(view_result));
    buffer.view = view;
    SetObjectName(device, image, "AHB swapchain image {}", index);
    SetObjectName(device, view, "AHB swapchain view {}", index);
}

void Swapchain::SetFrameGeneration(u32 lsfg_multiplier) {
    framegen_multiplier = lsfg_multiplier;
    if (client.Connected() &&
        !client.SetFifo(TlgSettings::Get().vsync.load())) {
        PeerGone();
    }
    const u32 count = BufferCount(lsfg_multiplier);
    if (count != image_count) {
        image_count = count;
        Recreate(width != 0 ? width : window.GetWidth(), height != 0 ? height : window.GetHeight());
    }
}

void Swapchain::Recreate(u32 width_, u32 height_) {
    LOG_DEBUG(Render_Vulkan, "Recreate the AHB swapchain: width={} height={}", width_, height_);
    Create(width_, height_);
}

void Swapchain::Destroy() {
    vk::Device device = instance.GetDevice();
    const auto wait_result = device.waitIdle();
    if (wait_result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "Failed to wait for device to become idle: {}",
                    vk::to_string(wait_result));
    }
    for (Buffer& buffer : buffers) {
        device.destroyImageView(buffer.view);
        device.destroyImage(buffer.image);
        device.freeMemory(buffer.memory);
        // The app and the compositor keep the memory while they use it (reference counted).
        if (buffer.ahb) {
            AHardwareBuffer_release(buffer.ahb);
        }
        if (buffer.release_fd >= 0) {
            close(buffer.release_fd);
        }
    }
    buffers.clear();
    for (const auto sem : image_acquired) {
        device.destroySemaphore(sem);
    }
    for (const auto sem : present_ready) {
        device.destroySemaphore(sem);
    }
    image_acquired.clear();
    present_ready.clear();
    free_queue.clear();
}

void Swapchain::PeerGone() const {
    // The app is the game's screen: without it (closed, killed) the process ends, as when the
    // window of the glibc build is closed.
    LOG_INFO(Frontend, "The app closed the frame connection");
    std::fflush(stdout);
    std::_Exit(0);
}

void Swapchain::HandleRelease(IssPresent::Release& release) {
    if (release.generation != generation || release.index >= buffers.size()) {
        if (release.fence_fd >= 0) {
            close(release.fence_fd); // a buffer of a set that is gone
        }
        return;
    }
    Buffer& buffer = buffers[release.index];
    if (buffer.release_fd >= 0) {
        close(buffer.release_fd);
    }
    buffer.release_fd = release.fence_fd;
    free_queue.push_back(release.index);
}

bool Swapchain::AcquireNextImage() {
    const IssBionicPerf::Scope timer{IssBionicPerf::Stage::AhbAcquire};
    if (zero_extent || buffers.empty()) {
        needs_recreation = true;
        return false;
    }
    IssPresent::Release release;
    // Releases that arrived meanwhile.
    while (client.Connected()) {
        const int got = client.WaitRelease(release, 0);
        if (got < 0) {
            PeerGone();
        }
        if (got == 0) {
            break;
        }
        HandleRelease(release);
    }
    // All buffers are at the app: the display is slower than the game. Wait for one.
    const auto wait_start = std::chrono::steady_clock::now();
    bool warned = false;
    while (free_queue.empty()) {
        const int got = client.WaitRelease(release, 1000);
        if (got < 0) {
            PeerGone();
        }
        if (got > 0) {
            HandleRelease(release);
        } else if (!warned && std::chrono::steady_clock::now() - wait_start > std::chrono::seconds(5)) {
            warned = true;
            LOG_WARNING(Render_Vulkan, "AHB swapchain: no buffer released by the app for 5 s");
        }
    }
    image_index = free_queue.front();
    free_queue.pop_front();
    ImportAcquire(image_index);
    return !needs_recreation;
}

void Swapchain::ImportAcquire(u32 index) {
    const IssBionicPerf::Scope timer{IssBionicPerf::Stage::AhbImport};
    Buffer& buffer = buffers[index];
    buffer.acquire_imported = false;
    const int fd = buffer.release_fd;
    if (fd < 0) {
        return; // never shown, or released at once: nothing to wait for
    }
    buffer.release_fd = -1;
    if (sync_import_ok) {
        const vk::ImportSemaphoreFdInfoKHR import_info{
            .semaphore = image_acquired[index],
            .flags = vk::SemaphoreImportFlagBits::eTemporary,
            .handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eSyncFd,
            .fd = fd,
        };
        const auto result = instance.GetDevice().importSemaphoreFdKHR(import_info);
        if (result == vk::Result::eSuccess) {
            buffer.acquire_imported = true; // the driver owns the fd now
            return;
        }
        LOG_WARNING(Render_Vulkan,
                    "Importing the release fence failed ({}): waiting for fences on the CPU instead",
                    vk::to_string(result));
        sync_import_ok = false;
    }
    // CPU wait: the fence signals when the display is done with the buffer.
    while (!WaitSyncFile(fd, 1000)) {
        LOG_WARNING(Render_Vulkan, "AHB swapchain: release fence of buffer {} still pending", index);
    }
    close(fd);
}

int Swapchain::ExportPresentReady() {
    const IssBionicPerf::Scope timer{IssBionicPerf::Stage::AhbExport};
    const auto [result, fd] = instance.GetDevice().getSemaphoreFdKHR({
        .semaphore = present_ready[image_index],
        .handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eSyncFd,
    });
    if (result != vk::Result::eSuccess) {
        // Without the fence the app would read a half-drawn buffer: wait for the GPU here.
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOG_ERROR(Render_Vulkan,
                      "Exporting the present-ready semaphore as a sync file failed ({}): falling "
                      "back to waiting for the GPU on every frame",
                      vk::to_string(result));
        }
        const auto idle = instance.GetDevice().waitIdle();
        ASSERT_MSG(idle == vk::Result::eSuccess, "waitIdle failed: {}", vk::to_string(idle));
        return -1;
    }
    return fd; // -1 is possible too: the semaphore was already signaled
}

bool Swapchain::Present() {
    const int fence_fd = ExportPresentReady();
    ++frame_counter;
    const int trace_index = std::exchange(capture_index, -1);
    if (trace_index >= 0) {
        const char* dir = std::getenv("TLG_DUMP_DIR"); char path[512];
        std::snprintf(path, sizeof(path), "%s/f%03d_presenter_info.txt", dir && *dir ? dir : "out/dump", trace_index);
        if (FILE* f = std::fopen(path, "wb")) {
            const auto now = std::chrono::steady_clock::now().time_since_epoch();
            std::fprintf(f, "serial=%llu\nframe=%llu\ngeneration=%u\nbuffer=%u\nsteady_ns=%lld\nahb_extent=%ux%u\nahb_crop=%u,%u,%u,%u\nsource_frame_extent=%ux%u\nahb_status=%s\n",
                (unsigned long long)capture_serial, (unsigned long long)frame_counter, generation, image_index,
                (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(now).count(), width, height,
                capture_crop[0], capture_crop[1], capture_crop[2], capture_crop[3], capture_frame_size[0], capture_frame_size[1], capture_status);
            std::fclose(f);
        }
    }
    if (client.Connected()) {
        const IssBionicPerf::Scope send_timer{IssBionicPerf::Stage::AhbSend};
        if (!client.SendFrame(generation, image_index, frame_counter, width, height, fence_fd, trace_index)) {
            PeerGone();
        }
        // The buffer is the app's until its release.
    } else {
        // Nobody shows it: the sync file is dropped, the buffer is free at once.
        free_queue.push_back(image_index);
        buffers[image_index].release_fd = -1;
    }
    if (fence_fd >= 0) {
        close(fence_fd);
    }
    buffers[image_index].acquire_imported = false;
    if (IssBionicPerf::OnPresent(g_tlg_compiles.load(std::memory_order_relaxed),
                                g_tlg_compile_ns.load(std::memory_order_relaxed))) {
        WaitDiagnostics::Report(true);
    }
    return !needs_recreation;
}

} // namespace Vulkan
#endif // __ANDROID__
