// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <cstdio>
#include <cstring>
#include <chrono>
#include <array>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <bit>
#include <cstdlib>
#include <magic_enum/magic_enum.hpp>
#include "tlg_copy.h"
#include "tlg_toggles.h"
#include "common/alignment.h"
#include "core/memory.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/buffer_cache/memory_tracker.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/texture_cache.h"

#include <vk_mem_alloc.h>

namespace VideoCore {

static constexpr size_t GDS_BUFFER_SIZE = 64_KB;
static constexpr size_t STREAM_BUFFER_SIZE = 128_MB;
// Memory is made resident in aligned granules of this size. Sparse binds get slower the more is
// already bound on some drivers, so it pays to bind a little ahead in few larger operations
// rather than a block at a time as the game streams data in.
static constexpr u64 RESIDENCY_GRANULE_SIZE = 16_MB;
static constexpr u64 RESIDENCY_CHUNK_SIZE = 256_MB;

static constexpr auto ARENA_USAGE =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eUniformBuffer | vk::BufferUsageFlagBits::eStorageBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eShaderDeviceAddress;

std::optional<u32> FindMemoryType(const vk::PhysicalDeviceMemoryProperties& properties,
                                  vk::MemoryPropertyFlags wanted, u32 memory_type_bits) {
    for (u32 i = 0; i < properties.memoryTypeCount; ++i) {
        if (((memory_type_bits >> i) & 1) == 0) {
            continue;
        }
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if ((flags & wanted) == wanted) {
            return i;
        }
    }
    return std::nullopt;
}

BufferCache::BufferCache(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_,
                         Vulkan::Runtime& runtime_, AmdGpu::Liverpool* liverpool_,
                         TextureCache& texture_cache_, PageManager& tracker)
    : instance{instance_}, scheduler{scheduler_}, runtime{runtime_},
      staging_pool{runtime_.GetStagingPool()}, liverpool{liverpool_},
      memory{Core::Memory::Instance()}, texture_cache{texture_cache_},
      memory_tracker{std::make_unique<MemoryTracker>(tracker)},
      stream_buffer{instance, scheduler, MemoryType::Stream, STREAM_BUFFER_SIZE},
      gds_buffer{instance, 0, GDS_BUFFER_SIZE, MemoryType::Stream, "GDS Buffer"},
      memory_semaphore{instance} {
    std::printf("GPU: buffer deferred readbacks: %s (mode=%d, publication=%s)\n",
                DeferredReadbackEnabled() ? "enabled" : "disabled",
                static_cast<int>(EmulatorSettings.GetReadbacksMode()),
                EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Relaxed
                    ? "cache owner" : "guest thread");
#if defined(__ANDROID__) && defined(ISS_ANDROID_BIONIC)
    std::printf("ISS hybrid V6 (V2 baseline): read protection=%s, staged buffer copies=%s, eager fence copies=%s, selective backoff=%s\n",
                RegionManager::HybridReadProtectionEnabled() ? "on" : "off",
                std::getenv("TLG_MAPPED_READBACK") && std::getenv("TLG_MAPPED_READBACK")[0] == '0' ? "on" : "off",
                std::getenv("TLG_EAGER_READBACK") && std::getenv("TLG_EAGER_READBACK")[0] == '1' ? "on" : "off",
                std::getenv("TLG_ISS_EAGER_BACKOFF_63") && std::getenv("TLG_ISS_EAGER_BACKOFF_63")[0] == '1' ? "on" : "off");
    std::printf("ISS experimental hybrid read protection: %s (readbacks=%d)\n",
                RegionManager::HybridReadProtectionEnabled() ? "on" : "off",
                static_cast<int>(EmulatorSettings.GetReadbacksMode()));
#endif
    // tlg: TLG_MAX_BUFFER_MB lowers the driver's buffer size limit as the arenas see it
    // (diagnostics: reproduces AMD on Windows 2048, RADV 4095 on any GPU).
    u64 max_buffer = instance.GetMaxBufferSize();
    if (const char* forced = std::getenv("TLG_MAX_BUFFER_MB")) {
        if (const u64 mb = std::strtoull(forced, nullptr, 10)) {
            max_buffer = std::min<u64>(max_buffer, mb << 20);
        }
    }
    // tlg: with a limit under two full pages (AMD on Windows 2 GiB, RADV ~4 GiB, Intel), pages
    // are the smallest size and an arena joins as many as the limit allows: one inFAMOUS request
    // of ~2.4 GiB (the cutscene before the graffiti) crossed three 1 GiB pages and stopped the
    // game ("cannot span more than two VA arena pages", iss_pc #18).
    if ((u64{2} << arena_page_bits) > max_buffer) {
        arena_page_bits = MIN_ARENA_PAGE_BITS;
    }
    arena_page_size = u64{1} << arena_page_bits;
    max_arena_pages = std::clamp<u64>(max_buffer >> arena_page_bits, 2, 16);
    num_arena_pages = u64{1} << (ADDRESS_SPACE_BITS - arena_page_bits);
    std::printf("GPU: buffer arenas of %llu MiB, up to %llu pages (driver buffer limit %llu MiB)\n",
                static_cast<unsigned long long>(arena_page_size >> 20),
                static_cast<unsigned long long>(max_arena_pages),
                static_cast<unsigned long long>(max_buffer >> 20));
    const vk::BufferCreateInfo probe_ci = {
        .flags =
            vk::BufferCreateFlagBits::eSparseBinding | vk::BufferCreateFlagBits::eSparseResidency,
        .size = arena_page_size,
        .usage = ARENA_USAGE,
        .sharingMode = vk::SharingMode::eExclusive,
    };
    const vk::DeviceBufferMemoryRequirements req_info = {
        .pCreateInfo = &probe_ci,
    };
    const auto device = instance.GetDevice();
    const auto reqs = device.getBufferMemoryRequirements(req_info).memoryRequirements;
    block_size = Common::AlignUp(std::max<u64>(reqs.alignment, MIN_BLOCK_SIZE), reqs.alignment);
    ASSERT_MSG(std::popcount(block_size) == 1, "Sparse block size {} is not a power of 2",
               block_size);
    block_shift = std::bit_width(block_size) - 1;
    blocks_per_arena_page = arena_page_size / block_size;
    blocks_per_arena_page_shift = arena_page_bits - block_shift;
    arena_memory_type_index =
        FindMemoryType(instance.GetMemoryProperties(), vk::MemoryPropertyFlagBits::eDeviceLocal,
                       reqs.memoryTypeBits)
            .value();
    const auto residency_flags =
        instance.GetMemoryProperties().memoryTypes[arena_memory_type_index].propertyFlags;
    residency_coherent = bool(residency_flags & vk::MemoryPropertyFlagBits::eHostCoherent);
    mapped_readbacks_enabled = MappedReadbackPolicy() &&
        bool(residency_flags & vk::MemoryPropertyFlagBits::eHostVisible) &&
        (residency_coherent || instance.NonCoherentAtomSize() <= 4096);
    std::printf("GPU: mapped arena readback: %s (host-visible=%d coherent=%d atom=%llu)\n",
                mapped_readbacks_enabled ? "on" : "off",
                bool(residency_flags & vk::MemoryPropertyFlagBits::eHostVisible),
                residency_coherent, static_cast<unsigned long long>(instance.NonCoherentAtomSize()));
    // Mapped Relaxed needs a HOST_READ barrier for direct arena mapping.
    // Hybrid staging needs the write notification for snapshot invalidation,
    // but GPU->staging transfers already provide host visibility at their own fence.
    const bool hybrid_stage_tracking = RegionManager::HybridReadProtectionEnabled();
    if (mapped_readbacks_enabled || hybrid_stage_tracking) {
        runtime.SetBufferWriteHook([](void* context, VAddr address, u64 size, u64 tick) {
            static_cast<BufferCache*>(context)->NoteDeviceWrite(address, size, tick);
        }, this, mapped_readbacks_enabled);
    }

    const u64 bda_pagetable_size =
        (blocks_per_arena_page * num_arena_pages) * sizeof(vk::DeviceAddress);
    fault_manager = std::make_unique<FaultManager>(instance, scheduler, *this, block_shift,
                                                   blocks_per_arena_page * num_arena_pages);
    bda_pagetable_buffer = std::make_unique<Buffer>(
        instance, 0, bda_pagetable_size, MemoryType::DeviceLocal, "BDA Page Table Buffer");
    runtime.FillBuffer(bda_pagetable_buffer.get(), 0u, bda_pagetable_size, 0u);
}

BufferCache::~BufferCache() {
    runtime.SetBufferWriteHook(nullptr, nullptr);
    for (const auto& [allocation, mapping] : residency_mappings) {
        if (mapping.data) instance.GetDevice().unmapMemory(allocation);
    }
}

void BufferCache::InvalidateMemory(VAddr device_addr, u64 size, bool assume_locks) {
    memory_tracker->InvalidateRegion(device_addr, size, [this, device_addr, size, assume_locks] {
        ReadMemory(device_addr, size, true, assume_locks);
    });
}

// tlg: the game fills its per-frame buffers (constants, skinning output) sequentially and
// every 4 KiB page cost a protection fault (~110k/s in the test scene, a fifth of each
// render worker's time in the kernel). A fault unprotects the aligned window around it instead;
// pages marked CPU-modified without being written only cost an upload when bound.
// Item: context = BufferCache, source = guest address, destination = host pointer, size,
// extra = the Buffer whose mapping holds the destination (flushed after the copy).
void BufferCache::RunGuestCopy(const TlgCopy::Item& item) {
    auto* cache = static_cast<BufferCache*>(item.context);
    auto* dst = reinterpret_cast<u8*>(item.destination);
    cache->memory->CopySparseMemory(item.source, dst, item.size);
    auto* buffer = reinterpret_cast<Buffer*>(item.extra);
    buffer->Flush(dst - buffer->mapped_data.data(), item.size);
}

// Small guest copies run on the recording thread (it spins for work: no wakeup, and it is
// idle most of the time); PoolSmallCopies (toggle 524288) batches them for the copy threads.
void BufferCache::SmallGuestCopy(const TlgCopy::Item& item) {
    if (Vulkan::Scheduler::HostCopyRangesEnabled() && !scheduler.IsRecordingDeferred()) {
        // In direct mode the snapshot must precede the commands that consume it and any
        // subsequent WRITE_DATA of its source; a per-thread batch could run too late.
        item.run(item);
        return;
    }
    if (scheduler.IsRecordingDeferred() && !TlgToggle::Disabled(TlgToggle::PoolSmallCopies)) {
        scheduler.RecordHostCopy([item] { item.run(item); });
    } else {
        TlgCopy::QueueCopy(item);
    }
    scheduler.NoteHostCopySource(item.source, item.size);
}

void BufferCache::ExtendWriteFault(VAddr device_addr) {
    static const u64 window = [] {
        const char* env = std::getenv("TLG_FAULT_WINDOW");
        const u64 kib = env ? std::strtoull(env, nullptr, 10) : 256;
        return std::bit_ceil(std::clamp<u64>(kib, 4, 1024)) * 1024;
    }();
    if (window <= TRACKER_BYTES_PER_PAGE || TlgToggle::Disabled(TlgToggle::FaultWindow)) {
        return;
    }
    memory_tracker->ExtendWriteFault(Common::AlignDown(device_addr, window), window);
}

// tlg: TLG_READBACK_STATS=1: every 5 s, how long CPU read/write faults that need GPU data spend
// waiting for the GPU command thread to reach them (queue) and inside the download (flush).
namespace {
struct ReadbackStats {
    std::atomic<u64> count{0}, queue_ns{0}, flush_ns{0}, finish_ns{0}, bytes{0};
    /// Deferred readbacks: recorded, joined, applied by guest threads, stale (fell back to the
    /// download), guest time from the request to the copy applied, bytes copied.
    std::atomic<u64> deferred{0}, joined{0}, applied{0}, stale{0}, deferred_ns{0}, deferred_bytes{0};
    std::atomic<u64> shared_completions{0};
    std::atomic<u64> deferred_retries{0}, retry_applied{0};
    std::atomic<u64> cpu_store_applied{0}, cpu_store_fallback{0},
        cpu_store_coverage_rejected{0}, cpu_store_backing_rejected{0};
    std::atomic<u64> cpu_merge_applied{0}, cpu_merge_bytes{0}, cpu_merge_rejected{0};
    std::atomic<u64> mapped_scheduled{0}, mapped_applied{0}, mapped_bytes{0},
        mapped_current{0}, mapped_unavailable{0}, mapped_stale{0}, mapped_inline{0},
        mapped_submitted{0};
    std::atomic<u64> mapped_partial{0}, mapped_partial_kept{0}, mapped_partial_retry{0};
    std::atomic<u64> mapped_prefetch_narrowed{0}, mapped_wider_wait_bypassed{0},
        mapped_coherent_skipped{0};
    /// Eager copies: pages copied, used by a fault, gone stale unread, skipped for backoff.
    std::atomic<u64> eager_copied{0}, eager_used{0}, eager_stale{0}, eager_skipped{0};
    std::atomic<s64> window_start{0};
};
ReadbackStats& Readbacks() {
    static ReadbackStats stats;
    return stats;
}
bool ReadbackStatsEnabled() {
    static const bool enabled = [] { const char* e = std::getenv("TLG_READBACK_STATS"); return e && *e == '1'; }();
    return enabled;
}
s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
void ReportReadbacks() {
    auto& r = Readbacks();
    const s64 now = NowNs();
    s64 start = r.window_start.load();
    if (!start) {
        r.window_start.compare_exchange_strong(start, now);
        return;
    }
    if (now - start < 5'000'000'000 || !r.window_start.compare_exchange_strong(start, now)) {
        return;
    }
    const u64 n = r.count.exchange(0);
    std::printf("Readback stats (5 s): %llu downloads, queue %.1f ms, flush %.1f ms (GPU finish %.1f ms), %.1f MB\n",
                static_cast<unsigned long long>(n), r.queue_ns.exchange(0) / 1e6, r.flush_ns.exchange(0) / 1e6,
                r.finish_ns.exchange(0) / 1e6, r.bytes.exchange(0) / 1048576.0);
    std::printf("  deferred: %llu recorded, %llu joined, %llu applied, %llu stale (downloaded), "
                "guest wait %.1f ms, %.1f MB; eager: %llu pages copied, %llu used, %llu stale unread, "
                "%llu skipped (backoff)\n",
                static_cast<unsigned long long>(r.deferred.exchange(0)),
                static_cast<unsigned long long>(r.joined.exchange(0)),
                static_cast<unsigned long long>(r.applied.exchange(0)),
                static_cast<unsigned long long>(r.stale.exchange(0)), r.deferred_ns.exchange(0) / 1e6,
                r.deferred_bytes.exchange(0) / 1048576.0,
                static_cast<unsigned long long>(r.eager_copied.exchange(0)),
                static_cast<unsigned long long>(r.eager_used.exchange(0)),
                static_cast<unsigned long long>(r.eager_stale.exchange(0)),
                static_cast<unsigned long long>(r.eager_skipped.exchange(0)));
    std::printf("  relaxed shared completions: %llu GPU wait/publication leaders\n",
                static_cast<unsigned long long>(r.shared_completions.exchange(0)));
    std::printf("  mapped readbacks: %llu scheduled, %llu applied, %.1f MB; "
                "fallback current/unknown %llu, mapping %llu, stale %llu\n",
                static_cast<unsigned long long>(r.mapped_scheduled.exchange(0)),
                static_cast<unsigned long long>(r.mapped_applied.exchange(0)),
                r.mapped_bytes.exchange(0) / 1048576.0,
                static_cast<unsigned long long>(r.mapped_current.exchange(0)),
                static_cast<unsigned long long>(r.mapped_unavailable.exchange(0)),
                static_cast<unsigned long long>(r.mapped_stale.exchange(0)));
    std::printf("  mapped ready on owner: %llu published without guest wait/publication round trip\n",
                static_cast<unsigned long long>(r.mapped_inline.exchange(0)));
    std::printf("  mapped current submitted: %llu without a staging copy; deferred retries: "
                "%llu requested, %llu applied\n",
                static_cast<unsigned long long>(r.mapped_submitted.exchange(0)),
                static_cast<unsigned long long>(r.deferred_retries.exchange(0)),
                static_cast<unsigned long long>(r.retry_applied.exchange(0)));
    std::printf("  mapped partial: %llu snapshots, %llu valid bytes kept, %llu invalid bytes restored\n",
                static_cast<unsigned long long>(r.mapped_partial.exchange(0)),
                static_cast<unsigned long long>(r.mapped_partial_kept.exchange(0)),
                static_cast<unsigned long long>(r.mapped_partial_retry.exchange(0)));
    std::printf("  mapped wait scope: %llu prefetches narrowed, %llu wider pending waits bypassed, "
                "%llu coherent requests skipped\n",
                static_cast<unsigned long long>(r.mapped_prefetch_narrowed.exchange(0)),
                static_cast<unsigned long long>(r.mapped_wider_wait_bypassed.exchange(0)),
                static_cast<unsigned long long>(r.mapped_coherent_skipped.exchange(0)));
    std::printf("  CPU write merge: %llu applied, %.2f MB, %llu rejected\n",
                static_cast<unsigned long long>(r.cpu_merge_applied.exchange(0)),
                r.cpu_merge_bytes.exchange(0) / 1048576.0,
                static_cast<unsigned long long>(r.cpu_merge_rejected.exchange(0)));
    std::printf("  native store overwrite: %llu applied, %llu normal-readback fallbacks "
                "(coverage %llu, backing %llu)\n",
                static_cast<unsigned long long>(r.cpu_store_applied.exchange(0)),
                static_cast<unsigned long long>(r.cpu_store_fallback.exchange(0)),
                static_cast<unsigned long long>(r.cpu_store_coverage_rejected.exchange(0)),
                static_cast<unsigned long long>(r.cpu_store_backing_rejected.exchange(0)));
    std::fflush(stdout);
}
} // namespace

std::tuple<const Buffer*, VAddr, VAddr> BufferCache::GetReadbackWindow(VAddr device_addr,
                                                                       u64 size) {
    const u32 first_block = device_addr >> block_shift;
    const u32 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);

