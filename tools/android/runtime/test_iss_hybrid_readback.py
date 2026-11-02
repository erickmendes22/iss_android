"""Source-level guard for the stable ISS Android Relaxed/Hybrid readback picker."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]


class HybridReadbackTest(unittest.TestCase):
    def test_hybrid_only_enables_precise_read_protection_on_relaxed(self):
        src = (ROOT / "port/gpu/shadps4/video_core/buffer_cache/region_manager.h").read_text()
        self.assertIn('TLG_ISS_HYBRID_READ_PROTECT', src)
        self.assertIn('enabled && EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Relaxed', src)
        self.assertIn('GpuReadbacksMode::Precise ||\n                HybridReadProtectionEnabled()', src)
        self.assertIn('UpdateProtection<enable, true>();', src)

    def test_readback_modes_are_accessible_in_one_dialog(self):
        src = (ROOT / "android/app/src/ro/cobrabm/fexdroid/BionicReadbackSettings.java").read_text()
        self.assertIn("RadioGroup choices = new RadioGroup(activity);", src)
        self.assertIn("choices.check(ids[mode(preferences)]);", src)
        self.assertIn("putBoolean(KEY,false)", src)
        self.assertIn("int choice = selected == ids[1] ? 1 : 0;", src)
        self.assertIn('return "Precise 2";',src)
        self.assertNotIn("ids[2]", src)
        self.assertIn("putBoolean(HYBRID_KEY,choice==1)", src)
        main = (ROOT / "android/app/src/ro/cobrabm/fexdroid/BionicMainActivity.java").read_text()
        player = (ROOT / "android/app/src/ro/cobrabm/fexdroid/BionicPlayerMenu.java").read_text()
        self.assertIn("BionicReadbackSettings.show(", main)
        self.assertIn("BionicReadbackSettings.show(", player)

    def test_hybrid_v6_optimized_eager_backoff_is_optional(self):
        settings = (ROOT / "android/app/src/ro/cobrabm/fexdroid/BionicRendererSettings.java").read_text()
        dialog = (ROOT / "android/app/src/ro/cobrabm/fexdroid/BionicReadbackSettings.java").read_text()
        core = (ROOT / "port/gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp").read_text()
        self.assertIn('env.put("TLG_ISS_EAGER_BACKOFF_63",hybrid && p.getBoolean("renderer_hybrid_eager_backoff",false) ? "1" : "0");', settings)
        self.assertIn('static final String BACKOFF_KEY = "renderer_hybrid_eager_backoff";', dialog)
        self.assertIn('.putBoolean(BACKOFF_KEY,optimized.isChecked())',dialog)
        self.assertIn('.putBoolean(TARGETED_KEY,targeted.isChecked()).apply();',dialog)
        self.assertIn('constexpr u8 EagerHybridMaxBackoff = 63;', core)
        self.assertIn('RegionManager::HybridReadProtectionEnabled() && opt && opt[0] == \'1\'', core)
        self.assertNotIn('FinishDeferredReadback(*rb, false);', core)

    def test_hybrid_v2_staging_and_eager_are_opt_in(self):
        settings = (ROOT / "android/app/src/ro/cobrabm/fexdroid/BionicRendererSettings.java").read_text()
        cache = (ROOT / "port/gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp").read_text()
        self.assertIn('env.put("TLG_MAPPED_READBACK","0");',settings)
        self.assertIn('env.put("TLG_EAGER_READBACK","1");',settings)
        self.assertIn('env.put("TLG_MAPPED_READBACK","1");',settings)
        self.assertIn('RegionManager::HybridReadProtectionEnabled());',cache)
        self.assertIn('ISS hybrid V6 (V2 baseline):',cache)

    def test_staged_gpu_writes_must_invalidate_old_copies(self):
        cache = (ROOT / "port/gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp").read_text()
        header = (ROOT / "port/gpu/shadps4/video_core/renderer_vulkan/vk_runtime.h").read_text()
        runtime = (ROOT / "port/gpu/shadps4/video_core/renderer_vulkan/vk_runtime.cpp").read_text()
        self.assertIn('if (mapped_readbacks_enabled || hybrid_stage_tracking)',cache)
        self.assertIn('if (RegionManager::HybridReadProtectionEnabled() && tick > readback->tick)',cache)
        self.assertIn('readback->stale.store(true, std::memory_order_release);',cache)
        self.assertIn('this, mapped_readbacks_enabled);',cache)
        self.assertIn('bool buffer_write_needs_host_barrier = true;',header)
        self.assertIn('if (buffer_write_needs_host_barrier)',runtime)

    def test_targeted_staging_is_opt_in_and_does_not_join_uncopied_pages(self):
        src = (ROOT / "port/gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp").read_text()
        settings = (ROOT / "android/app/src/ro/cobrabm/fexdroid/BionicRendererSettings.java").read_text()
        ui = (ROOT / "android/app/src/ro/cobrabm/fexdroid/BionicReadbackSettings.java").read_text()
        self.assertIn('TLG_ISS_HYBRID_TARGETED_STAGE',src)
        self.assertIn('hybrid_staging_hot_pages.ForEachSelected(',src)
        self.assertIn('readback->CopiesTouchPages(device_addr, device_addr + size)',src)
        self.assertIn('renderer_hybrid_targeted_staging',settings)
        self.assertIn('.putBoolean(TARGETED_KEY,targeted.isChecked()).apply();',ui)

    def test_precise_label_uses_hybrid_backend_without_readbacks_2(self):
        src = (ROOT / "android/app/src/ro/cobrabm/fexdroid/BionicRendererSettings.java").read_text()
        self.assertIn('renderer_experimental_hybrid_readback', src)
        self.assertIn('final boolean hybrid = p.getBoolean("renderer_experimental_hybrid_readback",false)', src)
        self.assertIn('|| p.getBoolean("fix_smoke_powers_precise_readback",false);', src)
        self.assertIn('env.put("TLG_ISS_HYBRID_READ_PROTECT",hybrid ? "1" : "0");', src)
        self.assertIn('env.put("TLG_READBACKS", "1");', src)
        self.assertNotIn('env.put("TLG_READBACKS", precise ? "2" : "1")',src)
        self.assertNotIn('CheckBox hybrid=new CheckBox(a);',src)


if __name__ == "__main__":
    unittest.main()
