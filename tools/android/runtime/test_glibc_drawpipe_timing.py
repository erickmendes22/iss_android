"""Glibc render hot path must not measure every GPU packet unless requested."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]
PIPE = (ROOT / "port/gpu/shadps4/video_core/renderer_vulkan/vk_draw_pipe.h").read_text()
RASTER = (ROOT / "port/gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp").read_text()
LAUNCH = (ROOT / "tools/android/runtime/launch.py").read_text()

class DrawTimingOptInTests(unittest.TestCase):
    def test_no_implicit_diagnostics_in_android_default(self):
        self.assertNotIn('"ISS_FRAME_STATS":', LAUNCH)
    def test_explicit_value_check_is_shared_with_rasterizer(self):
        self.assertIn('const char* value = std::getenv("TLG_FRAME_STATS")',PIPE)
        self.assertIn("return value && value[0] == '1';", PIPE)
        self.assertNotIn('std::getenv("TLG_FRAME_STATS") != nullptr',RASTER)
        self.assertEqual(RASTER.count("static const bool stats = DrawPipe::CollectTiming();"),3)
    def test_expensive_drain_timing_is_optional_but_wait_retained(self):
        section=PIPE.split("void Drain(u32 reason",1)[1].split("/// Stage A: position",1)[0]
        self.assertIn("if (collect_timing)",section)
        self.assertIn("consumed.load(std::memory_order_acquire) != head",section)
        self.assertIn("SleepUntilDrained()",section)
        self.assertIn("drain_cycles += waited;",section)
    def test_worker_records_commands_without_timing(self):
        section=PIPE.split("void Run(std::stop_token stop)",1)[1]
        self.assertIn("if (CollectTiming())",section)
        self.assertIn("} else {\n                handler(context,",section)
        self.assertIn("consumed.store(at, std::memory_order_seq_cst)",section)
        self.assertIn("NotifyDrain();",section)
        self.assertIn("busy_cycles.fetch_add(",section)

if __name__ == "__main__":
    unittest.main()