    // GPU-modified ranges come as many small scattered islands,
    // so the download is widened to a window around the request.
    // tlg: TLG_DOWNLOAD_WINDOW_KB sizes it (default 512). Each download waits for the GPU;
    // a game that writes records across megabytes the GPU also updates (inFAMOUS particles)
    // otherwise faults and waits once per window, many times per frame.
    static const u64 WindowSize = [] {
        const char* e = std::getenv("TLG_DOWNLOAD_WINDOW_KB");
        const u64 kb = e && *e ? std::strtoull(e, nullptr, 10) : 512;
        return std::max<u64>(kb, 64) * 1024;
    }();
    const VAddr arena_end = arena->cpu_addr + arena->size_bytes;
    const VAddr window_start =
        std::max<VAddr>(Common::AlignDown(device_addr, WindowSize), arena->cpu_addr);
    const VAddr window_end = std::min<VAddr>(
        std::max<VAddr>(window_start + WindowSize, device_addr + size), arena_end);
    return {arena, window_start, window_end};
}

void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks,
                             const std::function<bool()>& prepare) {
    const s64 queued_at = ReadbackStatsEnabled() ? NowNs() : 0;
    // A native store proof and its ordinary readback share the initial owner command.
    // Never repeat the proof on a stale retry or synchronous fallback.
    bool prepared = false, handled = false;
    const auto prepare_once = [&] {
        if (!prepared) {
            prepared = true;
            handled = prepare && prepare();
        }
        return handled;
    };
    const auto flush_request = [this, device_addr, size, is_write, queued_at, &prepare_once] {
        if (prepare_once()) return;
        const s64 started_at = queued_at ? NowNs() : 0;
        const auto [arena, window_start, window_end] = GetReadbackWindow(device_addr, size);
        DownloadMemory(arena, window_start, window_end - window_start);
        if (is_write) {
            memory_tracker->MarkRegionAsCpuModified(device_addr, size);
        }
        if (queued_at) {
            auto& r = Readbacks();
            r.count.fetch_add(1);
            r.queue_ns.fetch_add(started_at - queued_at);
            r.flush_ns.fetch_add(NowNs() - started_at);
            ReportReadbacks();
        }
    };
    if (assume_locks) {
        // A GPU-side thread touched the memory itself: it has to wait for the GPU.
        flush_request();
    } else if (DeferredReadbackEnabled() && !TlgToggle::Disabled(TlgToggle::DeferredReadback)) {
        // Keep stale Relaxed requests asynchronous for one bounded retry, rather
        // than immediately blocking the cache owner in DownloadMemory/Finish.
        unsigned max_attempts = 1;
#if defined(__ANDROID__) && defined(ISS_ANDROID_BIONIC)
        static const bool retry_enabled = [] {
            const char* env = std::getenv("TLG_READBACK_RETRY");
            return !(env && *env == '0');
        }();
        if (retry_enabled && EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Relaxed) {
            max_attempts = 2;
        }
#endif
        for (unsigned attempt = 0; attempt < max_attempts; ++attempt) {
            if (attempt && queued_at) Readbacks().deferred_retries.fetch_add(1);
            std::shared_ptr<DeferredReadback> readback;
            liverpool->SendCommand<true, true>([&] {
                if (prepare_once()) return;
                readback = StartDeferredReadback(device_addr, size);
                if (!readback && is_write) {
                    // Nothing to copy back, as when DownloadMemory finds nothing.
                    memory_tracker->MarkRegionAsCpuModified(device_addr, size);
                }
            });
            if (!readback) return;
            bool applied = false;
            if (readback) {
                if (EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Relaxed || readback->mapped) {
                    // One GPU wait and owner publication per shared snapshot, not per fault.
                    // Followers sleep in call_once without holding cache/region locks. The
                    // command processor never enters this flag (inline faults use flush_request).
                    std::call_once(readback->relaxed_completion, [&] {
                        // StartDeferredReadback may already have published a completed mapped
                        // submission on the owner. Avoid a second command-queue round trip.
                        if (readback->applied.load(std::memory_order_acquire)) return;
                        if (ReadbackStatsEnabled()) {
                            Readbacks().shared_completions.fetch_add(1, std::memory_order_relaxed);
                        }
                        // A known invalid snapshot cannot help this request. Retry on
                        // the owner instead of waiting its obsolete tick/publication.
                        if (readback->stale.load(std::memory_order_acquire) ||
                            readback->recovered.load(std::memory_order_acquire)) return;
                        scheduler.GetWorkSemaphore()->Wait(readback->tick);
                        if (readback->stale.load(std::memory_order_acquire) ||
                            readback->recovered.load(std::memory_order_acquire)) return;
                        liverpool->SendCommand<true, true>([&] {
                            FinishDeferredReadback(*readback, false);
                        });
                    });
                    applied = readback->applied.load(std::memory_order_acquire);
                    if (applied && readback->partial &&
                        memory_tracker->IsRegionGpuModified(device_addr, size)) {
                        // Other pages were published, but this request still needs newer data.
                        // Retry inside this fault, rather than returning to fault a second time.
                        applied = false;
                    }
                    if (applied && is_write) {
                        // As in Precise, this transition holds the region lock and checks the
                        // GPU bit again. A newer GPU write keeps the page protected and the
                        // guest store faults again; it cannot unprotect newer GPU data.
                        memory_tracker->MarkRegionAsCpuModifiedUnlessGpu(device_addr, size);
                    }
                } else {
                    applied = FinishDeferredReadback(*readback, true);
                    if (applied && is_write) {
                        memory_tracker->MarkRegionAsCpuModifiedUnlessGpu(device_addr, size);
                    }
                }
            }
            if (readback && !applied) {
                // Recover/recollect on the owner and share the replacement snapshot.
                // The newer write may now be submitted and eligible for a mapped read.
                // Bound retries so a page being rewritten continuously still progresses.
                if (attempt + 1 < max_attempts) continue;
                if (queued_at) {
                    Readbacks().stale.fetch_add(1);
                }
                liverpool->SendCommand<true, true>(flush_request);
            } else if (readback) {
                if (queued_at) {
                    Readbacks().deferred_ns.fetch_add(NowNs() - queued_at);
                    Readbacks().applied.fetch_add(1);
                    if (attempt) Readbacks().retry_applied.fetch_add(1);
                    ReportReadbacks();
                }
            }
            return;
        }
    } else {
        liverpool->SendCommand<true, true>(flush_request);
    }
}

void BufferCache::ReleaseReadProtection(VAddr device_addr, u64 size) {
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
}

u64 BufferCache::CollectDownloads(const Buffer* arena, VAddr device_addr, u64 size,
                                  boost::container::small_vector<vk::BufferCopy, 8>& copies) {
    u64 total_size_bytes = 0;
    const VAddr arena_base = arena->cpu_addr;
    memory_tracker->ForEachDownloadRange<false>(device_addr, size, [&](u64 address, u64 size) {
        const auto add_download = [&](VAddr start, VAddr end) {
            const u64 new_offset = start - arena_base;
            const u64 new_size = end - start;
            copies.push_back(vk::BufferCopy{
                .srcOffset = new_offset,
                .dstOffset = total_size_bytes,
                .size = new_size,
            });
            // Align up to avoid cache conflicts
            constexpr u64 align = 64ULL;
            constexpr u64 mask = ~(align - 1ULL);
            total_size_bytes += (new_size + align - 1) & mask;
        };
        gpu_modified_ranges.ForEachInRange(address, size, add_download);
        gpu_modified_ranges.Subtract(address, size);
    });
    return total_size_bytes;
}

