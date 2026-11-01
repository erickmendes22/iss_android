"""Exercise real pass selection with inherited MRT bindings and actual PS exports."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
GPU = ROOT / "port/gpu/shadps4/video_core/renderer_vulkan"

class MotionPassEligibility(unittest.TestCase):
    def test_only_effectively_written_guest_mrts_count(self):
        code = r'''#include <cassert>
#include "motion_history.h"
int main() {
 using Vulkan::Motion::GBufferMotionEligible;
 // A real G-buffer pass seeds the visible front surface's motion.
 assert(GBufferMotionEligible(0x1f, 0x1f));
 // A subsequent auxiliary pass inherits all five bound targets but exports
 // only one (or no color at all). The previous bound-only rule accepted it.
 assert(!GBufferMotionEligible(0x1f, 0x01));
 assert(!GBufferMotionEligible(0x1f, 0));
 assert(!GBufferMotionEligible(0x7f, 0x0f));
 // Disabled/unbound targets and our own slot must not inflate the count.
 assert(!GBufferMotionEligible(0x0f, 0x7f));
 assert(!GBufferMotionEligible(0x8f, 0x8f));
 assert(!GBufferMotionEligible(0x1f, 0x3e));
 assert(GBufferMotionEligible(0x7f, 0x3e));
 assert(GBufferMotionEligible(0x7f, 0x7f));
 int vector_owner = 1;
 if (GBufferMotionEligible(0x1f, 0x01)) vector_owner = 2;
 assert(vector_owner == 1);
}'''
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            (p / "test.cpp").write_text(code)
            subprocess.run(["c++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            "-I" + str(GPU), str(p / "test.cpp"), "-o", str(p / "test")], check=True)
            subprocess.run([str(p / "test")], check=True)

    def test_guard_uses_ordinary_shader_before_motion_variant(self):
        src = (GPU / "vk_pipeline_cache.cpp").read_text()
        start = src.index("bool PipelineCache::RefreshGraphicsKey(")
        body = src[start:src.index("bool PipelineCache::RefreshGraphicsStages(", start)]
        first = body.index("if (!RefreshGraphicsStages(sel))")
        guard = body.index("Motion::GBufferMotionEligible(motion_bound_mask, key.mrt_mask)")
        motion = body.index("if (motion_possible)", guard)
        auxiliary = body.index("key.mrt_mask |= 1u << mv")
        self.assertLess(first, guard)
        self.assertLess(guard, motion)
        self.assertLess(motion, auxiliary)
        self.assertIn("#ifdef ISS_ANDROID_BIONIC", body[first:guard])

if __name__ == "__main__":
    unittest.main()
