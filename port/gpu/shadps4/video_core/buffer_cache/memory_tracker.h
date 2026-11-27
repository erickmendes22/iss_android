// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <deque>
#include <mutex>
#include <type_traits>
#include <vector>

#include "tlg_toggles.h"
#include "common/debug.h"
#include "common/types.h"
#include "core/emulator_settings.h"
#include "video_core/buffer_cache/region_manager.h"

namespace VideoCore {

class MemoryTracker {
public:
    static constexpr size_t MAX_CPU_PAGE_BITS = 40;
    static constexpr size_t NUM_HIGH_PAGES = 1ULL << (MAX_CPU_PAGE_BITS - TRACKER_HIGHER_PAGE_BITS);
    static constexpr size_t MANAGER_POOL_SIZE = 32;

public:
    explicit MemoryTracker(PageManager& tracker_) : tracker{&tracker_} {}
    ~MemoryTracker() = default;

    /// Returns true if a region has been modified from the CPU
    bool IsRegionCpuModified(VAddr query_cpu_addr, u64 query_size) noexcept {
        return IteratePages<true>(
            query_cpu_addr, query_size, [](RegionManager* manager, u64 offset, size_t size) {
                return manager->template IsRegionModified<Type::CPU>(offset, size);
            });
    }

    /// Returns true if a region has been modified from the GPU
    bool IsRegionGpuModified(VAddr query_cpu_addr, u64 query_size) noexcept {
        return IteratePages<false>(
            query_cpu_addr, query_size, [](RegionManager* manager, u64 offset, size_t size) {
                return manager->template IsRegionModified<Type::GPU>(offset, size);
            });
    }

    /// Mark region as CPU modified, notifying the device_tracker about this change
    void MarkRegionAsCpuModified(VAddr dirty_cpu_addr, u64 query_size) {
        IteratePages<false>(dirty_cpu_addr, query_size,
                            [](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->template ChangeRegionState<Type::CPU, true>(
                                    manager->GetCpuAddr() + offset, size);
                            });
    }

    /// tlg: as MarkRegionAsCpuModified, for a write fault served by a copy applied on the
    /// faulting thread: pages the GPU marked modified again since are left alone, so the write
    /// faults again and reads them back first instead of the upload taking stale guest bytes.
    void MarkRegionAsCpuModifiedUnlessGpu(VAddr dirty_cpu_addr, u64 query_size) {
        IteratePages<false>(dirty_cpu_addr, query_size,
                            [](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                if (!manager->template IsRegionModified<Type::GPU>(offset, size)) {
                                    manager->template ChangeRegionState<Type::CPU, true>(
                                        manager->GetCpuAddr() + offset, size);
                                }
                            });
    }

    /// tlg: see RegionManager::ExtendWriteFault.
    void ExtendWriteFault(VAddr window_addr, u64 size) {
        IteratePages<false>(window_addr, size,
                            [](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->ExtendWriteFault(manager->GetCpuAddr() + offset, size);
                            });
    }

    /// Unmark region as modified from the host GPU
    void UnmarkRegionAsGpuModified(VAddr dirty_cpu_addr, u64 query_size) noexcept {
        IteratePages<false>(dirty_cpu_addr, query_size,
                            [](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->template ChangeRegionState<Type::GPU, false>(
                                    manager->GetCpuAddr() + offset, size);
                            });
    }

    /// tlg: applies a copy of GPU data made earlier (deferred and eager readbacks, buffer_cache.cpp).
    /// With the region locks of the range held (taken in address order, as ForEachUploadRange
    /// does), and only if pred() holds: write(start, end) is called for each run of pages still
    /// marked GPU modified, then the range is unmarked (which lifts the read protection). The GPU
    /// side marks the copy outdated before it marks the memory GPU modified again, under these
    /// locks, so a newer GPU write is never unmarked. Pages no longer marked were brought back
    /// by someone else and may hold newer CPU writes since: they are not written. Returns pred().
    template <typename Pred, typename Write>
    bool ApplyGpuCopyIf(VAddr cpu_addr, u64 size, Pred&& pred, Write&& write) {
        struct Segment {
            RegionManager* manager;
            u64 offset;
            size_t size;
        };
        std::array<Segment, 8> segments;
        size_t count = 0;
        bool too_large = false;
        IteratePages<false>(cpu_addr, size, [&](RegionManager* manager, u64 offset, size_t size) {
            if (count == segments.size()) {
                too_large = true;
                return;
            }
            segments[count++] = {manager, offset, size};
        });
        if (too_large) {
            return false; // over 8 regions: never a readback window; the caller downloads instead
        }
        for (size_t i = 0; i < count; ++i) {
            segments[i].manager->lock.lock();
        }
        const bool apply = pred();
        if (apply) {
            for (size_t i = 0; i < count; ++i) {
                const auto& [manager, offset, seg_size] = segments[i];
                const u64 base = manager->GetCpuAddr();
                const u64 first = offset / TRACKER_BYTES_PER_PAGE * TRACKER_BYTES_PER_PAGE;
                u64 run_start = 0;
                bool in_run = false;
                for (u64 page = first; page < offset + seg_size; page += TRACKER_BYTES_PER_PAGE) {
                    const bool modified =
                        manager->template IsRegionModified<Type::GPU>(page, TRACKER_BYTES_PER_PAGE);
                    if (modified && !in_run) {
                        run_start = std::max<u64>(page, offset);
                        in_run = true;
                    } else if (!modified && in_run) {
                        write(base + run_start, base + page);
                        in_run = false;
                    }
                }
                if (in_run) {
                    write(base + run_start, base + offset + seg_size);
                }
                manager->template ChangeRegionState<Type::GPU, false>(base + offset, seg_size);
            }
        }
        for (size_t i = count; i-- > 0;) {
            segments[i].manager->lock.unlock();
        }
        return apply;
    }