void BufferCache::DownloadMemory(const Buffer* arena, VAddr device_addr, u64 size) {
    // Copies back in flight over this memory took their ranges out of gpu_modified_ranges:
    // applied, or put back when stale, before those are looked at.
    SettleDeferredReadbacks(device_addr, device_addr + size);
    if (eager_count.load(std::memory_order_acquire)) {
        std::scoped_lock lock{eager_mutex};
        ApplyEagerSubtractsLocked();
    }
    FlushMergedWrites(arena, device_addr, size); // merged CPU bytes precede the download
    boost::container::small_vector<vk::BufferCopy, 8> copies;
    const u64 total_size_bytes = CollectDownloads(arena, device_addr, size, copies);
    if (total_size_bytes == 0) {
        return;
    }
    const VAddr arena_base = arena->cpu_addr;
    const auto download = staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached);
    for (auto& copy : copies) {
        copy.dstOffset += download.offset;
    }
    runtime.CopyBuffer(arena, download.buffer, copies);
    const s64 finish_at = ReadbackStatsEnabled() ? NowNs() : 0;
    scheduler.Finish();
    if (finish_at) {
        Readbacks().finish_ns.fetch_add(NowNs() - finish_at);
        Readbacks().bytes.fetch_add(total_size_bytes);
    }

    download.buffer->Invalidate(download.offset, download.size);
    for (const auto& copy : copies) {
        auto* dst_addr = std::bit_cast<u8*>(arena_base + copy.srcOffset);
        memory->TryWriteBacking(dst_addr, download.mapped + (copy.dstOffset - download.offset),
                                copy.size);
    }
    memory_tracker->UnmarkRegionAsGpuModified(device_addr, size);
}

namespace {
// tlg: TLG_BUFFER_STATS=1 — how buffer bindings reach the GPU, by guest region (256 MiB),
// printed every 5 s: small read-only copies into the stream buffer, arena bindings, and the
// bytes those re-upload after CPU writes. Input for engine-level short paths.
struct BufferStats {
    struct Region {
        u64 stream_count{}, stream_bytes{}, arena_count{}, arena_bytes{}, upload_bytes{};
    };
    std::map<u64, Region> regions;
    /// Hot window: frame of the last binding per 64 KiB block, and the distribution of the
    /// frames between uses (1, 2, 3, 4, 5-8, 9-16, 17+): the ring's reuse distance.
    std::unordered_map<u64, u64> last_use;
    std::array<u64, 7> reuse{};
    std::chrono::steady_clock::time_point window = std::chrono::steady_clock::now();
    u64 calls = 0;
};
/// 256 MiB regions; 1 MiB inside the hot 0x104xxxxxxx window (the engine's frame data).
BufferStats& Stats();
void NoteHotUse(VAddr address, u32 size) {
    if ((address >> 28) != 0x104) {
        return;
    }
    auto& st = Stats();
    const u64 frame = TlgStats::gpu_frames.load(std::memory_order_relaxed);
    for (u64 block = address >> 16; block <= (address + size - 1) >> 16; ++block) {
        auto [it, inserted] = st.last_use.try_emplace(block, frame);
        if (!inserted && it->second != frame) {
            const u64 d = frame - it->second;
            ++st.reuse[d <= 4 ? d - 1 : d <= 8 ? 4 : d <= 16 ? 5 : 6];
            it->second = frame;
        }
    }
}
u64 RegionKey(VAddr address) {
    return (address >> 28) == 0x104 ? (address >> 20) | (1ull << 40) : address >> 28;
}
bool BufferStatsEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("TLG_BUFFER_STATS");
        return value && value[0] == '1';
    }();
    return enabled;
}
BufferStats& Stats() {
    static BufferStats stats;
    return stats;
}
void PrintBufferStats() {
    auto& st = Stats();
    if ((++st.calls & 4095) != 0) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - st.window).count();
    if (seconds < 5.0) {
        return;
    }
    std::printf("Buffer stats (%.1f s, per second): region, stream copies/bytes, arena bindings, "
                "re-uploaded bytes\n", seconds);
    std::printf("  hot window reuse distance in frames (1,2,3,4,5-8,9-16,17+): %llu %llu %llu %llu "
                "%llu %llu %llu\n",
                (unsigned long long)st.reuse[0], (unsigned long long)st.reuse[1],
                (unsigned long long)st.reuse[2], (unsigned long long)st.reuse[3],
                (unsigned long long)st.reuse[4], (unsigned long long)st.reuse[5],
                (unsigned long long)st.reuse[6]);
    st.reuse = {};
    for (const auto& [region, r] : st.regions) {
        const u64 base = (region >> 40) ? (region & ((1ull << 40) - 1)) << 20 : region << 28;
        if ((region >> 40) && r.stream_bytes + r.upload_bytes < 1e6 * seconds) {
            continue; // quiet MiB of the hot window
        }
        std::printf("  %#012llx: %8.0f copies %8.2f MB, %8.0f arena %8.2f MB bound, %8.2f MB uploads\n",
                    static_cast<unsigned long long>(base), r.stream_count / seconds,
                    r.stream_bytes / seconds / 1e6, r.arena_count / seconds,
                    r.arena_bytes / seconds / 1e6,
                    r.upload_bytes / seconds / 1e6);
    }
    st.regions.clear();
    st.window = now;
}
} // namespace

std::pair<const Buffer*, u64> BufferCache::ObtainBuffer(VAddr device_addr, u32 size,
                                                        bool is_written, bool is_texel_buffer) {
    if (is_written) {
        NoteGpuWriteForEager(device_addr, size);
    }
    texture_cache.ResolveReadbacks(device_addr, size, true, ReadbackSource::BufferObtain);
    const bool stats = BufferStatsEnabled();
    if (stats) {
        PrintBufferStats();
        NoteHotUse(device_addr, size);
    }
    // For read-only buffers use device local stream buffer to reduce renderpass breaks.
    if (!is_written && size <= STREAM_THRESHOLD && !IsRegionGpuModified(device_addr, size)) {
        if (stats) {
            auto& region = Stats().regions[RegionKey(device_addr)];
            ++region.stream_count;
            region.stream_bytes += size;
        }
        // tlg: the guest data is copied on a copy thread, started now; submission and
        // guest-visible fences wait for it (Scheduler::WaitHostCopies).
        if (!stream_buffer.mapped_data.empty() &&
            !TlgToggle::Disabled(TlgToggle::DeferredStreamCopies)) {
            if (const auto offset = stream_buffer.Reserve(size, instance.UniformMinAlignment())) {
                SmallGuestCopy({
                    .run = &RunGuestCopy,
                    .context = this,
                    .source = device_addr,
                    .destination = reinterpret_cast<u64>(stream_buffer.mapped_data.data() + *offset),
                    .size = size,
                    .extra = reinterpret_cast<u64>(static_cast<Buffer*>(&stream_buffer)),
                });
                return {&stream_buffer, *offset};
            }
        }
        const auto [data, offset] = stream_buffer.Map(size, instance.UniformMinAlignment());
        memory->CopySparseMemory(device_addr, data, size);
        stream_buffer.Commit();
        return {&stream_buffer, offset};
    }
    const u64 first_block = device_addr >> block_shift;
    const u64 last_block = (device_addr + size - 1) >> block_shift;
    const auto* arena = GetArena(first_block, last_block);
    EnsureResident(arena, first_block, last_block);
    const u64 uploaded_before = TlgStats::buffer_upload_bytes.load(std::memory_order_relaxed);
    SynchronizeMemory(arena, device_addr, size, is_written, is_texel_buffer);
    if (stats) {
        auto& region = Stats().regions[RegionKey(device_addr)];
        ++region.arena_count;
        region.arena_bytes += size;
        region.upload_bytes +=
            TlgStats::buffer_upload_bytes.load(std::memory_order_relaxed) - uploaded_before;
    }
    if (is_written) {
        gpu_modified_ranges.Add(device_addr, size);
    }
    return {arena, arena->Offset(device_addr)};
}

namespace {
/// tlg: ObtainBufferForImage calls in progress on this thread. The texture cache calls it with
/// its mutex held (UpdateImage, ExpandImage), so no texture can be freed from there.
thread_local u32 image_refresh_depth = 0;
} // namespace

std::pair<const Buffer*, u64> BufferCache::ObtainBufferForImage(VAddr device_addr, u32 size) {
    ++image_refresh_depth;
    struct DepthGuard {
        ~DepthGuard() {
            --image_refresh_depth;
        }
    } depth_guard;
    texture_cache.ResolveReadbacks(device_addr, size, true, ReadbackSource::BufferForImage);
    if (IsRegionGpuModified(device_addr, size)) {
        return ObtainBuffer(device_addr, size, false);
    }
    const auto staging = staging_pool.Request(size, VideoCore::MemoryType::HostUncached,
                                              instance.StorageMinAlignment());
    if (!TlgToggle::Disabled(TlgToggle::DeferredUploads)) {
        // tlg: texture data is copied on the copy threads (streaming: 100+ MB per frame);
        // the upload reads the staging only after submission, which waits for the copies.
        constexpr u64 Chunk = 1_MB;
        for (u64 offset = 0; offset < staging.size; offset += Chunk) {
            const TlgCopy::Item item{
                .run = &RunGuestCopy,
                .context = this,
                .source = device_addr + offset,
                .destination = reinterpret_cast<u64>(staging.mapped + offset),
                .size = std::min(Chunk, staging.size - offset),
                .extra = reinterpret_cast<u64>(staging.buffer),
            };
            if (staging.size < Chunk) {
                SmallGuestCopy(item);
            } else {
                TlgCopy::Async([item] { item.run(item); });
                scheduler.NoteHostCopySource(item.source, item.size);
            }
        }
        return {staging.buffer, staging.offset};
    }
    memory->CopySparseMemory(device_addr, staging.mapped, staging.size);
    staging.Flush();
    return {staging.buffer, staging.offset};
}

bool BufferCache::IsRegionCpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionCpuModified(addr, size);
}

bool BufferCache::IsRegionGpuModified(VAddr addr, size_t size) {
    return memory_tracker->IsRegionGpuModified(addr, size);
}

void BufferCache::ProcessFaultBuffer() {
    fault_manager->ProcessFaultBuffer();
}

void BufferCache::SynchronizeDmaBuffers() {
    for (const auto& range : resident_ranges) {
        const u64 page = range.start >> (arena_page_bits - block_shift);
        const VAddr device_addr = range.start << block_shift;
        const u64 size = (range.end - range.start) << block_shift;
        SynchronizeMemory(address_space[page], device_addr, size, false, false);
    }
}

const Buffer* BufferCache::GetArena(u64 first_block, u64 last_block) {
    const u64 first_page = first_block >> blocks_per_arena_page_shift;
    const u64 last_page = last_block >> blocks_per_arena_page_shift;
    // Arenas cover contiguous runs of pages: when both ends are in the same one, so is the rest.
    if (const Buffer* arena = address_space[first_page]; arena && arena == address_space[last_page]) {
        return arena;
    }
    // tlg: the pages the new arena covers: the request plus every arena it touches (an arena may
    // join several pages, up to the driver's buffer limit).
    u64 lo = first_page, hi = last_page;
    for (u64 page = first_page; page <= last_page; ++page) {
        if (const Buffer* arena = address_space[page]) {
            lo = std::min<u64>(lo, arena->cpu_addr >> arena_page_bits);
            hi = std::max<u64>(hi, ((arena->cpu_addr + arena->size_bytes) >> arena_page_bits) - 1);
        }
    }
    const u64 num_pages = hi - lo + 1;
    // Over the driver's limit: the old code built such arenas too (two merged arenas could
    // exceed it) and drivers accepted them, so warn instead of stopping.
    if (num_pages > max_arena_pages) {
        LOG_WARNING(Render, "Arena of {} pages of {} MiB exceeds the driver buffer limit "
                    "(request {:#x}+{} MiB)", num_pages, arena_page_size >> 20,
                    first_block << block_shift,
                    ((last_block - first_block + 1) << block_shift) >> 20);
    }

    bool any_arena = false;
    for (u64 page = lo; page <= hi; ++page) {
        any_arena |= address_space[page] != nullptr;
    }
    const u64 base_addr = lo << arena_page_bits;
    const u64 total_size = num_pages << arena_page_bits;
    auto* new_arena = &arenas.emplace_back(instance, base_addr, total_size, MemoryType::Sparse);
    if (any_arena) {
        LOG_WARNING(Render, "Migrating arena ({} pages of {} MiB)", num_pages, arena_page_size >> 20);
        const u64 base_block = base_addr >> block_shift;
        const u64 end_block = (base_addr + total_size) >> block_shift;
        auto* bind = BindsForArena(new_arena);
        resident_ranges.ForEachInRange(base_block, end_block, [&](const Backing& backing) {
            const u64 start = std::max(base_block, backing.start);
            const u64 end = std::min(end_block, backing.end);
            bind->binds.push_back(vk::SparseMemoryBind{
                .resourceOffset = (start - base_block) << block_shift,
                .size = (end - start) << block_shift,
                .memory = backing.memory,
                .memoryOffset = (backing.offset + start - backing.start) << block_shift,
            });
        });
    }
    for (u64 page = lo; page <= hi; ++page) {
        address_space[page] = new_arena;
    }
    return new_arena;
}

