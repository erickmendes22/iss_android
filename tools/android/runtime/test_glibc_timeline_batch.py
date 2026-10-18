"""Android/glibc inFAMOUS staging timeline synchronization regression tests.

Bachata 0.2.5's readback metrics and GPU sync inspired auditing real duplicate
Vulkan timeline queries here. This optimization does not change readback
modes, coherency, GPU fences, staging reuse guards, or game-specific patches.
"""
from pathlib import Path
import unittest

ROOT=Path(__file__).resolve().parents[3]
STAGING=(ROOT/"port/gpu/shadps4/video_core/renderer_vulkan/vk_staging_buffer_pool.cpp").read_text()
STAGING_H=(ROOT/"port/gpu/shadps4/video_core/renderer_vulkan/vk_staging_buffer_pool.h").read_text()
BUFFER=(ROOT/"port/gpu/shadps4/video_core/buffer_cache/buffer.cpp").read_text()
SETTINGS=(ROOT/"port/gpu/shim/core/emulator_settings.h").read_text()

class StagingSyncTests(unittest.TestCase):
    def test_refresh_timeline_only_once_per_gpu_staging_scan(self):
        begin=STAGING.index("bool StagingBufferPool::GpuTickComplete")
        end=STAGING.index("void StagingBufferPool::TickFrame()",begin)
        fn=STAGING[begin:end]
        self.assertIn("#ifdef ISS_ANDROID_GLIBC",fn)
        self.assertIn("if (timeline->IsFree(tick))",fn)
        self.assertIn("if (!refreshed)",fn)
        self.assertEqual(1,fn.count("timeline->Refresh()"))
        self.assertIn("return timeline->IsFree(tick)",fn)
        self.assertIn("return scheduler.IsFree(tick)",fn)
    def test_shared_budget_between_ring_and_dedicated_caches(self):
        tick=STAGING.split("void StagingBufferPool::TickFrame()",1)[1].split("void StagingBufferPool::TrimRing",1)[0]
        self.assertIn("bool refreshed = false;",tick)
        self.assertIn("TrimRing(ring, remaining_deletions, refreshed)",tick)
        self.assertIn("TrimLarge(cache, remaining_deletions, refreshed)",tick)
        self.assertIn("size_t remaining_deletions = 4;",tick)
        self.assertIn("GpuTickComplete(entry.tick, refreshed)",STAGING)
        self.assertIn("GpuTickComplete(block.buffer->LastTick(), refreshed)",STAGING)
        self.assertIn("bool GpuTickComplete(u64 tick, bool& refreshed)",STAGING_H)
    def test_no_redundant_wait_for_completed_staging_ring(self):
        fn=BUFFER.split("bool StreamBuffer::WaitPendingOperations(",1)[1]
        self.assertIn("const bool completed = scheduler.IsFree(watch.tick);",fn)
        self.assertIn("if (!completed)",fn)
        self.assertIn("if (!allow_wait)",fn)
        self.assertIn("scheduler.Wait(watch.tick)",fn)
        self.assertIn("#else",fn)
        self.assertIn("wait_bound = watch.upper_bound;",fn)
    def test_preserve_readback_defaults_and_gpu_coherence(self):
        self.assertIn('Number("TLG_READBACKS", GpuReadbacksMode::Relaxed)',SETTINGS)
        self.assertIn('Flag("TLG_READBACK_LINEAR", false)',SETTINGS)
        CACHE=(ROOT/"port/gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp").read_text()
        self.assertIn("GpuReadbacksMode::Precise",CACHE)
        self.assertIn("scheduler.Finish();",CACHE)
        self.assertIn("ApplyGpuCopyIf",CACHE)

if __name__=="__main__":
    unittest.main()
