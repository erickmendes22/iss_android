// Compare the production ordered lookup against the original linear predicate.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>
#include "video_core/buffer_cache/readback_copy_pages.h"

struct Copy { std::uint64_t srcOffset, dstOffset, size; };
struct CountedCopies {
    const std::vector<Copy>& values;
    mutable unsigned reads{};
    std::size_t size() const { return values.size(); }
    const Copy& operator[](std::size_t i) const { ++reads; return values[i]; }
    struct Iterator {
        const CountedCopies* range;
        std::size_t i;
        const Copy& operator*() const { return (*range)[i]; }
        Iterator& operator++() { ++i; return *this; }
        bool operator!=(const Iterator& other) const { return i != other.i; }
    };
    Iterator begin() const { return {this, 0}; }
    Iterator end() const { return {this, size()}; }
};

int main() {
    std::mt19937_64 random(0x715373);
    unsigned checked = 0;
    // Real copies are ordered, nonempty and disjoint, but can share page borders
    // and have multiple dirty islands in one page. Check unaligned arena origins too.
    for (const unsigned n : {0u, 1u, 8u, 9u, 16u, 32u, 33u, 128u, 512u}) {
        for (unsigned layout = 0; layout < 8; ++layout) {
            std::vector<Copy> copies;
            std::uint64_t cursor = random() % 64;
            for (unsigned i = 0; i < n; ++i) {
                cursor += random() % 7000;
                const auto size = 1 + random() % 9000;
                copies.push_back({cursor, i * 10000ull, size});
                cursor += size;
            }
            const std::uint64_t base = 0x1040000000ull + random() % 4096;
            for (unsigned q = 0; q < 10000; ++q) {
                const auto start = base - 4096 + random() % (cursor + 8192);
                const auto end = start + (q % 31 == 0 ? 0 : random() % 17000);
                const bool expected = VideoCore::ReadbackCopyPagesOverlap(base, copies, start, end);
                assert(VideoCore::OrderedReadbackCopyPagesOverlap(base, copies, start, end) == expected);
                ++checked;
            }
            // Exact first/last bytes and adjacent pages of every generated island.
            for (const auto& copy : copies) {
                for (const auto addr : {base + copy.srcOffset - 1,
                                       base + copy.srcOffset,
                                       base + copy.srcOffset + copy.size - 1,
                                       base + copy.srcOffset + copy.size}) {
                    assert(VideoCore::OrderedReadbackCopyPagesOverlap(base, copies, addr, addr + 1) ==
                           VideoCore::ReadbackCopyPagesOverlap(base, copies, addr, addr + 1));
                    ++checked;
                }
            }
        }
    }
    std::vector<Copy> fragmented;
    for (unsigned i = 0; i < 128; ++i) fragmented.push_back({i * 8192ull + 16, i * 64ull, 4});
    constexpr std::uint64_t base = 0x10000000;
    CountedCopies before{fragmented}, after{fragmented};
    assert(!VideoCore::ReadbackCopyPagesOverlap(base, before, base + 0x200000, base + 0x200004));
    assert(!VideoCore::OrderedReadbackCopyPagesOverlap(base, after, base + 0x200000, base + 0x200004));
    assert(before.reads == 128 && after.reads == 2);
    std::printf("outside write: linear=%u ordered=%u copy inspections\n", before.reads, after.reads);
    before.reads = after.reads = 0;
    assert(!VideoCore::ReadbackCopyPagesOverlap(base, before, base + 0xfd000, base + 0xfd004));
    assert(!VideoCore::OrderedReadbackCopyPagesOverlap(base, after, base + 0xfd000, base + 0xfd004));
    assert(before.reads == 128 && after.reads <= 11);
    std::printf("internal gap: linear=%u ordered=%u copy inspections; %u differential checks\n",
                before.reads, after.reads, checked);
    // The common first-island hit stays at one inspection even for long lists.
    before.reads = after.reads = 0;
    assert(VideoCore::ReadbackCopyPagesOverlap(base, before, base + 16, base + 20));
    assert(VideoCore::OrderedReadbackCopyPagesOverlap(base, after, base + 16, base + 20));
    assert(before.reads == 1 && after.reads == 1);
    // Lists through 32 copies perform the old work, with no indexing setup.
    fragmented.resize(32); before.reads = after.reads = 0;
    assert(!VideoCore::ReadbackCopyPagesOverlap(base, before, base + 0x200000, base + 0x200004));
    assert(!VideoCore::OrderedReadbackCopyPagesOverlap(base, after, base + 0x200000, base + 0x200004));
    assert(before.reads == after.reads && before.reads == 32);
}
