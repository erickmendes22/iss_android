// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <thread>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include <functional>

#include "tlg_copy.h"
#include "iss_bionic_perf.h"
#include "video_core/renderer_vulkan/vk_gpu_profiler.h"
#include "tlg_toggles.h"
#include "video_core/renderer_vulkan/vk_wait_diagnostics.h"
#include "common/assert.h"
#include "common/debug.h"
#include "common/thread.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "tlg_threads.h"

namespace Vulkan {

std::mutex Scheduler::submit_mutex;

Scheduler::Scheduler(const Instance& instance, bool threaded_recording)
    : instance{instance}, work_semaphore{instance}, command_pool{instance, &work_semaphore} {
    // Experimental real Vulkan secondary command recording on ARM64 only.
    // Requires RenderState formats/1x MSAA and no rendering observer (Vulkan
    // inherited occlusion queries require additional feature support).
#if defined(__ANDROID__) && defined(ISS_ANDROID_BIONIC)
    if (const char* e = std::getenv("ISS_NATIVE_PASSES_ARM64_SECONDARY");
        e && e[0] == '1') {
        native_secondary_enabled = true;
        for (auto& p : native_secondary_pools) {
            p = std::make_unique<CommandPool>(instance, &work_semaphore,
                                                vk::CommandBufferLevel::eSecondary);
        }
        std::printf("ISS Native Passes ARM64 Vulkan secondary recording: experimental on "
                    "(eligible 1x passes, no occlusion observer; no pass reuse yet)\n");
    }
#endif
    // tlg: TLG_VK_RECORD_THREAD=0 records on the calling thread.
    const char* env = std::getenv("TLG_VK_RECORD_THREAD");
    if (threaded_recording && !(env && env[0] == '0')) {
#if defined(__ANDROID__) && defined(ISS_ANDROID_BIONIC) && !TRACY_GPU_ENABLED
        const char* parallel = std::getenv("TLG_PARALLEL_RECORDING");
        if (TlgThreads::Available() >= 8 && !(parallel && *parallel == '0')) {
            parallel_pool = std::make_unique<CommandPool>(instance, &work_semaphore);
            parallel_recording = std::make_unique<ParallelRecordQueue<RecordChunk, vk::CommandBuffer>>(
                [this](unsigned lane) {
                    auto& pool = lane ? *parallel_pool : command_pool;
                    const auto buffer = pool.Commit();
                    Check(buffer.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}));
                    return buffer;
                }, [](unsigned lane) {
                    IssBionicPerf::RegisterThread(lane ? IssBionicPerf::Thread::Recorder2
                                                     : IssBionicPerf::Thread::Recorder);
                    Common::SetCurrentThreadName(lane ? "tlg:VkRecorder1" : "tlg:VkRecorder");
                });
            std::printf("GPU: 2 Vulkan recorders; at most 2 primary buffers per submit, natural pass boundaries\n");
        }
#endif
        record_chunk = AcquireChunk();
        if (!parallel_recording) {
            recorder_thread = std::jthread(std::bind_front(&Scheduler::RecorderThread, this));
        }
        recording_threaded = true;
    }
#if TRACY_GPU_ENABLED
    profiler_scope = reinterpret_cast<tracy::VkCtxScope*>(std::malloc(sizeof(tracy::VkCtxScope)));
#endif
    AllocateWorkerCommandBuffers();
    if (IssBionicPerf::Enabled()) {
        std::printf("GPU: host copy range tracking: %s\n", HostCopyRangesEnabled() ? "on" : "off");
    }
    priority_pending_ops_thread =
        std::jthread(std::bind_front(&Scheduler::PriorityPendingOpsThread, this));
}

Scheduler::~Scheduler() {
    if (parallel_recording) {
        SyncRecording();
        parallel_recording.reset();
    }
    if (recorder_thread.joinable()) {
        SyncRecording();
        recorder_thread.request_stop();
        recorder_cv.notify_all();
        recorder_thread.join();
    }
#if TRACY_GPU_ENABLED
    std::free(profiler_scope);
#endif
}

bool Scheduler::FullBarriers() {
    static const bool enabled = [] {
        const char* env = std::getenv("TLG_FULL_BARRIERS");
        const bool on = env && env[0] == '1';
        if (on) {
            // In last_run.log whatever the GPU log level, so a tester's log shows the mode.
            std::printf("GPU: full barriers on (TLG_FULL_BARRIERS=1)\n");
        }
        return on;
    }();
    return enabled;
}

