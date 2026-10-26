"""Driver selection regressions for ISS v0.2 with Bloodborne PanVK."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
JAVA = ROOT / "android/app/src/ro/cobrabm/fexdroid"


class MaliPanvkTests(unittest.TestCase):
    def test_pre_fsr_baseline_and_mali_eligibility(self):
        shell = (ROOT / "tools/android/bionic/run-bionic.sh").read_text()
        self.assertIn("export TLG_UPSCALER=none ISS_UPSCALER=off", shell)
        self.assertNotIn("TLG_JITTER=1", shell)
        settings = (JAVA / "BionicDriverSettings.java").read_text()
        for pattern in ("Build.VERSION.SDK_INT<35", 'new File("/dev/mali0").exists()',
                        'new File("/dev/kgsl-3d0").exists()', '"panvk"'):
            self.assertIn(pattern, settings)
        self.assertIn("ISS_MALI_ELIGIBLE", (JAVA / "BionicGameActivity.java").read_text())

    def test_verified_bloodborne_package(self):
        driver = (ROOT / "tools/android/bionic/stage_panvk.py").read_text()
        self.assertIn("68d4eccc1442cb0bef08228c90127388e68bf6087898abc492c717b9a06ca61f", driver)
        self.assertIn("Bloodborne-Android", driver)
        workflow = (ROOT / ".github/workflows/bionic-experimental.yml").read_text()
        self.assertIn("stage_panvk.py", workflow)
        runtime = (JAVA / "BionicRuntimeInstaller.java").read_text()
        self.assertIn("drivers/panvk/libvulkan_panvk.so", runtime)

    def test_bloodborne_non_sparse_mali_gpu_arenas(self):
        cache = (ROOT / "port/gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp").read_text()
        header = (ROOT / "port/gpu/shadps4/video_core/buffer_cache/buffer_cache.h").read_text()
        buffer = (ROOT / "port/gpu/shadps4/video_core/buffer_cache/buffer.cpp").read_text()
        vk = (ROOT / "port/gpu/shadps4/video_core/renderer_vulkan/vk_platform.h").read_text()
        self.assertIn("non_sparse_arenas = instance.IsMali()", cache)
        self.assertIn("non_sparse_arenas ? MemoryType::Backed : MemoryType::Sparse", cache)
        self.assertIn("MigrateBackedArenas(new_arena", cache)
        self.assertIn("void BufferCache::MigrateBackedArenas", cache)
        self.assertIn("if (non_sparse_arenas) {", cache)
        self.assertIn('ASSERT_MSG(pending_binds.empty()', cache)
        self.assertIn("vk::DeviceMemory memory{}", (ROOT / "port/gpu/shadps4/video_core/buffer_cache/buffer.h").read_text())
        self.assertIn("device.bindBufferMemory(buffer, memory, 0)", buffer)
        self.assertIn("device.freeMemory(memory)", buffer)
        self.assertIn("static constexpr u64 MIN_ARENA_PAGE_BITS = 27", header)
        self.assertIn("std::source_location::current()", vk)

    def test_real_shell_routing(self):
        src = (ROOT / "tools/android/bionic/run-bionic.sh").read_text()
        block = src[src.index("# Family-aware defaults:"):src.index('echo "ISS Android Bionic:')]
        with tempfile.TemporaryDirectory() as temp:
            runtime = Path(temp) / "arm64"
            for family, lib in (("gen8", "libvulkan_freedreno.so"),
                                ("gen7", "libvulkan_freedreno.so"),
                                ("panvk", "libvulkan_panvk.so")):
                file = runtime / "drivers" / family / lib
                file.parent.mkdir(parents=True, exist_ok=True)
                file.write_bytes(b"mock")
            script = ('set -eu\nbase="$TEST_BASE"\nruntime="$base/arm64"\n'
                      'chip="$TEST_CHIP"\n' + block +
                      '\nprintenv ISS_VULKAN_DRIVER || printf system\n')

            def route(chip, selected="auto", mali="0"):
                env = dict(os.environ, TEST_BASE=temp, TEST_CHIP=chip,
                           ISS_BIONIC_DRIVER=selected, ISS_MALI_ELIGIBLE=mali)
                return subprocess.run(["sh", "-c", script], env=env, capture_output=True, text=True)

            for chip, which in (("0x44050001", "gen8"), ("0x43050001", "gen7"),
                                ("0x06030000", "gen7")):
                self.assertEqual(str(runtime / "drivers" / which / "libvulkan_freedreno.so"),
                                 route(chip).stdout.splitlines()[-1])
            self.assertEqual("system", route("unknown").stdout.splitlines()[-1])
            self.assertEqual(str(runtime / "drivers/panvk/libvulkan_panvk.so"),
                             route("unknown", mali="1").stdout.splitlines()[-1])
            self.assertEqual(4, route("unknown", selected="panvk").returncode)
            self.assertEqual(str(runtime / "drivers/panvk/libvulkan_panvk.so"),
                             route("unknown", selected="panvk", mali="1").stdout.splitlines()[-1])


if __name__ == "__main__":
    unittest.main()
