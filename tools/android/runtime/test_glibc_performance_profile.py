"""Android/glibc performance invariants: no accidental GPU correctness tradeoffs.

The production APK must be optimized and frame diagnostics opt-in. The Bionic
branch and GPU readback semantics are intentionally out of scope.
"""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]
BUILD = (ROOT / "tools/android/build/build_arm64.sh").read_text()
CMAKE = (ROOT / "port/gpu/CMakeLists.txt").read_text()
LAUNCH = (ROOT / "tools/android/runtime/launch.py").read_text()
TOGGLES = (ROOT / "port/gpu/shim/tlg_toggles.h").read_text()
SETTINGS = (ROOT / "port/gpu/shim/tlg_settings.h").read_text()
EMULATOR = (ROOT / "port/gpu/shim/core/emulator_settings.h").read_text()
SHELL = (ROOT / "tools/android/device/run-iss.sh").read_text()

class PerformanceProfileTests(unittest.TestCase):
    def test_glibc_gpu_release_ipo_and_reconfigure(self):
        self.assertIn("-DCMAKE_BUILD_TYPE=Release", BUILD)
        self.assertIn("-DTLG_LTO=ON", BUILD)
        self.assertNotIn("-DTLG_LTO=OFF", BUILD)
        self.assertNotIn("if [[ ! -f $out/gpu/build.ninja ]]", BUILD)
        self.assertIn('message(FATAL_ERROR "Android/glibc performance build requires working LTO:', CMAKE)
        self.assertIn("if (ISS_ANDROID_GLIBC)", CMAKE)

    def test_stats_opt_in_but_readbacks_unchanged(self):
        self.assertNotIn('"ISS_FRAME_STATS":', LAUNCH)
        self.assertIn('"ISS_READBACKS": "1"', LAUNCH)
        self.assertIn('"ISS_DEFERRED_READBACK": "1"', LAUNCH)
        self.assertNotIn('"ISS_GC_BUDGET_MB":', LAUNCH)
        self.assertIn('env.setdefault(key, value)', LAUNCH)
        self.assertIn('Flag("TLG_READBACK_LINEAR", false)', EMULATOR)
        self.assertNotIn("TLG_READBACK_LINEAR=1", SHELL)

    def test_clock_measurements_skip_disabled_diagnostics(self):
        wait = TOGGLES.split("struct WaitTimer {", 1)[1].split("\n};\n/// GPU thread rusage", 1)[0]
        self.assertIn("enabled ? std::chrono::steady_clock::now()", wait)
        self.assertIn("if (!enabled)", wait)
        self.assertIn("total.fetch_add", wait)

    def test_glibc_frame_queue_respects_other_builds(self):
        self.assertIn("#ifdef ISS_ANDROID_GLIBC", SETTINGS)
        section = SETTINGS.split("#ifdef ISS_ANDROID_GLIBC", 1)[1].split("#endif", 1)[0]
        self.assertIn("frames_queued{2}", section)
        self.assertIn("#else", section)
        self.assertIn("frames_queued{1}", section)

if __name__ == "__main__":
    unittest.main()