void Scheduler::RecordFullBarrier(vk::CommandBuffer cmdbuf) {
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &barrier,
    });
}

void Scheduler::BeginRendering(const RenderState& new_state) {
    if (is_rendering && render_state == new_state) {
        return;
    }
    EndRendering();
    is_rendering = true;
    render_state = new_state;

    std::array<vk::RenderingAttachmentInfo, 8> color_attachments;
    for (u32 i = 0; i < render_state.num_color_attachments; ++i) {
        const auto& cb = render_state.color_attachments[i];
        color_attachments[i] = vk::RenderingAttachmentInfo{
            .imageView = cb.image_view,
            .imageLayout = cb.image_layout,
            .loadOp = cb.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = vk::ClearColorValue{.uint32 = cb.clear_value}},
        };
    }

    const auto& db = render_state.depth_stencil_attachment;
    const vk::RenderingAttachmentInfo depth_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue =
            vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{.depth = std::bit_cast<float>(
                                                                          db.clear_value[0])}},
    };
    const vk::RenderingAttachmentInfo stencil_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil =
                                         vk::ClearDepthStencilValue{.stencil = db.clear_value[1]}},
    };

    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .offset = {0, 0},
                .extent = {render_state.width, render_state.height},
            },
        .layerCount = render_state.num_layers,
        .colorAttachmentCount = render_state.num_color_attachments,
        .pColorAttachments = color_attachments.data(),
        .pDepthAttachment = db.has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment = db.has_stencil ? &stencil_attachment : nullptr,
    };

    const bool use_secondary = native_secondary_enabled && recording_threaded &&
                               !direct_mode &&
                               (!rendering_observer ||
                                rendering_observer->AllowsSecondaryRendering()) &&
                               new_state.native_eligible && !FullBarriers();
    native_secondary_current_scope = use_secondary;
    if (!recording_threaded) {
        if (FullBarriers()) {
            RecordFullBarrier(current_cmdbuf);
        }
        current_cmdbuf.beginRendering(rendering_info);
        if (rendering_observer) {
            rendering_observer->OnBeginRendering();
        }
        return;
    }
    // Secondary content must begin on the PRIMARY command buffer. Every later
    // graphics closure is directed to the matching SECONDARY recorder lane.
    const unsigned native_lane = parallel_lane;
    RecordPrimary([this, info = rendering_info, color_attachments, depth_attachment,
                   stencil_attachment, state = new_state, use_secondary,
                   native_lane](vk::CommandBuffer primary) mutable {
        info.pColorAttachments = color_attachments.data();
        if (info.pDepthAttachment) info.pDepthAttachment = &depth_attachment;
        if (info.pStencilAttachment) info.pStencilAttachment = &stencil_attachment;
        if (FullBarriers()) RecordFullBarrier(primary);
        if (use_secondary) {
            info.flags |= vk::RenderingFlagBits::eContentsSecondaryCommandBuffers;
        }
        primary.beginRendering(info);
        if (use_secondary) {
            const auto secondary = native_secondary_pools[native_lane]->Commit();
            const vk::CommandBufferInheritanceRenderingInfo rendering_inheritance = {
                .colorAttachmentCount = state.num_color_attachments,
                .pColorAttachmentFormats = state.native_color_formats.data(),
                .depthAttachmentFormat = state.native_depth_format,
                .stencilAttachmentFormat = state.native_stencil_format,
                .rasterizationSamples = state.native_samples,
            };
            const vk::CommandBufferInheritanceInfo inheritance = {
                .pNext = &rendering_inheritance,
            };
            const vk::CommandBufferBeginInfo begin = {
                .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit |
                         vk::CommandBufferUsageFlagBits::eRenderPassContinue,
                .pInheritanceInfo = &inheritance,
            };
            Check(secondary.begin(begin));
            native_secondary_scopes[native_lane] = {secondary, true};
        }
    });
    if (rendering_observer) {
        rendering_observer->OnBeginRendering();
    }
}

