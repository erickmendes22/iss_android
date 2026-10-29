# SPDX-License-Identifier: GPL-2.0-or-later
"""Regression for the ISS temporal motion rejection policy from ZIP (16)."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]
MOTION = ROOT / "port/gpu/shadps4/video_core/host_shaders/camera_motion.comp"
TEMPORAL = ROOT / "port/gpu/shadps4/video_core/renderer_vulkan/vk_temporal_upscaler.cpp"

def accept_object_motion(width, height, motion, camera=(0, 0)):
    limit = tuple(max(4.0, v * 0.0125) for v in (width, height))
    return all(abs(obj - cam) < lim for obj, cam, lim in zip(motion, camera, limit))

class TestIssFsr3JitterGuard(unittest.TestCase):
    def test_capture16_stationary_scene_outliers(self):
        self.assertFalse(accept_object_motion(640, 360, (-126.1875, 40.40625)))
        self.assertFalse(accept_object_motion(640, 360, (93, -89.875)))
        self.assertFalse(accept_object_motion(640, 360, (43.25, -6.9)))
        self.assertTrue(accept_object_motion(640, 360, (0.1985, 0.0673)))
    def test_camera_relative_not_global_zero(self):
        self.assertTrue(accept_object_motion(640, 360, (23, -16), (20, -15)))
        self.assertFalse(accept_object_motion(640, 360, (43, -16), (20, -15)))
    def test_source_has_same_bound_in_native_and_fallback(self):
        source = MOTION.read_text()
        self.assertGreaterEqual(source.count("size * 0.0125"), 2)
        self.assertNotIn("max_difference = size * 0.25", source)
        self.assertIn("all(lessThan(abs(object_motion.rg), max_difference))", source)
    def test_detail_reactivity_not_default(self):
        source = TEMPORAL.read_text()
        self.assertIn("if (setting && setting[0] == '1')", source)

if __name__ == "__main__":
    unittest.main()
