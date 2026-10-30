"""Exercise the production motion SPIR-V with paired surfaces and rejected foreign-layer vectors."""
from array import array
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest


class MotionSurfacePairGpu(unittest.TestCase):
    @unittest.skipUnless(os.environ.get('ISS_MOTION_REPLAY') and os.environ.get('ISS_MOTION_SPIRV'),
                         'requires the standalone Vulkan motion replay harness')
    def test_direct_surface_vectors_and_foreign_layer_rejection(self):
        width, height = 640, 360
        depth = array('f', [.01]) * (width * height)
        objects = array('f', [0]) * (width * height * 4)
        cases = [
            ('foreign_layer_despite_neighbours', 1, .044, (1.2, 0), [(1, 0)] * 3, .05, 1, 0),
            ('no_target_history', 0, .044, (1.2, 0), [(1, 0)] * 3, .05, 1, 0),
            ('foreign_depth', 1, .044, (1.2, 0), [(1, 0)] * 3, .01, 1, 0),
            ('target_outlier', 1, .044, (100, 0), [(1, 0)] * 3, .05, 1, 0),
            ('source_outlier', 1, .044, (1.2, 0), [(20, 0)] * 3, .05, 1, 0),
            ('conflicting_direction', 1, .044, (1.2, 0), [(1, 0), (-1, 0), (0, 1)], .05, 1, 0),
            ('variance', 1, .044, (1.2, 0), [(.5, 0), (1, 0), (1.9, 0)], .05, 1, 0),
            ('no_source_history', 1, .044, (1.2, 0), [(1, 0)] * 3, .05, 0, 0),
            ('static', 1, .044, (.04, 0), [(1, 0)] * 3, .05, 1, 0),
            ('valid_target', 1, .05, (.4, 0), [(1, 0)] * 3, .05, 1, .4),
            ('paired_front_surface', 1, .05, (1.2, 0), [(1, 0)] * 3, .05, 1, 1.2),
            ('nonfinite', 1, .044, (float('nan'), 0), [(1, 0)] * 3, .05, 1, 0),
        ]
        locations = []
        for index, case in enumerate(cases):
            name, valid, tagged, motion, neighbours, source_depth, source_valid, expected = case
            x, y = 20 + (index % 8) * 70, 30 + (index // 8) * 80
            for yy in range(y - 7, y + 8):
                for xx in range(x - 7, x + 8):
                    depth[yy * width + xx] = .05
            at = (y * width + x) * 4
            objects[at:at + 4] = array('f', [*motion, valid, tagged])
            for (dx, dy), velocity in zip([(-1, 0), (0, -1), (1, 0)], neighbours):
                pixel = (y + dy) * width + x + dx
                depth[pixel] = source_depth
                objects[pixel * 4:pixel * 4 + 4] = array('f', [*velocity, source_valid, source_depth])
            locations.append((name, x, y, expected))
        # A mismatched layer at the viewport corner must also be rejected.
        for yy in range(8):
            for xx in range(8):
                depth[yy * width + xx] = .05
        objects[0:4] = array('f', [1.2, 0, 1, .044])
        for xx, yy in ((1, 0), (0, 1), (1, 1)):
            at = (yy * width + xx) * 4
            objects[at:at + 4] = array('f', [1, 0, 1, .05])
        locations.append(('viewport_corner', 0, 0, 0))
        for yy in range(183, 198):
            for xx in range(13, 28):
                depth[yy * width + xx] = .05
        at = (190 * width + 20) * 4
        objects[at:at+4] = array('f', [1.2, 0, 1, .044])
        depth[190 * width + 20] = float('nan')
        for xx, yy in ((19, 190), (20, 189), (21, 190)):
            at = (yy * width + xx) * 4
            objects[at:at+4] = array('f', [1, 0, 1, .05])
        locations.append(('nonfinite_scene_depth', 20, 190, 0))
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for frame in ('f000', 'f001'):
                (root / f'{frame}_depth_640x360_f32.raw').write_bytes(depth.tobytes())
                (root / f'{frame}_object_motion_640x360_rgba32f.raw').write_bytes(objects.tobytes())
                (root / f'{frame}_aa_info.txt').write_text('jitter_render_pixels=0,0\n')
            out = root / 'result'
            subprocess.run([os.environ['ISS_MOTION_REPLAY'], str(root), str(out),
                            os.environ['ISS_MOTION_SPIRV']], check=True)
            result = (out / 'f001_motion_640x360_rg16f.raw').read_bytes()
            for name, x, y, expected in locations:
                with self.subTest(case=name):
                    vx, vy = struct.unpack_from('<2e', result, (y * width + x) * 4)
                    self.assertAlmostEqual(vx, expected, delta=.002)
                    self.assertAlmostEqual(vy, 0, delta=.002)


if __name__ == '__main__':
    unittest.main()
