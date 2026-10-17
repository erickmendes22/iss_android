"""inFAMOUS Android/glibc: GPU staging eviction is safe and frame-budgeted.

Bachata's core025 buffer collector uses a bounded eviction budget, but its
buffer-cache implementation is incompatible with inFAMOUS's sparse arenas.
This test checks the equivalent *staging* protection only, preserving safety.
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]
SRC = ROOT / "port/gpu/shadps4/video_core/renderer_vulkan/vk_staging_buffer_pool.cpp"
HEADER = ROOT / "port/gpu/shadps4/video_core/renderer_vulkan/vk_staging_buffer_pool.h"
CMAKE = ROOT / "port/gpu/CMakeLists.txt"


class InfamousStagingEvictionBudgetTests(unittest.TestCase):
    def test_android_scope_and_shared_total_budget(self):
        s = SRC.read_text()
        self.assertIn("#ifdef ISS_ANDROID_GLIBC", s)
        self.assertIn("size_t remaining_deletions = 4;", s)
        self.assertIn("std::numeric_limits<size_t>::max()", s)
        self.assertIn("TrimRing(ring, remaining_deletions);", s)
        self.assertIn("TrimLarge(cache, remaining_deletions);", s)
        self.assertIn("target_compile_definitions(tlggpu PRIVATE ISS_ANDROID_GLIBC=1)", CMAKE.read_text())

    def test_gpu_lifetime_guards_preserved(self):
        s = SRC.read_text()
        self.assertIn("scheduler.IsFree(block.buffer->LastTick())", s)
        self.assertIn("scheduler.IsFree(entry.tick)", s)
        self.assertIn("entry.held", s)
        self.assertIn("frame - block.last_used_frame < RING_IDLE_FRAMES", s)
        self.assertIn("frame - entry.last_used_frame < LARGE_IDLE_FRAMES", s)
        self.assertIn("if (remaining_deletions == 0) break;", s)
        self.assertEqual(2, s.count("--remaining_deletions;"))
        h = HEADER.read_text()
        self.assertIn("TrimRing(Ring& ring, size_t& remaining_deletions)", h)
        self.assertIn("TrimLarge(std::vector<LargeBuffer>& cache, size_t& remaining_deletions)", h)


if __name__ == "__main__":
    unittest.main()