// tlg: from shadPS4-Infamous-FixBuild 4a206865 and 0c3fada0. Memory used to become resident a
// block at a time, each range its own driver allocation and sparse bind; granules and chunks
// make far fewer. When device memory runs out, old textures are freed and it is tried once more.
void BufferCache::EnsureResident(const Buffer* arena, u64 first_block, u64 last_block) {
    // Granules never cross an arena page (1 GiB or more), so the widened range stays within the
    // arena.
    const u64 granule_blocks = std::max<u64>(RESIDENCY_GRANULE_SIZE >> block_shift, 1);
    const u64 bind_first = Common::AlignDown(first_block, granule_blocks);
    const u64 bind_end = Common::AlignUp(last_block + 1, granule_blocks);

    for (u32 attempt = 0;; ++attempt) {
        u32 resident_blocks{};
        IntervalList bind_ranges;
        resident_ranges.ForEachGap(bind_first, bind_end, [&](u64 start, u64 end) {
            resident_blocks += end - start;
            bind_ranges.Add({start, end});
        });

        if (bind_ranges.Empty()) {
            return;
        }
        TlgStats::Timer timer{TlgStats::t_resident};

        // All memory is allocated before anything is recorded or bound: making room for it
        // submits, which sends the pending binds and runs pending operations (and so can make
        // some of these ranges resident: the gaps are looked up again after it).
        ResidencyAttempt state{.chunk = residency_chunk, .chunk_used = residency_chunk_used};
        boost::container::small_vector<std::pair<vk::DeviceMemory, u64>, 8> memories;
        for (const auto& range : bind_ranges) {
            const auto memory = AllocateResidency((range.end - range.start) << block_shift, state);
            if (!memory) {
                break;
            }
            memories.push_back(*memory);
        }
        if (memories.size() != bind_ranges.Size()) {
            UndoResidency(state);
            const u64 bytes = u64{resident_blocks} << block_shift;
            ASSERT_MSG(attempt == 0,
                       "Out of device memory making {} MB resident, after freeing textures",
                       bytes >> 20);
            ReleaseMemoryForResidency(bytes);
            continue;
        }

        boost::container::small_vector<vk::BufferCopy, 8> copies;
        const auto staging = staging_pool.Request(resident_blocks * sizeof(vk::DeviceAddress),
                                                  MemoryType::HostUncached);

        ArenaBinds* binds = BindsForArena(arena);
        auto* bda_addrs = reinterpret_cast<vk::DeviceAddress*>(staging.mapped);
        u64 offset = staging.offset;
        size_t index = 0;
        for (const auto& range : bind_ranges) {
            const u64 range_size = (range.end - range.start) << block_shift;
            const auto [device_memory, memory_offset] = memories[index++];

            Backing backing;
            backing.start = range.start;
            backing.end = range.end;
            backing.memory = device_memory;
            // In blocks, as Backing::CanMergeWith and SubRange count them.
            backing.offset = memory_offset >> block_shift;
            resident_ranges.Add(backing);

            LOG_DEBUG(Render, "Making range start={}, end={} resident", backing.start,
                      backing.end);

            const auto& bind = binds->binds.emplace_back(vk::SparseMemoryBind{
                .resourceOffset = (range.start << block_shift) - arena->cpu_addr,
                .size = range_size,
                .memory = device_memory,
                .memoryOffset = memory_offset,
            });

            for (u32 block = 0; block < bind.size; block += block_size) {
                *(bda_addrs++) = arena->BufferDeviceAddress() + bind.resourceOffset + block;
            }
            const u64 copy_size = (backing.end - backing.start) * sizeof(vk::DeviceAddress);
            copies.emplace_back(offset, backing.start * sizeof(vk::DeviceAddress), copy_size);
            offset += copy_size;
        }

        staging.Flush();
        runtime.CopyBuffer(staging.buffer, bda_pagetable_buffer.get(), copies);
        return;
    }
}

std::optional<std::pair<vk::DeviceMemory, u64>> BufferCache::AllocateResidency(
    u64 size, ResidencyAttempt& attempt) {
    // Null when the device is out of memory; any other failure ends the game as before.
    const auto allocate = [&](u64 allocation_size) -> vk::DeviceMemory {
        const vk::MemoryAllocateInfo alloc_info = {
            .allocationSize = allocation_size,
            .memoryTypeIndex = arena_memory_type_index,
        };
        auto result = instance.GetDevice().allocateMemory(alloc_info);
        if (result.result == vk::Result::eErrorOutOfDeviceMemory) {
            return {};
        }
        const auto memory = Vulkan::Check(std::move(result));
        if (mapped_readbacks_enabled) {
            residency_mappings.emplace(memory, ResidencyMapping{.size = allocation_size});
        }
        attempt.allocated.push_back(memory);
        ++residency_allocations;
        return memory;
    };
    using Allocation = std::pair<vk::DeviceMemory, u64>;
    // Ranges keep coming as the game streams data in. Carving them out of large chunks keeps the
    // driver from allocating memory each time, which is slow and limited to a few thousand
    // allocations per process on Windows. Large ranges get their own.
    if (size > RESIDENCY_CHUNK_SIZE / 4) {
        const auto memory = allocate(size);
        return memory ? std::optional{Allocation{memory, 0}} : std::nullopt;
    }
    if (!residency_chunk || residency_chunk_used + size > RESIDENCY_CHUNK_SIZE) {
        const auto chunk = allocate(RESIDENCY_CHUNK_SIZE);
        if (!chunk) {
            // A whole chunk may not fit when memory is short, while the range itself still does.
            static u32 warned = 0;
            if (warned < 16 || warned % 64 == 0) {
                LOG_WARNING(Render, "No room for a {} MB residency chunk, allocating {} MB instead",
                            RESIDENCY_CHUNK_SIZE >> 20, size >> 20);
            }
            ++warned;
            const auto memory = allocate(size);
            return memory ? std::optional{Allocation{memory, 0}} : std::nullopt;
        }
        residency_chunk = chunk;
        residency_chunk_used = 0;
    }
    const u64 offset = residency_chunk_used;
    residency_chunk_used += size;
    return Allocation{residency_chunk, offset};
}

void BufferCache::UndoResidency(const ResidencyAttempt& attempt) {
    // Nothing was bound to this memory yet: chunks taken since are freed with the rest.
    for (const vk::DeviceMemory memory : attempt.allocated) {
        if (auto it = residency_mappings.find(memory); it != residency_mappings.end()) {
            if (it->second.data) instance.GetDevice().unmapMemory(memory);
            residency_mappings.erase(it);
        }
        instance.GetDevice().freeMemory(memory);
    }
    residency_allocations -= attempt.allocated.size();
    residency_chunk = attempt.chunk;
    residency_chunk_used = attempt.chunk_used;
}

void BufferCache::ReleaseMemoryForResidency(u64 bytes) {
    // The device can run out of memory when the game makes gigabytes resident at once, which the
    // texture collector, running once per submission, can't keep up with.
    LOG_WARNING(Render, "Out of device memory making {} MB resident, freeing textures",
                bytes >> 20);
    // The texture binding helper may be looking images up for the draw being prepared on this
    // thread: join it before images go (the same hook the buffer cache uses before it changes
    // image state).
    runtime.BeforeImageAccess();
    if (image_refresh_depth == 0) {
        texture_cache.ReleaseMemoryForAllocation();
    } else {
        // Refreshing an image, under the texture cache's lock: only the deletions already
        // pending can help.
        LOG_WARNING(Render, "Out of device memory while refreshing an image: only waiting for "
                            "the GPU to release memory");
    }
    // The images' memory is freed by pending operations once the GPU is done with them. This
    // thread owns the caches (stage B while draw packets are in flight, stage A otherwise:
    // the scheduler's owner hook drains the draw pipe first), as for DownloadMemory's Finish.
    // Inside a pending operation (the fault buffer's) the nested pop does nothing and the
    // deletions run after it, so the second attempt may still fail there.
    scheduler.Finish();
    scheduler.PopPendingOperations();
}

namespace {
constexpr u64 EagerPageSize = 4096;
constexpr u64 EagerBufferSize = 32ull << 20; // 8192 pages
constexpr u32 EagerIdleFences = 2400;        // ~40 s at 60 fences/s: forgotten
// tlg: a page whose copies keep going stale unread (the GPU writes it again before the game
// reads it) is copied at fewer fences, twice as few each time, up to 1 in 8, until a copy of it
// is read again. In the fork three quarters of such copies were wasted (9a5224bf).
constexpr u8 EagerMaxBackoff = 7;
// Hybrid opt-in: spend fewer speculative copies on repeatedly stale, unread pages.
constexpr u8 EagerHybridMaxBackoff = 63;
} // namespace

bool BufferCache::EagerEnabled() {
    static const bool enabled = [] {
        const char* e = std::getenv("TLG_EAGER_READBACK");
        return e && *e == '1' &&
               (EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Precise ||
                RegionManager::HybridReadProtectionEnabled());
    }();
    return enabled;
}

void BufferCache::NoteReadback(VAddr addr, u64 size) {
    if (!EagerEnabled()) {
        return;
    }
    std::scoped_lock lock{eager_mutex};
    for (VAddr page = Common::AlignDown(addr, EagerPageSize); page < addr + size; page += EagerPageSize) {
        auto [it, inserted] = eager_pages.try_emplace(page);
        if (!inserted) {
            it->second.idle = 0;
            it->second.skip = 0; // a copy at the next fence would have spared this fault
            continue;
        }
        u64 offset;
        if (!eager_free_offsets.empty()) {
            offset = eager_free_offsets.back();
            eager_free_offsets.pop_back();
        } else if (eager_next_offset + EagerPageSize <= EagerBufferSize) {
            offset = eager_next_offset;
            eager_next_offset += EagerPageSize;
        } else {
            eager_pages.erase(it);
            break;
        }
        it->second = EagerPage{offset, 0, true, false, 0, 0, 0};
    }
    eager_count.store(static_cast<u32>(eager_pages.size()), std::memory_order_release);
}

void BufferCache::ApplyEagerSubtractsLocked() {
    for (const VAddr page : eager_subtracts) {
        gpu_modified_ranges.Subtract(page, EagerPageSize);
    }
    eager_subtracts.clear();
}

void BufferCache::MarkEagerStale(VAddr addr, u64 size) {
    if (!eager_count.load(std::memory_order_acquire)) {
        return;
    }
    std::scoped_lock lock{eager_mutex};
    for (auto it = eager_pages.lower_bound(Common::AlignDown(addr, EagerPageSize));
         it != eager_pages.end() && it->first < addr + size; ++it) {
        it->second.dirty = true;
        it->second.valid = false;
    }
}

void BufferCache::NoteGpuWriteForEager(VAddr addr, u64 size) {
    // Before the memory is marked GPU modified (SynchronizeMemory), see ApplyGpuCopyIf.
    MarkDeferredStale(addr, size, false);
    if (!eager_count.load(std::memory_order_acquire)) {
        return;
    }
    std::scoped_lock lock{eager_mutex};
    ApplyEagerSubtractsLocked();
    for (auto it = eager_pages.lower_bound(Common::AlignDown(addr, EagerPageSize));
         it != eager_pages.end() && it->first < addr + size; ++it) {
        auto& page = it->second;
        if (page.valid) {
            // Copied at a fence and never read before the GPU wrote it again.
            static const u8 backoff_cap = [] {
                const char* opt = std::getenv("TLG_ISS_EAGER_BACKOFF_63");
                return RegionManager::HybridReadProtectionEnabled() && opt && opt[0] == '1'
                           ? EagerHybridMaxBackoff : EagerMaxBackoff;
            }();
            page.backoff = std::min<u8>(static_cast<u8>(page.backoff * 2 + 1), backoff_cap);
            page.skip = page.backoff;
            if (ReadbackStatsEnabled()) {
                Readbacks().eager_stale.fetch_add(1);
            }
        }
        page.dirty = true;
        page.valid = false;
    }
}

void BufferCache::EagerCopyAtFence() {
    if (deferred_count.load(std::memory_order_acquire)) {
        PruneDeferredReadbacks();
    }
    if (!eager_count.load(std::memory_order_acquire)) {
        return;
    }
    if (!eager_buffer) {
        eager_buffer = std::make_unique<Buffer>(instance, 0, EagerBufferSize, MemoryType::HostCached,
                                                "tlg eager readback");
    }
    std::scoped_lock lock{eager_mutex};
    ApplyEagerSubtractsLocked();
    const Buffer* arena = nullptr;
    boost::container::small_vector<vk::BufferCopy, 64> copies;
    boost::container::small_vector<EagerPage*, 64> copied;
    u64 skipped = 0;
    const auto flush_copies = [&] {
        if (!copies.empty()) {
            runtime.CopyBuffer(arena, eager_buffer.get(), copies);
            copies.clear();
        }
    };
    for (auto it = eager_pages.begin(); it != eager_pages.end();) {
        auto& page = it->second;
        if (++page.idle > EagerIdleFences) {
            eager_free_offsets.push_back(page.offset);
            it = eager_pages.erase(it);
            continue;
        }
        if (page.dirty && page.skip > 0 &&
            memory_tracker->IsRegionGpuModified(it->first, EagerPageSize)) {
            --page.skip;
            ++skipped;
        } else if (page.dirty && memory_tracker->IsRegionGpuModified(it->first, EagerPageSize)) {
            const u64 block = it->first >> block_shift;
            const Buffer* page_arena = GetArena(block, (it->first + EagerPageSize - 1) >> block_shift);
            if (page_arena != arena) {
                flush_copies();
                arena = page_arena;
            }
            copies.push_back(vk::BufferCopy{.srcOffset = it->first - arena->cpu_addr,
                                            .dstOffset = page.offset,
                                            .size = EagerPageSize});
            copied.push_back(&page);
        }
        ++it;
    }
    eager_count.store(static_cast<u32>(eager_pages.size()), std::memory_order_release);
    flush_copies();
    if (ReadbackStatsEnabled()) {
        Readbacks().eager_copied.fetch_add(copied.size());
        Readbacks().eager_skipped.fetch_add(skipped);
        ReportReadbacks();
    }
    if (copied.empty()) {
        return;
    }
    const u64 tick = scheduler.CurrentTick();
    scheduler.Flush();
    for (EagerPage* page : copied) {
        page->tick = tick;
        page->dirty = false;
        page->valid = true;
    }
}

