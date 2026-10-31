"""Compile/test the Bionic graphics fast-bind guard and source integration."""
from pathlib import Path
import shutil
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
RENDERER = ROOT / "port/gpu/shadps4/video_core/renderer_vulkan"

class FastGraphicsBindTest(unittest.TestCase):
    def test_exact_hit_and_invalidation(self):
        cxx = shutil.which(os.environ.get("CXX", "c++"))
        if not cxx:
            self.skipTest("C++ compiler unavailable")
        code = r"""
#include <cstdint>
#include "video_core/renderer_vulkan/vk_fast_pipeline_memo.h"
int main() {
    Vulkan::FastGraphicsPipelineMemo<std::uint64_t, std::uint64_t> memo;
    if (!memo.NeedsBind(4, 9, 11, 1)) return 1;
    if (memo.NeedsBind(4, 9, 11, 1)) return 2;
    if (!memo.NeedsBind(4, 10, 11, 1)) return 3;
    if (!memo.NeedsBind(4, 9, 11, 1)) return 4;
    if (!memo.NeedsBind(5, 9, 11, 1)) return 5;
    if (!memo.NeedsBind(5, 9, 12, 1)) return 6;
    if (!memo.NeedsBind(5, 9, 12, 2)) return 7;
    if (memo.NeedsBind(5, 9, 12, 2)) return 8;
    memo.Invalidate();
    if (!memo.NeedsBind(5, 9, 12, 2)) return 9;
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as temp:
            p = Path(temp)
            (p / "test.cpp").write_text(code)
            subprocess.run([cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "port/gpu/shadps4"),
                            str(p / "test.cpp"), "-o", str(p / "memo")],
                           check=True, timeout=40)
            subprocess.run([str(p / "memo")], check=True, timeout=10)

    def test_gpu_recording_stream_is_protected(self):
        raster = (RENDERER / "vk_rasterizer.cpp").read_text()
        sched = (RENDERER / "vk_scheduler.cpp").read_text()
        scene = (RENDERER / "vk_scene_resolution.cpp").read_text()
        java = (ROOT / "android/app/src/ro/cobrabm/fexdroid/BionicRendererSettings.java").read_text()
        self.assertIn("FastBindGraphicsPipeline(cmdbuf, handle,", raster)
        self.assertIn("FastBindGraphicsPipeline(cmdbuf, square,", raster)
        self.assertIn("FastBindGraphicsPipeline(cmdbuf, bound,", raster)
        self.assertIn('ISS_FAST_PIPELINE_BIND', raster)
        self.assertIn("InvalidateGuestGraphicsBindings()", scene)
        self.assertIn("descriptor_stream_epoch.fetch_add", sched)
        self.assertIn("renderer_fast_pipeline_bind", java)

if __name__ == "__main__":
    unittest.main()
