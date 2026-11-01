"""Exercise the frame policy used by depth/color draws and FSR dispatch."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class SceneJitter(unittest.TestCase):
    def test_discovery_replacement_and_frame_consistency(self):
        program = r'''
#include <cassert>
#include "video_core/renderer_vulkan/iss_scene_jitter.h"
int main() {
 Vulkan::IssSceneJitter p;
 const Vulkan::IssSceneJitter::Key a{0x10000,640,360,130}, b{0x20000,640,360,130};
 p.BeginFrame(false);
 assert(!p.Applies(a)); // early depth prepass
 p.Observe(a);
 assert(!p.Applies(a)); // late material pass and FSR must also use zero
 p.BeginFrame(false);
 assert(p.Applies(a)); // already known at the first depth draw
 p.Observe(a);
 assert(p.Applies(a));
 assert(!p.Applies(b));
 p.Observe(b); // resource change cannot turn on jitter midway through frame
 assert(!p.Applies(b));
 p.BeginFrame(false);
 assert(p.Applies(b) && !p.Applies(a));
 p.Observe(b);
 p.BeginFrame(true); // settings/output change invalidates the anchor
 assert(!p.Applies(b));
 p.BeginFrame(false); // UI-only frame supplies no scene anchor
 assert(!p.Applies(b));
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path / 'test.cpp').write_text(program)
            subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(ROOT / 'port/gpu/shadps4'),
                            str(path / 'test.cpp'), '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)

    def test_production_consumers_use_the_same_displacement(self):
        gpu = ROOT / 'port/gpu/shadps4/video_core/renderer_vulkan'
        raster = (gpu / 'vk_rasterizer.cpp').read_text()
        scaled = (gpu / 'vk_temporal_upscaler.cpp').read_text().split(
            'void TemporalUpscaler::RunScaled() {', 1)[1]
        self.assertEqual(raster.count('draw_jitter = upscaler->IssGeometryJitter(db_desc.first);'), 2)
        self.assertIn('const auto jitter = IssGeometryJitter(camera_motion.Depth());', scaled)
        self.assertLess(scaled.index('camera_motion.SetEffectiveJitter(jitter);'),
                        scaled.index('camera_motion.RecordMotion('))
        self.assertIn('info.jitterOffset = {sign * jitter[0] + native_jitter_pixels[0]', scaled)


if __name__ == '__main__':
    unittest.main()