bool BufferCache::TryEagerRead(VAddr addr, u64 size) {
    if (!eager_count.load(std::memory_order_acquire)) {
        return false;
    }
    const VAddr page_addr = Common::AlignDown(addr, EagerPageSize);
    if (addr + size > page_addr + EagerPageSize || !eager_buffer) {
        return false;
    }
    u64 offset, tick;
    {
        std::scoped_lock lock{eager_mutex};
        const auto it = eager_pages.find(page_addr);
        if (it == eager_pages.end() || !it->second.valid ||
            !memory_tracker->IsRegionGpuModified(page_addr, EagerPageSize)) {
            return false;
        }
        if (merged_count.load(std::memory_order_acquire)) {
            std::scoped_lock merge_lock{merge_mutex};
            const auto m = merged_writes.upper_bound(page_addr + EagerPageSize - 1);
            if (m != merged_writes.begin() && std::prev(m)->second > page_addr) {
                return false; // CPU bytes not yet on the GPU: the copy would lose them
            }
        }
        it->second.idle = 0;
        offset = it->second.offset;
        tick = it->second.tick;
    }
    scheduler.GetWorkSemaphore()->Wait(tick);
    std::scoped_lock lock{eager_mutex};
    const auto it = eager_pages.find(page_addr);
    if (it == eager_pages.end() || !it->second.valid || it->second.tick != tick) {
        return false; // the GPU wrote it again meanwhile
    }
    eager_buffer->Invalidate(offset, EagerPageSize);
    // tlg: only while the page is still GPU modified, checked under the region lock: a deferred
    // readback or a download may have brought it back meanwhile, and the guest may have
    // written it since without a fault.
    // (Unmapped memory has no backing to write, as for DownloadMemory.)
    const bool applied = memory_tracker->ApplyGpuCopyIf(
        page_addr, EagerPageSize, [] { return true; }, [&](VAddr start, VAddr end) {
            memory->TryWriteBacking(reinterpret_cast<void*>(start),
                                    eager_buffer->mapped_data.data() + offset + (start - page_addr),
                                    end - start);
        });
    if (!applied) {
        return false;
    }
    eager_subtracts.push_back(page_addr);
    it->second.valid = false;
    it->second.backoff = 0;
    it->second.skip = 0;
    if (ReadbackStatsEnabled()) {
        Readbacks().eager_used.fetch_add(1);
    }
    return true;
}

bool BufferCache::DeferredReadbackEnabled() {
    static const bool enabled = [] {
        const char* e = std::getenv("TLG_DEFERRED_READBACK");
        if (!e || *e != '1') {
            return false;
        }
        const auto mode = EmulatorSettings.GetReadbacksMode();
        if (mode == GpuReadbacksMode::Precise) {
            return true;
        }
#if defined(__ANDROID__) && defined(ISS_ANDROID_BIONIC)
        // Relaxed write faults still need the GPU bytes they are not overwriting.
        // ReadMemory uses owner-thread publication for this mode, never a guest-thread
        // writeback that could race with Relaxed cache mutation.
        // Hot-page tracking deliberately keeps some CPU pages writable. Without write
        // faults, a guest could change a prefetched page before its copy is published.
        const char* hot = std::getenv("TLG_HOT_PAGES");
        return mode == GpuReadbacksMode::Relaxed && !(hot && *hot == '1');
#else
        return false;
#endif
    }();
    return enabled;
}

bool BufferCache::MappedReadbackPolicy() {
#if defined(__ANDROID__) && defined(ISS_ANDROID_BIONIC)
    const char* env = std::getenv("TLG_MAPPED_READBACK");
    return !(env && *env == '0') && DeferredReadbackEnabled() &&
           EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Relaxed &&
           !EmulatorSettings.IsDirectMemoryAccessEnabled();
#else
    return false;
#endif
}

void BufferCache::NoteDeviceWrite(VAddr address, u64 size, u64 tick) {
    if (!write_ticks.Write(address, size, tick)) return;
    // Unlike a staging copy, a mapped read has not taken its snapshot yet. Uploads
    // and tiling as well as shader writes must invalidate it before device execution.
    if (!deferred_count.load(std::memory_order_acquire)) return;
    std::scoped_lock lock{deferred_mutex};
    for (const auto& readback : deferred_readbacks) {
        if (!readback->Done() &&
            readback->CopiesTouchPages(address, address + size)) {
            if (!readback->mapped) {
                // A staging copy was captured at readback->tick. If a later GPU
                // submission overwrites a copied page, its old contents must not
                // reach guest memory or unprotect that page. The existing
                // recovery/retry path will fetch the newer GPU contents.
                // Only enabled on hybrid, not for the ordinary Relaxed policy.
                if (RegionManager::HybridReadProtectionEnabled() && tick > readback->tick) {
                    readback->stale.store(true, std::memory_order_release);
                }
            } else if (readback->page_scoped &&
                EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Relaxed &&
                !EmulatorSettings.IsDirectMemoryAccessEnabled()) {
                readback->invalid_pages.Add({Common::AlignDown(address, u64{4096}),
                                             Common::AlignUp(address + size, u64{4096})});
            } else {
                readback->stale.store(true, std::memory_order_release);
            }
        }
    }
}

bool BufferCache::PrepareMappedReadback(DeferredReadback& readback, u64 bytes, bool allow_current,
                                       u64 wait_tick_limit) {
    if (!mapped_readbacks_enabled || EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Relaxed ||
        EmulatorSettings.IsDirectMemoryAccessEnabled()) return false;
    u64 tick = 0;
    for (const auto& copy : readback.copies) {
        const u64 written = write_ticks.Latest(readback.arena_base + copy.srcOffset, copy.size);
        if (!written || written > scheduler.CurrentTick() ||
            (!allow_current && written == scheduler.CurrentTick())) {
            if (ReadbackStatsEnabled()) Readbacks().mapped_current.fetch_add(1);
            return false;
        }
        tick = std::max(tick, written);
    }
    // Prefetch is optional: a newer, unfinished neighbor must not delay the page
    // the guest actually requested. StartDeferredReadback retries just that page.
    // Completed neighbors still participate, preserving normal ready batching.
    if (wait_tick_limit && tick > wait_tick_limit &&
        !scheduler.GetWorkSemaphore()->IsFree(tick)) {
        if (ReadbackStatsEnabled()) Readbacks().mapped_prefetch_narrowed.fetch_add(1);
        return false;
    }
    std::vector<DeferredReadback::MappedSpan> spans;
    bool available = true;
    for (const auto& copy : readback.copies) {
        const VAddr start = readback.arena_base + copy.srcOffset, end = start + copy.size;
        VAddr covered = start;
        resident_ranges.ForEachInRange(start >> block_shift, ((end - 1) >> block_shift) + 1,
            [&](const Backing& backing) {
                if (!available) return;
                const VAddr lo = std::max(start, backing.start << block_shift);
                const VAddr hi = std::min(end, backing.end << block_shift);
                auto found = residency_mappings.find(backing.memory);
                if (lo != covered || found == residency_mappings.end()) {
                    available = false;
                    return;
                }
                auto& mapping = found->second;
                const u64 offset = (backing.offset << block_shift) + lo - (backing.start << block_shift);
                if (offset > mapping.size || hi - lo > mapping.size - offset || mapping.failed) {
                    available = false;
                    return;
                }
                if (!mapping.data) {
                    const auto result = instance.GetDevice().mapMemory(backing.memory, 0, VK_WHOLE_SIZE);
                    if (result.result != vk::Result::eSuccess || !result.value) {
                        mapping.failed = true;
                        available = false;
                        return;
                    }
                    mapping.data = static_cast<const u8*>(result.value);
                }
                spans.push_back({backing.memory, offset, hi - lo,
                                 copy.dstOffset + lo - start, mapping.data + offset});
                covered = hi;
            });
        if (!available || covered != end) { available = false; break; }
    }
    if (!available || spans.empty()) {
        if (ReadbackStatsEnabled()) Readbacks().mapped_unavailable.fetch_add(1);
        return false;
    }
    readback.mapped_spans = std::move(spans);
    readback.snapshot_size = bytes;
    readback.tick = tick;
    readback.mapped = true;
#if defined(__ANDROID__) && defined(ISS_ANDROID_BIONIC)
    static const bool partial = [] {
        const char* env = std::getenv("TLG_MAPPED_READBACK_PARTIAL");
        return !(env && *env == '0');
    }();
    readback.page_scoped = partial;
#endif
    if (ReadbackStatsEnabled()) Readbacks().mapped_scheduled.fetch_add(1);
    return true;
}

bool BufferCache::CaptureMappedReadback(DeferredReadback& readback) {
    // Only the Relaxed cache owner publishes these reads. No device pointer is read
    // until the old submission (including its HOST_READ barrier) has completed.
    if (EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Relaxed ||
        EmulatorSettings.IsDirectMemoryAccessEnabled() ||
        !scheduler.GetWorkSemaphore()->IsFree(readback.tick)) return false;
    readback.publication_copies.clear();
    readback.retry_copies.clear();
    for (const auto& copy : readback.copies) {
        const VAddr start = readback.arena_base + copy.srcOffset, end = start + copy.size;
        const bool changed = readback.invalid_pages.Overlaps(start, end) ||
                             write_ticks.Latest(start, copy.size) > readback.tick;
        if (!changed) {
            // Keep the normal all-valid path at one interval check per copy.
            readback.publication_copies.push_back(copy);
            continue;
        }
        if (!readback.page_scoped) return false;
        for (VAddr at = start; at < end;) {
            const VAddr page = Common::AlignDown(at, u64{4096});
            const VAddr hi = std::min(end, page + 4096);
            const bool changed = readback.invalid_pages.Overlaps(page, page + 4096) ||
                                 write_ticks.Latest(page, 4096) > readback.tick;
            auto& parts = changed ? readback.retry_copies : readback.publication_copies;
            parts.push_back(vk::BufferCopy{.srcOffset = at - readback.arena_base,
                                          .dstOffset = copy.dstOffset + at - start,
                                          .size = hi - at});
            at = hi;
        }
    }
    if (readback.publication_copies.empty()) {
        // Every copied page changed. No mapped pointer may be read at the old tick.
        readback.stale.store(true, std::memory_order_release);
        return false;
    }
    readback.mapped_snapshot.resize(readback.snapshot_size);
    const auto device = instance.GetDevice();
    const u64 atom = instance.NonCoherentAtomSize();
    for (const auto& span : readback.mapped_spans) {
        for (const auto& copy : readback.publication_copies) {
            const u64 begin = std::max(span.destination, copy.dstOffset);
            const u64 end = std::min(span.destination + span.size, copy.dstOffset + copy.size);
            if (begin >= end) continue;
            const u64 delta = begin - span.destination;
            if (!residency_coherent) {
                const auto found = residency_mappings.find(span.memory);
                if (found == residency_mappings.end()) return false;
                // Atoms are <=4 KiB and residency offsets are page aligned: this
                // invalidation cannot touch any neighboring, invalidated GPU page.
                const u64 lo = Common::AlignDown(span.offset + delta, atom);
                const u64 hi = std::min(Common::AlignUp(span.offset + delta + end - begin, atom),
                                        found->second.size);
                const vk::MappedMemoryRange range{.memory = span.memory, .offset = lo, .size = hi - lo};
                if (device.invalidateMappedMemoryRanges(range) != vk::Result::eSuccess) return false;
            }
            std::memcpy(readback.mapped_snapshot.data() + begin, span.source + delta, end - begin);
        }
    }
    readback.partial = !readback.retry_copies.empty();
    return true;
}

