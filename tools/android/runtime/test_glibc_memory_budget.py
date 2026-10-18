"""Regression checks for Glibc/Turnip's real VK heap-based GC budget."""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
LAUNCH = (ROOT / "tools/android/runtime/launch.py").read_text()
CACHE = (ROOT / "port/gpu/shadps4/video_core/texture_cache/texture_cache.cpp").read_text()
INSTANCE = (ROOT / "port/gpu/shadps4/video_core/renderer_vulkan/vk_instance.cpp").read_text()

class AutoBudgetTests(unittest.TestCase):
    def test_no_forced_gc_cap_in_android_glibc_launcher(self):
        self.assertNotIn('"ISS_GC_BUDGET_MB":', LAUNCH)
        self.assertIn('"ISS_READBACKS": "1"', LAUNCH)
        self.assertIn('"ISS_DEFERRED_READBACK": "1"', LAUNCH)
    def test_driver_heap_budget_drives_integrated_gpu_thresholds(self):
        self.assertIn('const u64 budget = forced_budget ? forced_budget : instance.GetDeviceMemoryBudgetNow();', CACHE)
        self.assertIn('total_used_memory = instance.GetDeviceMemoryUsage();', CACHE)
        self.assertIn('trigger_gc_memory = budget / 10 * 7;', CACHE)
        self.assertIn('pressure_gc_memory = budget / 100 * 85;', CACHE)
        self.assertIn('critical_gc_memory = budget / 100 * 95;', CACHE)
        self.assertIn('if (instance.IsIntegrated() || forced_budget)', CACHE)
        self.assertIn('memory_budget_props.heapBudget[heap]', INSTANCE)
        self.assertIn('memory_budget_props.heapUsage[heap]', INSTANCE)
    def test_explicit_gc_override_not_removed(self):
        self.assertIn('std::getenv("TLG_GC_BUDGET_MB")', CACHE)
        self.assertIn('env.setdefault(key, value)', LAUNCH)

if __name__ == "__main__":
    unittest.main()
