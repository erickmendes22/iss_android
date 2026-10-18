// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>

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

} // namespace VideoCore