std::shared_ptr<BufferCache::DeferredReadback> BufferCache::StartDeferredReadback(VAddr device_addr,
                                                                                  u64 size) {
    // Opt-in locality test. Unmodified V2 behavior when disabled.
    static const bool targeted_stage = [] {
        const char* e = std::getenv("TLG_ISS_HYBRID_TARGETED_STAGE");
        return e && e[0] == '1' && RegionManager::HybridReadProtectionEnabled();
    }();
    PruneDeferredReadbacks();
    const bool prioritize_request = mapped_readbacks_enabled &&
        EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Relaxed &&
        !EmulatorSettings.IsDirectMemoryAccessEnabled();
    if (prioritize_request && !memory_tracker->IsRegionGpuModified(device_addr, size)) {
        // Another completion cleared this request while it was queued. Starting a
        // neighbor's prefetch here can introduce an entirely unrelated GPU wait.
        if (ReadbackStatsEnabled()) Readbacks().mapped_coherent_skipped.fetch_add(1);
        return nullptr;
    }
    const u64 wait_tick_limit = prioritize_request ? write_ticks.Latest(device_addr, size) : 0;
    const auto [arena, window_start, window_end] = GetReadbackWindow(device_addr, size);
    std::shared_ptr<DeferredReadback> joined;
    {
        // Another guest thread is already waiting for this memory: wait for the same copy.
        std::scoped_lock lock{deferred_mutex};
        for (const auto& readback : deferred_readbacks) {
            const VAddr join_start = readback->mapped ? device_addr : window_start;
            const VAddr join_end = readback->mapped ? device_addr + size : window_end;
            if (!readback->Done() && !readback->stale && readback->start <= join_start &&
                join_end <= readback->end) {
                // A staging envelope covers 512 KiB, but not necessarily each
                // page inside it. Never return an unrelated incomplete snapshot.
                if (targeted_stage && !readback->mapped &&
                    !readback->CopiesTouchPages(device_addr, device_addr + size)) {
                    continue;
                }
                if (readback->mapped && wait_tick_limit && readback->tick > wait_tick_limit &&
                    !scheduler.GetWorkSemaphore()->IsFree(readback->tick)) {
                    // Its wider prefetch waits longer than our own data needs.
                    // Settlement below recovers it without waiting on the owner.
                    continue;
                }
                if (readback->page_scoped &&
                    (readback->invalid_pages.Overlaps(device_addr, device_addr + size) ||
                     write_ticks.Latest(device_addr, size) > readback->tick)) {
                    // This caller needs newer data. Do not make it wait the old tick
                    // and then the new one. Settle below salvages an already-ready
                    // neighbor or recovers without blocking if the old tick is in flight.
                    readback->invalid_pages.Add({Common::AlignDown(device_addr, u64{4096}),
                                                 Common::AlignUp(device_addr + size, u64{4096})});
                    continue;
                }
                if (ReadbackStatsEnabled()) {
                    Readbacks().joined.fetch_add(1);
                }
                joined = readback;
                break;
            }
        }
    }
    if (joined) {
        // A shared mapped read can become ready between guest requests. Publish
        // outside deferred_mutex, avoiding another guest wait/owner round trip.
        if (joined->mapped && scheduler.IsFree(joined->tick) &&
            FinishDeferredReadback(*joined, false) && ReadbackStatsEnabled()) {
            Readbacks().mapped_inline.fetch_add(1);
        }
        return joined;
    }
    std::shared_ptr<DeferredReadback> readback;
    u64 total_size_bytes = 0;
    const bool try_mapped = mapped_readbacks_enabled;
    const VAddr required_start = std::max<VAddr>(Common::AlignDown(device_addr, u64{4096}), arena->cpu_addr);
    const VAddr required_end = std::min<VAddr>(Common::AlignUp(device_addr + size, u64{4096}),
                                              arena->cpu_addr + arena->size_bytes);
    const VAddr prefetch_start = std::max<VAddr>(Common::AlignDown(device_addr, u64{65536}), arena->cpu_addr);
    const VAddr prefetch_end = std::min<VAddr>(std::max<VAddr>(prefetch_start + 65536, required_end),
                                              arena->cpu_addr + arena->size_bytes);
    for (unsigned attempt = 0; attempt < (try_mapped ? 3u : 1u); ++attempt) {
        const bool mapped_attempt = try_mapped && attempt < 2;
        if (mapped_attempt && attempt == 1 && required_start == prefetch_start &&
            required_end == prefetch_end) continue;
        // Match the release's 64 KiB mapped prefetch. An unrelated current-tick
        // write elsewhere in the 512 KiB staging window must not disqualify it.
        // If the prefetch is ineligible, retry only the requested pages (also in
        // core025), preserving every dirty island in each page before unprotecting it.
        const VAddr start = !mapped_attempt ? window_start : attempt ? required_start : prefetch_start;
        const VAddr end = !mapped_attempt ? window_end : attempt ? required_end : prefetch_end;
        // Overlapping copies are settled first, so their dirty ranges are available
        // and one readback cannot unmark memory another has not published yet.
        const bool targeted_now = targeted_stage && !try_mapped && !mapped_attempt;
        SettleDeferredReadbacks(targeted_now ? required_start : start,
                                targeted_now ? required_end : end,
                                mapped_attempt ? wait_tick_limit : 0);
        if (eager_count.load(std::memory_order_acquire)) {
            std::scoped_lock lock{eager_mutex};
            ApplyEagerSubtractsLocked();
        }
        FlushMergedWrites(arena, start, end - start);
        readback = std::make_shared<DeferredReadback>();
        readback->arena_base = arena->cpu_addr;
        readback->start = start;
        readback->end = end;
        total_size_bytes = 0;
        bool selected = false;
        if (targeted_now) {
            selected = hybrid_staging_hot_pages.ForEachSelected(
                window_start, window_end, device_addr, device_addr + size,
                [&](VAddr page_start, u64 page_length) {
                    const std::size_t first_copy = readback->copies.size();
                    const u64 dest_offset = total_size_bytes;
                    total_size_bytes +=
                        CollectDownloads(arena, page_start, page_length, readback->copies);
                    for (std::size_t i = first_copy; i < readback->copies.size(); ++i) {
                        readback->copies[i].dstOffset += dest_offset;
                    }
                });
        }
        if (!selected) {
            total_size_bytes = CollectDownloads(arena, start, end - start, readback->copies);
        }
        if (mapped_attempt) {
            if (total_size_bytes && PrepareMappedReadback(*readback, total_size_bytes, false,
                                                          attempt == 0 ? wait_tick_limit : 0)) break;
            // CollectDownloads removed these ranges. Restore them before the next
            // attempt or the conventional, unchanged 512 KiB staging fallback.
            for (const auto& copy : readback->copies) {
                gpu_modified_ranges.Add(readback->arena_base + copy.srcOffset, copy.size);
            }
            continue;
        }
        if (!total_size_bytes) return nullptr;
        // Submitted-only attempts above retain their smaller/older wait target. If
        // they failed, keep the conventional 512 KiB sharing envelope, but a known
        // current write needs submission, not an additional GPU-to-staging copy.
        // Flush records the HOST_READ barrier before signalling this tick. Capture
        // still waits/revalidates; mapping a pointer never makes it readable yet.
        static const bool map_current = [] {
            const char* env = std::getenv("TLG_MAPPED_READBACK_CURRENT");
            return !(env && *env == '0');
        }();
        if (try_mapped && map_current &&
            PrepareMappedReadback(*readback, total_size_bytes, true)) {
            if (readback->tick == scheduler.CurrentTick()) {
                scheduler.Flush();
                if (ReadbackStatsEnabled()) Readbacks().mapped_submitted.fetch_add(1);
            }
            break;
        }
        // Held until pruned: the guest thread reads it after the GPU is done with it.
        readback->staging =
            staging_pool.Request(total_size_bytes, VideoCore::MemoryType::HostCached, 0, true);
        for (auto& copy : readback->copies) {
            copy.dstOffset += readback->staging.offset;
        }
        runtime.CopyBuffer(arena, readback->staging.buffer, readback->copies);
        readback->tick = scheduler.CurrentTick();
        scheduler.Flush();
        break;
    }
    // A submitted write may already be complete when the fault first reaches the
    // owner. Publish now while no newer owner command can invalidate it; never wait
    // here. Otherwise retain the asynchronous wait and revalidation path below.
    const bool published_inline = readback->mapped && scheduler.IsFree(readback->tick) &&
                                  FinishDeferredReadback(*readback, false);
    if (published_inline && ReadbackStatsEnabled()) Readbacks().mapped_inline.fetch_add(1);
    if (!published_inline) {
        std::scoped_lock lock{deferred_mutex};
        deferred_readbacks.push_back(readback);
        deferred_count.store(static_cast<u32>(deferred_readbacks.size()),
                             std::memory_order_release);
    }
    if (ReadbackStatsEnabled()) {
        Readbacks().deferred.fetch_add(1);
        Readbacks().deferred_bytes.fetch_add(total_size_bytes);
    }
    return readback;
}

bool BufferCache::FinishDeferredReadback(DeferredReadback& readback, bool wait) {
    if (wait) {
        scheduler.GetWorkSemaphore()->Wait(readback.tick);
    }
    std::scoped_lock lock{readback.mutex};
    if (readback.applied.load(std::memory_order_acquire)) {
        return true;
    }
    if (readback.recovered.load(std::memory_order_acquire) || readback.stale.load()) {
        return false;
    }
    const u8* mapped;
    if (readback.mapped) {
        // Relaxed completion runs on the cache owner. Revalidate before reading the
        // physical mapping: a newer queued writer must use the ordinary snapshot path.
        if (!CaptureMappedReadback(readback)) {
            if (ReadbackStatsEnabled()) Readbacks().mapped_stale.fetch_add(1);
            return false;
        }
        mapped = readback.mapped_snapshot.data();
    } else {
        readback.staging.Invalidate();
        mapped = readback.staging.mapped - readback.staging.offset;
    }
    // A different GPU write inside the 512 KiB prefetch window can now stay live.
    // Apply/unprotect only pages that were actually copied, never the entire window.
    // Copies on the same page are grouped: unmarking the page after the first copy
    // would otherwise prevent the remaining copies from being published.
    const auto& copies = readback.mapped ? readback.publication_copies : readback.copies;
    bool applied = true;
    ForEachReadbackCopyPageGroup(
        readback.arena_base, copies,
        [&](VAddr group_start, VAddr group_end, std::size_t first, std::size_t last) {
            if (!applied) {
                return;
            }
            const VAddr start = std::max<VAddr>(group_start, readback.start);
            const VAddr end = std::min<VAddr>(group_end, readback.end);
            if (start >= end) {
                return;
            }
            applied = memory_tracker->ApplyGpuCopyIf(
                start, end - start, [&] { return !readback.stale.load(); },
                [&](VAddr write_start, VAddr write_end) {
                    for (std::size_t i = first; i < last; ++i) {
                        const auto& copy = copies[i];
                        const VAddr copy_start = readback.arena_base + copy.srcOffset;
                        const VAddr lo = std::max<VAddr>(copy_start, write_start);
                        const VAddr hi = std::min<VAddr>(copy_start + copy.size, write_end);
                        if (lo < hi) {
                            memory->TryWriteBacking(reinterpret_cast<void*>(lo),
                                mapped + copy.dstOffset + (lo - copy_start), hi - lo);
                        }
                    }
                });
        });
    if (!applied) {
        return false;
    }
    // Only the rejected bytes go back into the dirty set. Valid pages are already
    // published/unprotected, and can contain newer CPU writes by the time we retry.
    u64 retry_bytes = 0, kept_bytes = 0;
    for (const auto& copy : readback.retry_copies) {
        gpu_modified_ranges.Add(readback.arena_base + copy.srcOffset, copy.size);
        retry_bytes += copy.size;
    }
    if (readback.partial && ReadbackStatsEnabled()) {
        for (const auto& copy : copies) kept_bytes += copy.size;
        Readbacks().mapped_partial.fetch_add(1);
        Readbacks().mapped_partial_kept.fetch_add(kept_bytes);
        Readbacks().mapped_partial_retry.fetch_add(retry_bytes);
    }
    readback.applied.store(true, std::memory_order_release);
    if (readback.mapped && ReadbackStatsEnabled()) {
        Readbacks().mapped_applied.fetch_add(1);
        Readbacks().mapped_bytes.fetch_add(readback.snapshot_size);
    }
    return true;
}

void BufferCache::RecoverDeferredReadback(DeferredReadback& readback) {
    std::scoped_lock lock{readback.mutex};
    if (readback.Done()) {
        return;
    }
    // Its ranges left gpu_modified_ranges when the copy was recorded. The copy won't be used:
    // they are downloaded again, with whatever the GPU wrote since.
    for (const auto& copy : readback.copies) {
        gpu_modified_ranges.Add(readback.arena_base + copy.srcOffset, copy.size);
    }
    readback.recovered.store(true, std::memory_order_release);
}

