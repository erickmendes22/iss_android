"""Test 4 KiB-precise deferred readback invalidation in the real C++ helper."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
BUFFER = ROOT / "port/gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp"
BUFFER_HDR = ROOT / "port/gpu/shadps4/video_core/buffer_cache/buffer_cache.h"

class ReadbackCopyPagesTests(unittest.TestCase):
    def test_calls_use_copied_pages_not_prefetch_window(self):
        body = BUFFER.read_text()
        self.assertIn("readback->CopiesTouchPages(start, end)", body)
        self.assertIn("readback->CopiesTouchPages(addr, addr + size)", body)
        self.assertIn("ForEachReadbackCopyPageGroup(", body)
        self.assertNotIn("readback.start, readback.end - readback.start", body)
        self.assertIn('readback_copy_pages.h', BUFFER_HDR.read_text())

    @unittest.skipUnless(shutil.which("c++"), "host C++ compiler unavailable")
    def test_cpp_page_overlap_and_unmodified_islands(self):
        code = r'''
#include <cassert>
#include <cstdint>
#include <vector>
#include <utility>
#include "video_core/buffer_cache/readback_copy_pages.h"
struct Copy { uint64_t srcOffset, dstOffset, size; };
int main() {
    constexpr uint64_t base = 0x10000000;
    std::vector<Copy> copies{{0x1010, 0, 16}, {0x45000, 16, 8192}};
    auto overlap = [&](uint64_t first, uint64_t last) {
        return VideoCore::ReadbackCopyPagesOverlap(base, copies, base + first, base + last);
    };
    assert(overlap(0x1000, 0x1100));       // copied bytes
    assert(overlap(0x1f00, 0x2000));       // other bytes in copied 4K page
    assert(!overlap(0x2000, 0x4000));      // different pages inside 512K window
    assert(!overlap(0x44000, 0x45000));    // immediate prior page
    assert(overlap(0x45000, 0x47000));    // copied 8K spans two pages
    assert(!overlap(0x47000, 0x48000));   // immediate following page
    assert(!overlap(0, 0));                // zero-length write
    assert(!overlap(0x10000, 0x11000));   // dirty island absent
    std::vector<Copy> empty{};
    assert(!VideoCore::ReadbackCopyPagesOverlap(base, empty, base, base + 4096));
    std::vector<Copy> partial{{0x1fff, 0, 2}};
    assert(VideoCore::ReadbackCopyPagesOverlap(base, partial, base + 0x2000, base + 0x2001));
    assert(VideoCore::ReadbackCopyPagesOverlap(base, partial, base + 0x1000, base + 0x1001));
    // Noncontiguous dirty islands in the same readback window must not
    // clear the GPU-dirty flag of untouched intermediate pages.
    std::vector<Copy> islands{{0x1010, 0, 16}, {0x1800, 16, 8},
                              {0x9000, 24, 32}, {0xa000, 56, 4096}};
    std::vector<std::pair<uint64_t,uint64_t>> spans;
    std::vector<std::pair<size_t,size_t>> indices;
    VideoCore::ForEachReadbackCopyPageGroup(base, islands,
        [&](uint64_t a, uint64_t b, size_t i, size_t j) {
            spans.emplace_back(a-base,b-base);
            indices.emplace_back(i,j);
        });
    assert((spans == std::vector<std::pair<uint64_t,uint64_t>>{
        {0x1000,0x2000}, {0x9000,0xb000}
    }));
    assert((indices == std::vector<std::pair<size_t,size_t>>{
        {0,2}, {2,4}
    }));
}
'''
        with tempfile.TemporaryDirectory() as temp:
            src = Path(temp) / "readback_pages.cpp"
            out = Path(temp) / "readback_pages"
            src.write_text(code)
            subprocess.run(["c++", "-std=c++20", "-O2",
                            "-I", str(ROOT / "port/gpu/shadps4"),
                            str(src), "-o", str(out)],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(out)], check=True, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()