void Scheduler::EndRendering() {
    // tlg: is_rendering belongs to the draw recording thread (stage B) while draw packets are in
    // flight: drain first (owner hook), or a stale "not rendering" lets the next command (a
    // stage A CopyBuffer) be recorded inside the render pass stage B opens meanwhile, which
    // crashed the NVIDIA driver in the recorder thread (TLG intro; GR2 gameplay).
    CheckOwner(__builtin_return_address(0));
    if (!is_rendering) {
        return;
    }
    if (rendering_observer) {
        rendering_observer->OnEndRendering();
    }
    is_rendering = false;
    native_secondary_current_scope = false;
    const unsigned native_lane = parallel_lane;
    RecordPrimary([this, native_lane](vk::CommandBuffer primary) {
        auto& scope = native_secondary_scopes[native_lane];
        if (scope.active) {
            Check(scope.buffer.end());
            primary.executeCommands(scope.buffer);
            scope = {};
        }
        primary.endRendering();
        if (FullBarriers()) RecordFullBarrier(primary);
    });
}

void Scheduler::RecordVertexInput(
    std::span<const vk::VertexInputBindingDescription2EXT> bindings,
    std::span<const vk::VertexInputAttributeDescription2EXT> attributes) {
    CheckOwner(__builtin_return_address(0));
    static const bool enabled = [] {
#if defined(__ANDROID__) && defined(ISS_ANDROID_BIONIC)
        const char* env = std::getenv("TLG_VERTEX_INPUT_CACHE");
        const bool on = !(env && *env == '0');
        std::printf("GPU: vertex input state reuse: %s\n", on ? "on" : "off");
        return on;
#else
        return false;
#endif
    }();
    if (enabled && vertex_input_state.Matches(bindings, attributes)) {
        if (IssBionicPerf::Enabled()) {
            IssBionicPerf::vertex_input_skipped.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    ReserveRecordData(bindings.size_bytes() + attributes.size_bytes() + 128);
    const auto saved_bindings = RecordData(bindings);
    const auto saved_attributes = RecordData(attributes);
    Record([saved_bindings, saved_attributes](vk::CommandBuffer cmdbuf) {
        cmdbuf.setVertexInputEXT(saved_bindings, saved_attributes);
    });
    if (enabled) {
        vertex_input_state.Store(bindings, attributes);
    }
    if (IssBionicPerf::Enabled()) {
        IssBionicPerf::vertex_input_issued.fetch_add(1, std::memory_order_relaxed);
    }
}

void Scheduler::TraceDirectRecording(void* caller) {
    static const bool enabled = [] {
        const char* env = std::getenv("TLG_RECORDER_TRACE");
        return env && env[0] == '1';
    }();
    if (!enabled) {
        return;
    }
    static std::mutex mutex;
    static std::unordered_map<void*, u64> callers;
    static u64 calls;
    std::scoped_lock lk{mutex};
    ++callers[caller];
    if (++calls % 2000) {
        return;
    }
    std::vector<std::pair<u64, void*>> top;
    for (const auto& [address, count] : callers) {
        top.emplace_back(count, address);
    }
    std::ranges::sort(top, std::greater{});
    for (size_t i = 0; i < std::min<size_t>(top.size(), 8); ++i) {
#ifdef _WIN32
        struct { const char* dli_fname; void* dli_fbase; } info{nullptr, nullptr};
#else
        Dl_info info{};
        dladdr(top[i].second, &info);
#endif
        std::printf("Recorder sync caller: %llu x %s+0x%lx\n",
                    static_cast<unsigned long long>(top[i].first),
                    info.dli_fname ? info.dli_fname : "?",
                    static_cast<unsigned long>(reinterpret_cast<uintptr_t>(top[i].second) -
                                               reinterpret_cast<uintptr_t>(info.dli_fbase)));
    }
    callers.clear();
}

std::unique_ptr<RecordChunk> Scheduler::AcquireChunk() {
    if (parallel_recording) return parallel_recording->Acquire();
    std::scoped_lock lk{recorder_mutex};
    if (free_chunks.empty()) {
        return std::make_unique<RecordChunk>();
    }
    auto chunk = std::move(free_chunks.back());
    free_chunks.pop_back();
    return chunk;
}

void Scheduler::SignalAfterHostCopies(std::function<void()> signal) {
    CheckOwner(__builtin_return_address(0));
    if (!IsRecordingDeferred()) {
        WaitHostCopies();
        signal();
        return;
    }
    TlgCopy::FlushBatch();
    deferred_signals_issued.fetch_add(1, std::memory_order_relaxed);
    Record([signal = std::move(signal), done = deferred_signals_done,
            this, lane = parallel_lane](vk::CommandBuffer) mutable {
        AfterRecordingPrefix(lane, [signal = std::move(signal), done = std::move(done)]() mutable {
            TlgCopy::AfterCopies([signal = std::move(signal), done = std::move(done)] {
                signal();
                done->fetch_add(1, std::memory_order_release);
            });
        });
    });
    KickRecording(true);
}

void Scheduler::WaitDeferredSignals() {
    const u64 issued = deferred_signals_issued.load(std::memory_order_relaxed);
    if (deferred_signals_done->load(std::memory_order_acquire) >= issued ||
        TlgToggle::Disabled(TlgToggle::OrderedGuestWrites)) {
        return;
    }
    TlgStats::WaitTimer timer{TlgStats::host_copies_wait_ns};
    KickRecording(true);
    while (deferred_signals_done->load(std::memory_order_acquire) < issued) {
        // Helps the copy threads the signals wait for.
        TlgCopy::WaitAsync();
        std::this_thread::yield();
    }
}

bool Scheduler::HostCopyRangesEnabled() {
    static const bool enabled = [] {
#if defined(__ANDROID__) && defined(ISS_ANDROID_BIONIC)
        const char* env = std::getenv("TLG_HOST_COPY_RANGES");
        return !(env && *env == '0');
#else
        return false;
#endif
    }();
    return enabled;
}

void Scheduler::NoteHostCopySource(u64 address, u64 size) {
    if (HostCopyRangesEnabled()) host_copy_sources.Note(address, size);
}

void Scheduler::WaitHostCopiesFor(u64 address, u64 size) {
    if (HostCopyRangesEnabled() && !host_copy_sources.Overlaps(address, size)) {
        if (IssBionicPerf::Enabled()) {
            IssBionicPerf::host_copy_skipped.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    if (IssBionicPerf::Enabled()) {
        IssBionicPerf::host_copy_required.fetch_add(1, std::memory_order_relaxed);
    }
    WaitHostCopies();
}

void Scheduler::WaitHostCopies() {
    const IssBionicPerf::Scope host_timer{IssBionicPerf::Stage::HostCopies};
    // A copy registered while this wait runs must not be forgotten by its completion.
    auto* sources = HostCopyRangesEnabled() ? &host_copy_sources : nullptr;
    const u64 seen = sources ? sources->Snapshot() : 0;
    struct Retire {
        HostCopySources* sources;
        u64 seen;
        ~Retire() { if (sources) sources->RetireThrough(seen); }
    } retire{sources, seen};
    if (host_copies_done.load(std::memory_order_acquire) < host_copies_issued) {
        TlgStats::WaitTimer timer{TlgStats::host_copies_wait_ns};
        TlgStats::host_copy_waits.fetch_add(1, std::memory_order_relaxed);
        KickRecording(true);
        while (host_copies_done.load(std::memory_order_acquire) < host_copies_issued) {
            std::this_thread::yield();
        }
    }
    TlgStats::WaitTimer timer{TlgStats::copy_threads_wait_ns};
    TlgCopy::WaitAsync();
}

void Scheduler::KickRecording(bool force) {
    if (!recording_threaded) {
        return;
    }
    CheckOwner(__builtin_return_address(0));
    // Callers kick where nobody holds the raw command buffer: deferral resumes.
    direct_mode = false;
    // Batches of tens of KiB keep the queue handoff cheap relative to the work it carries.
    if (!force && full_chunks.empty() && record_chunk->Size() < 32 * 1024) {
        return;
    }
    if (full_chunks.empty() && record_chunk->Empty()) {
        return;
    }
    if (parallel_recording) {
        HandOverParallel();
        return;
    }
    bool wake;
    {
        std::scoped_lock lk{recorder_mutex};
        for (auto& chunk : full_chunks) {
            recorder_queue.push_back(std::move(chunk));
        }
        if (!record_chunk->Empty()) {
            recorder_queue.push_back(std::move(record_chunk));
        }
        wake = recorder_sleeping;
        queued_chunks.store(recorder_queue.size(), std::memory_order_release);
    }
    full_chunks.clear();
    // A busy recorder picks the new chunks up by itself: waking it is a syscall per draw.
    if (wake) {
        recorder_cv.notify_one();
    }
    if (!record_chunk) {
        record_chunk = AcquireChunk();
    }
}

void Scheduler::SyncRecording() {
    if (!recording_threaded) {
        return;
    }
    const IssBionicPerf::Scope record_timer{IssBionicPerf::Stage::RecorderSync};
    KickRecording(true);
    if (parallel_recording) {
        parallel_recording->Sync();
        current_cmdbuf = parallel_recording->Current(parallel_lane);
        return;
    }
    TlgStats::WaitTimer timer{TlgStats::sync_recording_ns};
    std::unique_lock lk{recorder_mutex};
    recorder_idle_cv.wait(lk, [this] { return recorder_queue.empty() && !recorder_busy; });
}

void Scheduler::HandOverParallel() {
    for (auto& chunk : full_chunks) {
        parallel_bytes += chunk->Size();
        parallel_recording->Push(parallel_lane, std::move(chunk));
    }
    full_chunks.clear();
    if (!record_chunk->Empty()) {
        parallel_bytes += record_chunk->Size();
        parallel_recording->Push(parallel_lane, std::move(record_chunk));
        record_chunk = AcquireChunk();
    }
}

void Scheduler::SplitRecordingAtBoundary(const RenderState& next_state) {
    CheckOwner(__builtin_return_address(0));
    if (!parallel_recording || parallel_lane || !IsRecordingDeferred() ||
        !WillBeginRendering(next_state)) return;
    size_t bytes = parallel_bytes + record_chunk->Size();
    for (const auto& chunk : full_chunks) bytes += chunk->Size();
    if (bytes < parallel_target) return;
    // This pass must end for the next draw anyway. Cut BEFORE its vertex/index,
    // descriptors, push constants and dynamic state, never in BeginRendering.
    EndRendering();
    SplitRecording();
}

void Scheduler::SplitRecording() {
    Record([queue = parallel_recording.get()](vk::CommandBuffer) { queue->CompletePrefix(); });
    HandOverParallel();
    parallel_lane = 1;
    dynamic_state.Invalidate();
    vertex_input_state.Invalidate();
}

void Scheduler::BeginParallelBatch() {
    if (!parallel_recording) return;
    if (parallel_bytes) {
        parallel_target = std::clamp(parallel_bytes / 2, size_t{64 * 1024}, size_t{2 * 1024 * 1024});
    }
    parallel_recording->Start(current_cmdbuf);
    parallel_lane = 0;
    parallel_bytes = 0;
}

std::span<const vk::CommandBuffer> Scheduler::EndRecordingBuffers() {
    unsigned count = 0;
    if (parallel_recording && parallel_lane) {
        submission_buffers[count++] = parallel_recording->First();
        Check(submission_buffers[0].end());
        if (IssBionicPerf::Enabled()) {
            IssBionicPerf::parallel_splits.fetch_add(1, std::memory_order_relaxed);
        }
    }
    Check(current_cmdbuf.end());
    submission_buffers[count++] = current_cmdbuf;
    if (parallel_recording && IssBionicPerf::Enabled()) {
        IssBionicPerf::parallel_batches.fetch_add(1, std::memory_order_relaxed);
        IssBionicPerf::parallel_deferred_host.fetch_add(
            parallel_recording->DeferredHost(), std::memory_order_relaxed);
    }
    return {submission_buffers.data(), count};
}

void Scheduler::RecorderThread(std::stop_token stoken) {
    IssBionicPerf::RegisterThread(IssBionicPerf::Thread::Recorder);
    Common::SetCurrentThreadName("tlg:VkRecorder");
    while (true) {
        // Spin briefly before sleeping: the next chunk usually follows within microseconds,
        // and a sleeping recorder costs the GPU thread a wake-up syscall per kick. With few
        // hardware threads (Steam Deck: 8) the spin would take time from guest threads.
        static const auto spin_time = std::chrono::microseconds(
            TlgThreads::Available() >= 12 ? 200 : 20);
        const auto spin_until = std::chrono::steady_clock::now() + spin_time;
        for (u32 spins = 1; queued_chunks.load(std::memory_order_acquire) == 0; ++spins) {
            Common::HostPause();
            // The clock is read every 256 pauses, not per iteration.
            if (!(spins & 255) &&
                (stoken.stop_requested() || std::chrono::steady_clock::now() >= spin_until)) {
                break;
            }
        }
        std::unique_ptr<RecordChunk> chunk;
        {
            std::unique_lock lk{recorder_mutex};
            recorder_sleeping = true;
            recorder_cv.wait(lk, stoken, [this] { return !recorder_queue.empty(); });
            recorder_sleeping = false;
            if (recorder_queue.empty()) {
                return; // stop requested
            }
            chunk = std::move(recorder_queue.front());
            recorder_queue.pop_front();
            queued_chunks.store(recorder_queue.size(), std::memory_order_release);
            recorder_busy = true;
        }
        // current_cmdbuf only changes after SyncRecording(), which waits for this thread.
        chunk->Execute(current_cmdbuf);
        {
            std::scoped_lock lk{recorder_mutex};
            free_chunks.push_back(std::move(chunk));
            recorder_busy = false;
            if (recorder_queue.empty()) {
                recorder_idle_cv.notify_all();
            }
        }
    }
}

void Scheduler::Flush(SubmitInfo& info) {
    // When flushing, we only send data to the driver; no waiting is necessary.
    SubmitExecution(info);
}

void Scheduler::Flush() {
    SubmitInfo info{};
    Flush(info);
}

void Scheduler::Finish(std::source_location caller) {
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    const bool diagnose = WaitDiagnostics::Enabled();
    const auto start = diagnose ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    SubmitInfo info{};
    SubmitExecution(info);
    const u64 submit_ns = diagnose ? std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count() : 0;
    Wait(presubmit_tick, caller, submit_ns);
#ifdef ISS_ANDROID_BIONIC
    // The explicit wait advanced the cached timeline; reclaim without another driver query.
    PopPendingOperations();
#endif
}

void Scheduler::WaitSubmitted(std::source_location caller) {
    const u64 tick = CurrentTick();
    if (tick > 1) {
        Wait(tick - 1, caller);
    }
}

void Scheduler::Wait(u64 tick, std::source_location caller, u64 submit_ns) {
    const bool diagnose = WaitDiagnostics::Enabled();
    auto start = diagnose ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    if (tick >= work_semaphore.CurrentTick()) {
        // Make sure we are not waiting for the current tick without signalling
        SubmitInfo info{};
        Flush(info);
    }
    TlgStats::WaitTimer timer{TlgStats::tick_wait_ns};
    const auto submitted = diagnose ? std::chrono::steady_clock::now() : start;
    work_semaphore.Wait(tick, caller);
    if (diagnose) {
        const auto completed = std::chrono::steady_clock::now();
        WaitDiagnostics::Get().Add(caller.file_name(), caller.line(),
            submit_ns + std::chrono::duration_cast<std::chrono::nanoseconds>(submitted - start).count(),
            std::chrono::duration_cast<std::chrono::nanoseconds>(completed - submitted).count());
    }
}

void Scheduler::PopPendingOperations() {
    if (num_pending_ops.load(std::memory_order_acquire) == 0) {
        return; // every draw comes here
    }
    std::unique_lock lk(pending_ops_mutex);
    if (pending_ops.empty()) {
        return;
    }
#ifndef ISS_ANDROID_BIONIC
    // Legacy platforms poll at most once per 32 draw/dispatch calls when a callback waits.
    // Bionic uses completion cached by the submission window, waits and resource-pool misses.
    if (!work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        if ((++pending_polls & 31) != 0 && !TlgToggle::Disabled(TlgToggle::PendingPollLimit)) {
            return;
        }
        work_semaphore.Refresh();
    }
#endif
    // tlg: a callback can submit (the fault buffer's ObtainBuffer, which may upload in parts or
    // wait for the GPU to free memory) and so come back here on this thread while it is still
    // the front op: running it again, or popping it, would corrupt the queue. The outer loop
    // carries on once the callback returns.
    if (popping_pending_ops) {
        return;
    }
    popping_pending_ops = true;
    while (!pending_ops.empty() && work_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        pending_ops.front().callback();
        pending_ops.pop();
        num_pending_ops.fetch_sub(1, std::memory_order_release);
    }
    popping_pending_ops = false;
}

void Scheduler::AllocateWorkerCommandBuffers() {
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };

    current_cmdbuf = command_pool.Commit();
    Check(current_cmdbuf.begin(begin_info));

    // Invalidate dynamic state so it gets applied to the new command buffer.
    dynamic_state.Invalidate();
    vertex_input_state.Invalidate();
    BeginParallelBatch();

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        static const auto scope_loc =
            GPU_SCOPE_LOCATION("Guest Frame", MarkersPalette::GpuMarkerColor);
        new (profiler_scope) tracy::VkCtxScope{profiler_ctx, &scope_loc, current_cmdbuf, true};
    }
#endif
}

void Scheduler::SubmitExecution(SubmitInfo& info) {
    const IssBionicPerf::Scope submit_timer{IssBionicPerf::Stage::Submission};
    std::scoped_lock lk{submit_mutex};
    const u64 signal_value = work_semaphore.NextTick();
#ifdef ISS_ANDROID_BIONIC
    // Keep up to eight submitted command buffers in flight. Unlike a counter query after
    // every submit, Wait first checks cached completion, so one query can retire a batch.
    // Bound the backlog before reusing command buffers/descriptors or running callbacks.
    constexpr u64 MaxInFlightSubmissions = 8;
    if (signal_value > MaxInFlightSubmissions) {
        work_semaphore.Wait(signal_value - MaxInFlightSubmissions);
    }
#endif
    work_since_submit.store(0, std::memory_order_relaxed);

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        profiler_scope->~VkCtxScope();
        TracyVkCollect(profiler_ctx, current_cmdbuf);
    }
#endif

    if (on_submit) {
        on_submit(info);
    }

    EndRendering();
    if (auto* profiler = GpuProfiler::Get(); profiler && profiler->Records(this)) {
        // Until the next submission's first timestamp: mostly the GPU waiting for it.
        profiler->Mark(0x5B317ull, [] { return std::string{"(between submissions: GPU idle)"}; });
    }
    SyncRecording();
    // Guest memory copies into staging read by this submission (copy threads).
    WaitHostCopies();
    FlushHostReadBarrier();
    const auto command_buffers = EndRecordingBuffers();

    const vk::Semaphore timeline = work_semaphore.Handle();
    info.AddSignal(timeline, signal_value);

    static constexpr std::array<vk::PipelineStageFlags, 2> wait_stage_masks = {
        vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
    };

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = info.num_wait_semas,
        .pWaitSemaphoreValues = info.wait_ticks.data(),
        .signalSemaphoreValueCount = info.num_signal_semas,
        .pSignalSemaphoreValues = info.signal_ticks.data(),
    };

    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = info.num_wait_semas,
        .pWaitSemaphores = info.wait_semas.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = static_cast<u32>(command_buffers.size()),
        .pCommandBuffers = command_buffers.data(),
        .signalSemaphoreCount = info.num_signal_semas,
        .pSignalSemaphores = info.signal_semas.data(),
    };

    ImGui::Core::TextureManager::Submit();
    vk::Result submit_result;
    {
        const IssBionicPerf::Scope driver_timer{IssBionicPerf::Stage::DriverSubmit};
        submit_result = instance.GetGraphicsQueue().submit(submit_info, info.fence);
    }
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");