void BufferCache::SettleDeferredReadbacks(VAddr start, VAddr end, u64 wait_tick_limit) {
    if (!deferred_count.load(std::memory_order_acquire)) {
        return;
    }
    boost::container::small_vector<std::shared_ptr<DeferredReadback>, 8> overlapping;
    {
        std::scoped_lock lock{deferred_mutex};
        for (const auto& readback : deferred_readbacks) {
            if (!readback->Done() && readback->CopiesTouchPages(start, end)) {
                overlapping.push_back(readback);
            }
        }
    }
    for (const auto& readback : overlapping) {
        const bool wider_wait = readback->mapped && wait_tick_limit &&
            readback->tick > wait_tick_limit &&
            !scheduler.GetWorkSemaphore()->IsFree(readback->tick);
        if (wider_wait && ReadbackStatsEnabled()) Readbacks().mapped_wider_wait_bypassed.fetch_add(1);
        // A stale copy won't be applied: no need to wait for it (2d2511fe).
        const bool changed_in_flight = readback->page_scoped &&
            !readback->invalid_pages.Empty() &&
            !scheduler.GetWorkSemaphore()->IsFree(readback->tick);
        // Do not turn the old no-wait stale recovery into a GPU-owner stall.
        // Salvage partial snapshots only when ready; otherwise recollect conservatively.
        if (wider_wait || readback->stale.load() || changed_in_flight ||
            !FinishDeferredReadback(*readback, true)) {
            RecoverDeferredReadback(*readback);
        }
    }
}

void BufferCache::PruneDeferredReadbacks() {
    if (!deferred_count.load(std::memory_order_acquire)) {
        return;
    }
    boost::container::small_vector<std::shared_ptr<DeferredReadback>, 8> stale;
    {
        std::scoped_lock lock{deferred_mutex};
        for (const auto& readback : deferred_readbacks) {
            if (!readback->Done() && readback->stale.load()) {
                stale.push_back(readback);
            }
        }
    }
    for (const auto& readback : stale) {
        RecoverDeferredReadback(*readback);
    }
    std::scoped_lock lock{deferred_mutex};
    std::erase_if(deferred_readbacks, [this](const std::shared_ptr<DeferredReadback>& readback) {
        if (!readback->Done()) {
            return false;
        }
        // Freed for reuse once the current tick completes; a guest thread still holding the
        // object only looks at its flags (applied or recovered) from now on.
        if (!readback->mapped) staging_pool.FreeDeferred(readback->staging);
        return true;
    });
    deferred_count.store(static_cast<u32>(deferred_readbacks.size()), std::memory_order_release);
}

void BufferCache::MarkDeferredStale(VAddr addr, u64 size, bool cpu) {
    if (!deferred_count.load(std::memory_order_acquire)) {
        return;
    }
    boost::container::small_vector<std::shared_ptr<DeferredReadback>, 4> marked;
    {
        std::scoped_lock lock{deferred_mutex};
        for (const auto& readback : deferred_readbacks) {
            if (!readback->Done() && readback->CopiesTouchPages(addr, addr + size)) {
                if (!cpu && readback->page_scoped &&
                    EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Relaxed &&
                    !EmulatorSettings.IsDirectMemoryAccessEnabled()) {
                    readback->invalid_pages.Add({Common::AlignDown(addr, u64{4096}),
                                                 Common::AlignUp(addr + size, u64{4096})});
                } else {
                    readback->stale.store(true);
                }
                if (cpu) {
                    marked.push_back(readback);
                }
            }
        }
    }
    // An apply already past its stale check finishes before the guest writes.
    for (const auto& readback : marked) {
        std::scoped_lock lock{readback->mutex};
    }
}

// A store fault may release its whole page only when ALL GPU-dirty bytes on that page
// are replaced. Otherwise keep the normal readback: trapping every following 16-byte store
// would turn one page fault into hundreds of owner commands.
bool BufferCache::CpuStoreOverwrite(VAddr dst, const void* src, u64 size) {
#if defined(__ANDROID__) && defined(ISS_ANDROID_BIONIC)
    static const bool enabled = [] {
        const char* global = std::getenv("TLG_WRITE_MERGE");
        const char* native = std::getenv("TLG_NATIVE_STORE_MERGE");
        return global && *global == '1' && !(native && *native == '0');
    }();
    if (!enabled || !src || size != 16 || (dst & 15) || dst >= (1ULL << 40) ||
        EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Relaxed ||
        EmulatorSettings.IsDirectMemoryAccessEnabled()) return false;
    const VAddr page = Common::AlignDown(dst, u64{4096});
    const VAddr page_end = page + 4096;
    std::array<u8, 16> bytes;
    std::memcpy(bytes.data(), src, bytes.size());
    bool applied = false;
    // Image publications precede the buffer readback, just as in OnWriteFault.
    // Resolve on the caller so an image wait does not block the command owner.
    texture_cache.ResolveReadbacks(dst, size, false, ReadbackSource::CpuWrite);
    ReadMemory(dst, size, true, false, [&]() -> bool {
        // A shared completion may already have made this request coherent while
        // it was queued. Do not start a prefetch of unrelated neighboring pages.
        if (!memory_tracker->IsRegionGpuModified(dst, size)) return true;
        if (eager_count.load(std::memory_order_acquire)) return false;
        bool any = false, covered = true;
        const auto check = [&](VAddr begin, VAddr end) {
            const auto a = std::max(begin, page), b = std::min(end, page_end);
            if (a >= b) return;
            any = true;
            if (a < dst || b > dst + size) covered = false;
        };
        gpu_modified_ranges.ForEachInRange(page, 4096, check);
        // Pending snapshots have removed their ranges from gpu_modified_ranges. Include them
        // in the proof, even when stale, or an unfinished publication could hide GPU bytes.
        {
            std::scoped_lock lock{deferred_mutex};
            for (const auto& readback : deferred_readbacks) {
                if (readback->Done()) continue;
                for (const auto& copy : readback->copies) {
                    const VAddr at = readback->arena_base + copy.srcOffset;
                    check(at, at + copy.size);
                }
            }
        }
        if (!any || !covered) {
            if (ReadbackStatsEnabled()) Readbacks().cpu_store_coverage_rejected.fetch_add(1);
            return false;
        }
        // The entire page will become writable; outstanding host reads anywhere on it must
        // finish before releasing protection, not only host reads of these first 16 bytes.
        scheduler.WaitHostCopiesFor(page, 4096);
        scheduler.WaitDeferredSignals();
        texture_cache.ResolveReadbacks(page, 4096, true, ReadbackSource::CpuWrite);
        MarkEagerStale(page, 4096);
        MarkDeferredStale(page, 4096, true);
        SettleDeferredReadbacks(page, page_end); // stale copies recover without a GPU wait
        if (!memory->TryCpuWriteBacking(reinterpret_cast<void*>(dst), bytes.data(), size)) {
            if (ReadbackStatsEnabled()) Readbacks().cpu_store_backing_rejected.fetch_add(1);
            return false;
        }
        gpu_modified_ranges.Subtract(page, 4096);
        memory_tracker->UnmarkRegionAsGpuModified(page, 4096);
        memory_tracker->MarkRegionAsCpuModified(page, 4096);
        texture_cache.InvalidateMemory(dst, size);
        applied = true;
        return true;
    });
    if (ReadbackStatsEnabled()) {
        (applied ? Readbacks().cpu_store_applied : Readbacks().cpu_store_fallback).fetch_add(1);
        ReportReadbacks();
    }
    return applied;
#else
    return false;
#endif
}

bool BufferCache::CpuWriteMergeRelaxed(VAddr dst, const void* src, int fill, u64 size) {
#if defined(__ANDROID__) && defined(ISS_ANDROID_BIONIC)
    static const bool enabled = [] {
        const char* env = std::getenv("TLG_RELAXED_WRITE_MERGE");
        return !(env && *env == '0');
    }();
    // Bound the extra CPU snapshot. Aliases/permissions are checked atomically by
    // TryCpuWriteBacking; overlapping memcpy must keep the normal libc contract.
    if (!enabled || EmulatorSettings.IsDirectMemoryAccessEnabled() ||
        size < 64 || size > (4ULL << 20) || dst + size < dst) return false;
    const auto source = reinterpret_cast<VAddr>(src);
    if (src && (source + size < source || (dst < source + size && source < dst + size))) return false;
    std::vector<u8> payload(size);
    // Read a protected source on the caller, before entering the owner queue. A
    // source-image readback may itself fault/wait and cannot run on the cache owner.
    if (src) std::memcpy(payload.data(), src, size);
    else std::fill(payload.begin(), payload.end(), static_cast<u8>(fill));
    bool applied = false;
    liverpool->SendCommand<true, true>([&] {
        if (EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Relaxed ||
            !memory_tracker->IsRegionGpuModified(dst, size)) return;
        // Earlier CPU-side uploads/signals may still read/write this backing.
        // Finish overlapping host copies before replacing their source bytes.
        scheduler.WaitHostCopiesFor(dst, size);
        scheduler.WaitDeferredSignals();
        // Images may have a pending publication over the same bytes. Settle it
        // before overwriting them, as the ordinary CPU-write fault does.
        texture_cache.ResolveReadbacks(dst, size, true, ReadbackSource::CpuWrite);
        MarkEagerStale(dst, size);
        MarkDeferredStale(dst, size, true);
        if (!memory->TryCpuWriteBacking(reinterpret_cast<void*>(dst), payload.data(), size)) return;
        // No owner readback/upload can enter between the store and registration.
        // Do NOT mark the whole page CPU dirty: its untouched GPU bytes must survive.
        AddMergedWriteRange(dst, size);
        texture_cache.InvalidateMemory(dst, size);
        applied = true;
    });
    if (ReadbackStatsEnabled()) {
        if (applied) {
            Readbacks().cpu_merge_applied.fetch_add(1);
            Readbacks().cpu_merge_bytes.fetch_add(size);
            ReportReadbacks();
        } else Readbacks().cpu_merge_rejected.fetch_add(1);
    }
    return applied;
#else
    return false;
#endif
}

bool BufferCache::CpuWriteMerge(VAddr dst, const void* src, int fill, u64 size) {
    if (!size || dst >= (1ULL << 40) || size > (1ULL << 40) - dst) return false;
    static const bool enabled = [] {
        const char* e = std::getenv("TLG_WRITE_MERGE");
        return e && *e == '1';
    }();
    if (!enabled || size < 16 || !memory_tracker->IsRegionGpuModified(dst, size)) {
        return false;
    }
#if defined(__ANDROID__) && defined(ISS_ANDROID_BIONIC)
    if (EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Relaxed) {
        return CpuWriteMergeRelaxed(dst, src, fill, size);
    }
#endif
    if (EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Precise) return false;
    // The copies of these pages made earlier (eager, deferred) lack the bytes about to be
    // written: drop them before the write, waiting for an apply in progress, so none lands
    // over the new bytes (guest thread: only marks them, gpu_modified_ranges belongs to the
    // GPU-side threads).
    MarkEagerStale(dst, size);
    MarkDeferredStale(dst, size, true);
    if (src) {
        if (!memory->TryWriteBacking(reinterpret_cast<void*>(dst), src, size)) {
            return false;
        }
    } else {
        std::array<u8, 4096> chunk;
        chunk.fill(static_cast<u8>(fill));
        for (u64 done = 0; done < size;) {
            const u64 n = std::min<u64>(chunk.size(), size - done);
            if (!memory->TryWriteBacking(reinterpret_cast<void*>(dst + done), chunk.data(), n)) {
                return done == 0 ? false : (std::memset(reinterpret_cast<void*>(dst + done), fill, size - done), true);
            }
            done += n;
        }
    }
    AddMergedWriteRange(dst, size);
    return true;
}

void BufferCache::AddMergedWriteRange(VAddr dst, u64 size) {
    std::scoped_lock lock{merge_mutex};
    VAddr start = dst, end = dst + size;
    auto it = merged_writes.upper_bound(start);
    if (it != merged_writes.begin() && std::prev(it)->second >= start) {
        --it;
        start = it->first;
        end = std::max(end, it->second);
        it = merged_writes.erase(it);
    }
    while (it != merged_writes.end() && it->first <= end) {
        end = std::max(end, it->second);
        it = merged_writes.erase(it);
    }
    merged_writes.emplace(start, end);
    merged_count.store(merged_writes.size(), std::memory_order_release);
}

void BufferCache::FlushMergedWrites(const Buffer* arena, VAddr device_addr, u64 size) {
    if (!merged_count.load(std::memory_order_acquire)) {
        return;
    }
    const VAddr lo = std::max<VAddr>(device_addr, arena->cpu_addr);
    const VAddr hi = std::min<VAddr>(device_addr + size, arena->cpu_addr + arena->size_bytes);
    if (lo >= hi) {
        return;
    }
    boost::container::small_vector<std::pair<VAddr, VAddr>, 16> ranges;
    {
        std::scoped_lock lock{merge_mutex};
        auto it = merged_writes.upper_bound(lo);
        if (it != merged_writes.begin() && std::prev(it)->second > lo) {
            --it;
        }
        while (it != merged_writes.end() && it->first < hi) {
            const VAddr a = it->first, b = it->second;
            const VAddr s = std::max(a, lo), e = std::min(b, hi);
            ranges.emplace_back(s, e);
            it = merged_writes.erase(it);
            if (a < s) merged_writes.emplace(a, s);
            if (e < b) it = merged_writes.emplace(e, b).first, ++it;
        }
        merged_count.store(merged_writes.size(), std::memory_order_release);
    }
    if (ranges.empty()) {
        return;
    }
    for (const auto& [s, e] : ranges) {
        NoteGpuWriteForEager(s, e - s); // the GPU copy changes: an older eager copy is stale
    }
    u64 total = 0;
    for (const auto& [s, e] : ranges) {
        total += Common::AlignUp(e - s, 16);
    }
    const auto staging = staging_pool.Request(total, MemoryType::HostUncached);
    boost::container::small_vector<vk::BufferCopy, 16> copies;
    u64 offset = 0;
    for (const auto& [s, e] : ranges) {
        memory->CopySparseMemory(s, staging.mapped + offset, e - s);
        copies.push_back(vk::BufferCopy{.srcOffset = staging.offset + offset,
                                        .dstOffset = s - arena->cpu_addr,
                                        .size = e - s});
        offset += Common::AlignUp(e - s, 16);
    }
    staging.Flush();
    runtime.CopyBuffer(staging.buffer, arena, copies);
}

