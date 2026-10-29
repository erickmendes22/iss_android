"""Exercise configure-then-build patch application on the actual pinned SDK."""
import pathlib
import re
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[3]
MODULE = ROOT / 'port/gpu/third_party/fsr-vulkan'


class FsrPatchStack(unittest.TestCase):
    def test_android_patches_can_be_verified_again_after_configuration(self):
        pinned = subprocess.run(['git', '-C', str(ROOT), 'ls-tree', 'HEAD',
                                 'port/gpu/third_party/fsr-vulkan'],
                                capture_output=True, text=True, check=True).stdout.split()[2]
        available = subprocess.run(['git', '-C', str(MODULE), 'cat-file', '-e', pinned],
                                   capture_output=True)
        if available.returncode:
            self.skipTest('Pinned FidelityFX submodule is not initialized')
        script = (ROOT / 'tools/android/bionic/build_gpu.sh').read_text()
        names = re.search(r'for patch_name in (.+); do', script).group(1).split()
        patches = [ROOT / 'port/gpu/patches/fsr-vulkan' / name for name in names]
        sources = set()
        for patch in patches:
            sources.update(re.findall(r'^diff --git a/(\S+) b/', patch.read_text(), re.M))
        with tempfile.TemporaryDirectory() as temporary:
            directory = pathlib.Path(temporary)
            subprocess.run(['git', 'init', '-q', str(directory)], check=True)
            for name in sources:
                data = subprocess.run(['git', '-C', str(MODULE), 'show', f'{pinned}:{name}'],
                                      capture_output=True, check=True).stdout
                target = directory / name
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
            for phase in ('configure', 'compile'):
                for patch in patches:
                    reverse = subprocess.run(['git', '-C', temporary, 'apply', '--reverse',
                                              '--check', str(patch)], capture_output=True)
                    if reverse.returncode:
                        applied = subprocess.run(['git', '-C', temporary, 'apply', str(patch)],
                                                 capture_output=True, text=True)
                        self.assertEqual(applied.returncode, 0,
                                         f'{phase}: {patch.name}: {applied.stderr}')


if __name__ == '__main__':
    unittest.main()