#ifndef ISS_ANDROID_BIONIC
    work_semaphore.Refresh();
#endif
    AllocateWorkerCommandBuffers();

    // Apply pending operations
    PopPendingOperations();
}

void Scheduler::FlushHostReadBarrier() {
    if (!std::exchange(host_read_barrier, false)) return;
    const vk::MemoryBarrier2 barrier{
        .srcStageMask = std::exchange(host_read_src_stages, {}),
        .srcAccessMask = std::exchange(host_read_src_access, {}),
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    current_cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1, .pMemoryBarriers = &barrier,
    });
}

void Scheduler::PriorityPendingOpsThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuSchedPriorityPendingOpsRunner");

    while (!stoken.stop_requested()) {
        PendingOp op;
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops_cv.wait(lk, stoken,
                                         [this] { return !priority_pending_ops.empty(); });
            if (stoken.stop_requested()) {
                break;
            }

            op = std::move(priority_pending_ops.front());
            priority_pending_ops.pop();
        }

        work_semaphore.Wait(op.gpu_tick);
        if (stoken.stop_requested()) {
            break;
        }

        op.callback();
    }
}

void DynamicState::Commit(const Instance& instance, const vk::CommandBuffer& cmdbuf) {
    // A null command buffer only updates the dirty flags, exactly as recording would.
    CommitWith(instance.IsDepthBoundsSupported(), instance.IsDynamicColorWriteMaskSupported(),
               instance.IsAttachmentFeedbackLoopLayoutSupported(), [&](auto&& command) {
                   if (cmdbuf) {
                       command(cmdbuf);
                   }
               });
}

} // namespace Vulkan