bool BufferCache::SynchronizeMemory(const Buffer* arena, VAddr device_addr, u32 size,
                                    bool is_written, bool is_texel_buffer) {
    FlushMergedWrites(arena, device_addr, size);
    boost::container::small_vector<vk::BufferCopy, 4> copies;
    size_t total_size_bytes{};
    const Buffer* src_buffer{};
    memory_tracker->ForEachUploadRange(
        device_addr, size, is_written,
        [&](u64 addr, u64 size) {
            copies.emplace_back(total_size_bytes, addr, size);
            total_size_bytes += size;
        },
        [&] { src_buffer = UploadCopies(arena, copies, total_size_bytes); });

    if (src_buffer) {
        runtime.CopyBuffer(src_buffer, arena, copies);
    }
    if (is_texel_buffer && !is_written) {
        return SynchronizeMemoryFromImage(arena, device_addr, size);
    }
    return false;
}

const Buffer* BufferCache::UploadCopies(const Buffer* arena, std::span<vk::BufferCopy> copies,
                                        size_t total_size_bytes) {
    if (copies.empty()) {
        return nullptr;
    }
    // Resolve before launching host copies: a copy-thread fault could deadlock submission.
    for (const auto& copy : copies) {
        texture_cache.ResolveReadbacks(copy.dstOffset, copy.size, true, ReadbackSource::BufferUpload);
    }
    TlgStats::buffer_upload_bytes.fetch_add(total_size_bytes, std::memory_order_relaxed);
    if (total_size_bytes > MAX_UPLOAD_PART_BYTES) {
        // Recorded here, part by part: nothing left for the caller to copy.
        UploadInParts(arena, copies);
        return nullptr;
    }
    const auto staging = staging_pool.Request(total_size_bytes, MemoryType::HostUncached);
    // tlg: the guest memory is copied into staging on the copy threads, started now in
    // groups of about 1 MiB; submission and guest-visible fences wait for them
    // (Scheduler::WaitHostCopies).
    if (!TlgToggle::Disabled(TlgToggle::DeferredUploads) && total_size_bytes < 1_MB) {
        // Small uploads join the calling thread's batch.
        for (auto& copy : copies) {
            SmallGuestCopy({
                .run = &RunGuestCopy,
                .context = this,
                .source = copy.dstOffset,
                .destination = reinterpret_cast<u64>(staging.mapped + copy.srcOffset),
                .size = copy.size,
                .extra = reinterpret_cast<u64>(staging.buffer),
            });
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
        }
        return staging.buffer;
    }
    if (!TlgToggle::Disabled(TlgToggle::DeferredUploads)) {
        struct HostCopy {
            VAddr source;
            u8* destination;
            u64 size;
        };
        using Group = boost::container::small_vector<HostCopy, 8>;
        auto group = std::make_shared<Group>();
        u64 group_bytes = 0;
        const auto launch = [&] {
            TlgCopy::Async([group, memory = memory, staging] {
                for (const auto& copy : *group) {
                    memory->CopySparseMemory(copy.source, copy.destination, copy.size);
                }
                staging.Flush();
            });
            for (const auto& copy : *group) {
                scheduler.NoteHostCopySource(copy.source, copy.size);
            }
        };
        for (auto& copy : copies) {
            group->push_back({copy.dstOffset, staging.mapped + copy.srcOffset, copy.size});
            group_bytes += copy.size;
            copy.srcOffset += staging.offset;
            copy.dstOffset -= arena->cpu_addr;
            if (group_bytes >= 1_MB) {
                launch();
                group = std::make_shared<Group>();
                group_bytes = 0;
            }
        }
        if (!group->empty()) {
            launch();
        }
        return staging.buffer;
    }
    const auto copy = [&](std::size_t i) {
        memory->CopySparseMemory(copies[i].dstOffset, staging.mapped + copies[i].srcOffset,
                                 copies[i].size);
    };
    if (total_size_bytes >= 2_MB && copies.size() > 1) {
        TlgCopy::ParallelFor(copies.size(), copy);
    } else {
        for (std::size_t i = 0; i < copies.size(); ++i) {
            copy(i);
        }
    }
    for (auto& copy : copies) {
        copy.srcOffset += staging.offset;
        copy.dstOffset -= arena->cpu_addr;
    }
    staging.Flush();
    return staging.buffer;
}

void BufferCache::UploadInParts(const Buffer* arena, std::span<const vk::BufferCopy> ranges) {
    u64 total_bytes = 0;
    for (const auto& range : ranges) {
        total_bytes += range.size;
    }
    LOG_WARNING(Render, "Uploading {} MB in parts of {} MB", total_bytes >> 20,
                MAX_UPLOAD_PART_BYTES >> 20);
    // Guest memory is copied on this thread (helped by the copy threads), not deferred: the
    // staging memory is reused as soon as the GPU has copied the part.
    struct HostCopy {
        VAddr source;
        u8* destination;
        u64 size;
    };
    boost::container::small_vector<vk::BufferCopy, 8> copies;
    std::vector<HostCopy> host_copies;
    Vulkan::StagingBufferRef staging{};
    u64 part_bytes = 0;
    u32 parts = 0;
    const auto upload_part = [&] {
        TlgCopy::ParallelFor(host_copies.size(), [&](std::size_t i) {
            memory->CopySparseMemory(host_copies[i].source, host_copies[i].destination,
                                     host_copies[i].size);
        });
        staging.Flush();
        runtime.CopyBuffer(staging.buffer, arena, copies);
        copies.clear();
        host_copies.clear();
        part_bytes = 0;
        ++parts;
    };
    // Pieces of a few MB spread a part over the copy threads.
    constexpr u64 PieceBytes = 4_MB;
    for (const auto& range : ranges) {
        for (u64 done = 0; done < range.size;) {
            if (part_bytes == 0) {
                if (parts != 0) {
                    // The staging memory is free again once the GPU has copied the last part.
                    // This thread owns the caches, as for DownloadMemory's Finish.
                    scheduler.Finish();
                }
                staging = staging_pool.Request(MAX_UPLOAD_PART_BYTES, MemoryType::HostUncached);
            }
            const u64 size = std::min(range.size - done, MAX_UPLOAD_PART_BYTES - part_bytes);
            const VAddr addr = range.dstOffset + done;
            copies.push_back(vk::BufferCopy{.srcOffset = staging.offset + part_bytes,
                                            .dstOffset = addr - arena->cpu_addr,
                                            .size = size});
            for (u64 piece = 0; piece < size; piece += PieceBytes) {
                host_copies.push_back({addr + piece, staging.mapped + part_bytes + piece,
                                       std::min(PieceBytes, size - piece)});
            }
            part_bytes += size;
            done += size;
            if (part_bytes == MAX_UPLOAD_PART_BYTES) {
                upload_part();
            }
        }
    }
    if (part_bytes != 0) {
        upload_part();
    }
}

bool BufferCache::SynchronizeMemoryFromImage(const Buffer* arena, VAddr device_addr, u32 size) {
    if (auto type = texture_cache.IsMeta(device_addr)) {
        if (*type == TextureCache::MetaType::HTile) {
            static constexpr u32 ZmaskUncompressed = 0xf;
            runtime.FillBuffer(arena, arena->Offset(device_addr), size, ZmaskUncompressed);
            return true;
        } else {
            LOG_WARNING(Render_Vulkan, "Unhandled metadata type {}", magic_enum::enum_name(*type));
        }
    }
    // tlg: most texel buffers alias no image; remember misses until images change.
    const u64 generation = texture_cache.RegistryGeneration();
    auto& miss = image_miss_cache[((device_addr >> 6) ^ size * 0x9E3779B1u) % image_miss_cache.size()];
    if (miss.address == device_addr && miss.size == size && miss.generation == generation &&
        !TlgToggle::Disabled(TlgToggle::TextureBindingMemo)) {
        return false;
    }
    const ImageId image_id = texture_cache.FindImageFromRange(device_addr, size);
    if (!image_id) {
        miss = {device_addr, size, generation};
        return false;
    }
    // tlg: the lookups above only read what the texture binding helper leaves alone; the
    // copy below changes image state, so the helper finishes first.
    runtime.BeforeImageAccess();
    Image& image = texture_cache.GetImage(image_id);
    if (image.info.props.is_bc_decoded) {
        return false; // tlg: TLG_BC_TINY: decoded texels, not the guest's blocks
    }
    ASSERT_MSG(device_addr == image.info.guest_address,
               "Texel buffer aliases image subresources {:x} : {:x}", device_addr,
               image.info.guest_address);
    const u64 arena_offset = arena->Offset(device_addr);
    boost::container::small_vector<vk::BufferImageCopy, 8> buffer_copies;
    for (u32 mip = 0; mip < image.info.resources.levels; mip++) {
        const auto& mip_info = image.info.mips_layout[mip];
        const u32 width = std::max(image.info.size.width >> mip, 1u);
        const u32 height = std::max(image.info.size.height >> mip, 1u);
        const u32 depth = std::max(image.info.size.depth >> mip, 1u);
        if (arena_offset + mip_info.offset + mip_info.size > arena->size_bytes) {
            break;
        }
        buffer_copies.push_back(vk::BufferImageCopy{
            .bufferOffset = mip_info.offset,
            .bufferRowLength = mip_info.pitch,
            .bufferImageHeight = mip_info.height,
            .imageSubresource{
                .aspectMask = image.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                .mipLevel = mip,
                .baseArrayLayer = 0,
                .layerCount = image.info.resources.layers,
            },
            .imageOffset = {0, 0, 0},
            .imageExtent = {width, height, depth},
        });
    }
    if (buffer_copies.empty()) {
        return false;
    }
    auto& tile_manager = texture_cache.GetTileManager();
    tile_manager.TileImage(image, buffer_copies, arena, arena_offset);
    return true;
}

void BufferCache::SubmitPendingArenaBinds(Vulkan::SubmitInfo& info) {
    if (pending_binds.empty()) {
        return;
    }

    std::vector<vk::SparseBufferMemoryBindInfo> buffer_binds;
    buffer_binds.reserve(pending_binds.size());

    for (const auto& binds : pending_binds) {
        buffer_binds.emplace_back(vk::SparseBufferMemoryBindInfo{
            .buffer = binds.arena->Handle(),
            .bindCount = static_cast<u32>(binds.binds.size()),
            .pBinds = binds.binds.data(),
        });
    }

    const u64 signal_tick = memory_semaphore.NextTick();
    const auto signal_sema = memory_semaphore.Handle();

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .signalSemaphoreValueCount = 1u,
        .pSignalSemaphoreValues = &signal_tick,
    };

    const vk::BindSparseInfo sparse_info = {
        .pNext = &timeline_si,
        .bufferBindCount = static_cast<u32>(buffer_binds.size()),
        .pBufferBinds = buffer_binds.data(),
        .signalSemaphoreCount = 1u,
        .pSignalSemaphores = &signal_sema,
    };

    info.AddWait(signal_sema, signal_tick);
    const auto bind_start = std::chrono::steady_clock::now();
    auto submit_result = instance.GetGraphicsQueue().bindSparse(sparse_info);
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");
    const auto elapsed = std::chrono::steady_clock::now() - bind_start;
    // Sparse binds are reported to get slower the more memory is bound on NVIDIA drivers. Log the
    // slow ones (the first few, then every 32nd) with how much is resident, to confirm the cost.
    if (elapsed >= std::chrono::milliseconds{2}) {
        static u32 slow_binds = 0;
        if (slow_binds < 16 || slow_binds % 32 == 0) {
            u64 bound_blocks{};
            for (const auto& range : resident_ranges) {
                bound_blocks += range.end - range.start;
            }
            LOG_WARNING(Render,
                        "Sparse bind took {:.1f} ms, {} MB resident in {} allocations "
                        "(slow bind #{})",
                        std::chrono::duration<double, std::milli>(elapsed).count(),
                        (bound_blocks << block_shift) >> 20, residency_allocations,
                        slow_binds + 1);
        }
        ++slow_binds;
    }

    pending_binds.clear();
}

} // namespace VideoCore