    /// Removes all protection from a page and ensures GPU data has been flushed if requested
    void InvalidateRegion(VAddr cpu_addr, u64 size, auto&& on_flush) noexcept {
        IteratePages<false>(
            cpu_addr, size, [&on_flush](RegionManager* manager, u64 offset, size_t size) {
                const bool should_flush = [&] {
                    // Perform both the GPU modification check and CPU state change with the lock
                    // in case we are racing with GPU thread trying to mark the page as GPU
                    // modified. If we need to flush the flush function is going to perform CPU
                    // state change.
                    std::scoped_lock lk{manager->lock};
                    if (EmulatorSettings.GetReadbacksMode() != GpuReadbacksMode::Disabled &&
                        manager->template IsRegionModified<Type::GPU>(offset, size)) {
                        return true;
                    }
                    manager->template ChangeRegionState<Type::CPU, true>(
                        manager->GetCpuAddr() + offset, size);
                    return false;
                }();
                if (should_flush) {
                    on_flush();
                }
            });
    }

    /// Call 'func' for each CPU modified range and unmark those pages as CPU modified
    void ForEachUploadRange(VAddr query_cpu_range, u64 query_size, bool is_written, auto&& func,
                            auto&& on_upload) {
        IteratePages<true>(query_cpu_range, query_size,
                           [&func, is_written](RegionManager* manager, u64 offset, size_t size) {
                               // tlg: read-only bindings skip the region lock when no page
                               // in range is CPU-modified. A guest write sets its bit in the
                               // fault handler before the store, and the draw that uses the
                               // data reaches this thread through the submission queue lock.
                               if (!is_written &&
                                   !TlgToggle::Disabled(TlgToggle::LockFreeUploadCheck) &&
                                   !manager->template IsRegionModified<Type::CPU>(offset, size)) {
                                   return;
                               }
                               manager->lock.lock();
                               manager->template ForEachModifiedRange<Type::CPU, true>(
                                   manager->GetCpuAddr() + offset, size, func);
                               if (!is_written) {
                                   manager->lock.unlock();
                               }
                           });
        on_upload();
        if (!is_written) {
            return;
        }
        IteratePages<false>(query_cpu_range, query_size,
                            [&func, is_written](RegionManager* manager, u64 offset, size_t size) {
                                manager->template ChangeRegionState<Type::GPU, true>(
                                    manager->GetCpuAddr() + offset, size);
                                manager->lock.unlock();
                            });
    }

    /// Call 'func' for each GPU modified range and unmark those pages as GPU modified
    template <bool clear>
    void ForEachDownloadRange(VAddr query_cpu_range, u64 query_size, auto&& func) {
        IteratePages<false>(query_cpu_range, query_size,
                            [&func](RegionManager* manager, u64 offset, size_t size) {
                                std::scoped_lock lk{manager->lock};
                                manager->template ForEachModifiedRange<Type::GPU, clear>(
                                    manager->GetCpuAddr() + offset, size, func);
                            });
    }

private:
    /**
     * @brief IteratePages Iterates L2 word manager page table.
     * @param cpu_address Start byte cpu address
     * @param size Size in bytes of the region of iterate.
     * @param func Callback for each word manager.
     * @return
     */
    template <bool create_region_on_fail, typename Func>
    bool IteratePages(VAddr cpu_address, size_t size, Func&& func) {
        RENDERER_TRACE;
        using FuncReturn = typename std::invoke_result<Func, RegionManager*, u64, size_t>::type;
        static constexpr bool BOOL_BREAK = std::is_same_v<FuncReturn, bool>;
        std::size_t remaining_size{size};
        std::size_t page_index{cpu_address >> TRACKER_HIGHER_PAGE_BITS};
        u64 page_offset{cpu_address & TRACKER_HIGHER_PAGE_MASK};
        while (remaining_size > 0) {
            const std::size_t copy_amount{
                std::min<std::size_t>(TRACKER_HIGHER_PAGE_SIZE - page_offset, remaining_size)};
            auto* manager{top_tier[page_index]};
            if (manager) {
                if constexpr (BOOL_BREAK) {
                    if (func(manager, page_offset, copy_amount)) {
                        return true;
                    }
                } else {
                    func(manager, page_offset, copy_amount);
                }
            } else if constexpr (create_region_on_fail) {
                CreateRegion(page_index);
                manager = top_tier[page_index];
                if constexpr (BOOL_BREAK) {
                    if (func(manager, page_offset, copy_amount)) {
                        return true;
                    }
                } else {
                    func(manager, page_offset, copy_amount);
                }
            }
            page_index++;
            page_offset = 0;
            remaining_size -= copy_amount;
        }
        return false;
    }

    void CreateRegion(std::size_t page_index) {
        const VAddr base_cpu_addr = page_index << TRACKER_HIGHER_PAGE_BITS;
        if (free_managers.empty()) {
            manager_pool.emplace_back();
            auto& last_pool = manager_pool.back();
            for (size_t i = 0; i < MANAGER_POOL_SIZE; i++) {
                std::construct_at(&last_pool[i], tracker, 0);
                free_managers.push_back(&last_pool[i]);
            }
        }
        // Each manager tracks a 4_MB virtual address space.
        auto* new_manager = free_managers.back();
        new_manager->SetCpuAddress(base_cpu_addr);
        free_managers.pop_back();
        top_tier[page_index] = new_manager;
    }

    PageManager* tracker;
    std::deque<std::array<RegionManager, MANAGER_POOL_SIZE>> manager_pool;
    std::vector<RegionManager*> free_managers;
    std::array<RegionManager*, NUM_HIGH_PAGES> top_tier{};
};

} // namespace VideoCore
