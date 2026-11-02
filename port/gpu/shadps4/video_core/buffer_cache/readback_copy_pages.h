// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstddef>

namespace VideoCore {

// Deferred readbacks copy only GPU-dirty islands from a much wider prefetch window.
// A later CPU/GPU write only invalidates the copy if it touches a page containing
// copied bytes. Page granularity is conservative: memory protection and GPU
// modification tracking both operate on 4 KiB pages.
//
// Copies is a range of VkBufferCopy-like elements with srcOffset and size;
// their destination offsets refer to staging memory and are irrelevant here.
template <typename Copies>
[[nodiscard]] bool ReadbackCopyPagesOverlap(std::uint64_t arena_base, const Copies& copies,
                                            std::uint64_t start, std::uint64_t end) noexcept {
    if (end <= start) {
        return false;
    }
    constexpr unsigned PageShift = 12;
    const std::uint64_t first_page = start >> PageShift;
    const std::uint64_t last_page = (end - 1) >> PageShift;
    for (const auto& copy : copies) {
        if (copy.size == 0) {
            continue;
        }
        const std::uint64_t copy_start = arena_base + copy.srcOffset;
        const std::uint64_t copy_first_page = copy_start >> PageShift;
        const std::uint64_t copy_last_page = (copy_start + copy.size - 1) >> PageShift;
        if (copy_first_page <= last_page && first_page <= copy_last_page) {
            return true;
        }
    }
    return false;
}

// CollectDownloads emits nonempty, ordered, disjoint byte ranges. Their final
// pages are therefore nondecreasing even when several islands share one page.
// Avoid walking every island on each unrelated CPU/GPU write. Tiny lists retain
// the original linear path; no index allocation or snapshot state is needed.
template <typename Copies>
[[nodiscard]] bool OrderedReadbackCopyPagesOverlap(std::uint64_t arena_base,
                                                  const Copies& copies,
                                                  std::uint64_t start,
                                                  std::uint64_t end) noexcept {
    if (copies.size() <= 32) {
        return ReadbackCopyPagesOverlap(arena_base, copies, start, end);
    }
    if (end <= start) {
        return false;
    }
    constexpr unsigned PageShift = 12;
    const std::uint64_t first_page = start >> PageShift;
    const std::uint64_t last_page = (end - 1) >> PageShift;
    const auto& first = copies[0];
    if (last_page < ((arena_base + first.srcOffset) >> PageShift)) {
        return false;
    }
    if (first_page <= ((arena_base + first.srcOffset + first.size - 1) >> PageShift)) {
        return true; // preserve the one-inspection common hit at the first island
    }
    const auto& last = copies[copies.size() - 1];
    if (first_page > ((arena_base + last.srcOffset + last.size - 1) >> PageShift)) {
        return false;
    }
    std::size_t lo = 1, hi = copies.size();
    while (lo < hi) {
        const std::size_t mid = lo + (hi - lo) / 2;
        const auto& copy = copies[mid];
        if (((arena_base + copy.srcOffset + copy.size - 1) >> PageShift) < first_page) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < copies.size() &&
           ((arena_base + copies[lo].srcOffset) >> PageShift) <= last_page;
}

// Visit contiguous sets of pages containing source copies, not an entire
// readback prefetch window. Copies are in ascending guest-address order:
// CollectDownloads walks the modified-region bitmaps and sorted dirty intervals.
// Copy ranges within the same 4 KiB page must be applied *together*, because
// ApplyGpuCopyIf clears the GPU-modified bit for every touched page.
template <typename Copies, typename Visitor>
void ForEachReadbackCopyPageGroup(std::uint64_t arena_base, const Copies& copies,
                                  Visitor&& visit) {
    constexpr std::uint64_t PageMask = ~std::uint64_t{4095};
    std::size_t i = 0;
    while (i < copies.size()) {
        if (!copies[i].size) {
            ++i;
            continue;
        }
        const std::uint64_t first = arena_base + copies[i].srcOffset;
        const std::uint64_t group_begin = first & PageMask;
        std::uint64_t group_end = (first + copies[i].size + 4095) & PageMask;
        std::size_t j = i + 1;
        while (j < copies.size()) {
            if (!copies[j].size) {
                ++j;
                continue;
            }
            const std::uint64_t next_first = arena_base + copies[j].srcOffset;
            if ((next_first & PageMask) > group_end) {
                break;
            }
            const std::uint64_t next_end = (next_first + copies[j].size + 4095) & PageMask;
            if (next_end > group_end) {
                group_end = next_end;
            }
            ++j;
        }
        visit(group_begin, group_end, i, j);
        i = j;
    }
}

} // namespace VideoCore
