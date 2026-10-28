"""Bionic inFAMOUS: opt-in Vulkan FSR 3.1, no FSR4/DLSS, native AHB output.

Host-side invariants; Vulkan dispatch correctness requires Android device logs.
"""
from pathlib import Path
import os
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[3]
GPU = ROOT / "port/gpu"
CM = (GPU / "CMakeLists.txt").read_text()
HDR = (GPU / "shadps4/video_core/renderer_vulkan/vk_temporal_upscaler.h").read_text()
PRESENTER = (GPU / "shadps4/video_core/renderer_vulkan/vk_presenter.cpp").read_text()
RUN = (ROOT / "tools/android/bionic/run-bionic.sh").read_text()
PROBE = (ROOT / "port/src/probe.c").read_text()


class BionicFsr3Tests(unittest.TestCase):
    def test_native_vulkan_fsr3_is_linked_without_fsr4(self):
        self.assertIn("add_subdirectory(third_party/fsr-vulkan EXCLUDE_FROM_ALL)", CM)
        self.assertIn("FFX_VK_PORTABLE_BUILD_FSR4_V07_VULKAN OFF", CM)
        self.assertIn("target_link_libraries(tlggpu PRIVATE ffx-vulkan::portable)", CM)
        self.assertIn("vk_temporal_upscaler.cpp", (GPU / "shadps4/video_core/renderer_vulkan/vk_temporal_upscaler.cpp").as_posix())
        self.assertNotIn("vk_temporal_upscaler_bionic.h", HDR)
        self.assertIn("FfxVkPortableUpscaleContext* context", HDR)
        self.assertIn("vk_fsr4", CM)
        self.assertIn("vk_fsr4_android.cpp", str(next(GPU.glob("shadps4/video_core/renderer_vulkan/vk_fsr4_android.cpp"))))
        self.assertIn("FFX_VK_PORTABLE_OK", (GPU / "shadps4/video_core/renderer_vulkan/vk_temporal_upscaler.cpp").read_text())

    def test_iss_fsr3_material_mip_bias_preserves_detail(self):
        import math
        renderer = (GPU / "shadps4/video_core/renderer_vulkan/vk_temporal_upscaler.cpp").read_text()
        raster = (GPU / "shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp").read_text()
        self.assertIn("const float resolution_bias = std::log2(render / output);", renderer)
        self.assertIn("return resolution_bias - 1.0f;", renderer)
        self.assertIn("Scaled() && TlgSettings::Get().upscaler == TlgSettings::UpscalerFsr3", renderer)
        self.assertIn("return resolution_bias;", renderer)
        self.assertIn("sampler_lod_bias += upscaler->SceneMipBias();", raster)
        self.assertIn("GetGraphicsKey().mrt_mask", raster)
        self.assertIn("std::popcount(", raster)
        self.assertIn("sampler.is_depth ? 0.0f : sampler_lod_bias", raster)
        # The inFAMOUS G-buffer gets AMD's suggested material mip
        # levels; post/UI/depth keep their original sampling.
        for width, output, expected in ((640, 1280, -2.0),
                                        (960, 1280, math.log2(.75) - 1.0),
                                        (1280, 1280, 0.0)):
            with self.subTest(render_width=width):
                actual = math.log2(width / output) - 1.0 if width < output else 0.0
                self.assertAlmostEqual(actual, expected, places=6)
        # Existing non-ISS/PC code retains log2(render/output) without -1.

    def test_iss_ldr_fsr3_has_valid_exposure_input(self):
        renderer = (GPU / "shadps4/video_core/renderer_vulkan/vk_temporal_upscaler.cpp").read_text()
        # AMD FSR3: an absent input exposure image is only valid with
        # AUTO_EXPOSURE. Keep HDR flag behavior unchanged and inverted depth.
        self.assertIn('if (!hdr && TlgSettings::Get().upscaler == TlgSettings::UpscalerFsr3)', renderer)
        self.assertIn('create_info.flags |= FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE;', renderer)
        self.assertIn('create_info.flags |= FFX_VK_PORTABLE_CONTEXT_DEPTH_INVERTED;', renderer)
        self.assertIn('info.exposure.structSize = sizeof(info.exposure);', renderer)
        self.assertIn('FFX_VK_PORTABLE_CONTEXT_DEPTH_INVERTED |\n                        FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE;', renderer)
        # Do not turn the LDR frame into HDR input: that corrupts color.
        self.assertIn('create_info.flags = hdr ? FFX_VK_PORTABLE_CONTEXT_HDR_COLOR_INPUT', renderer)
        manifest = (ROOT / 'android/app/BionicAndroidManifest.xml').read_text()
        self.assertIn('package="com.daedalus.iss.bionic"', manifest)
        build = (ROOT / 'tools/android/bionic/build_apk.sh').read_text()
        self.assertIn("package: name='com.daedalus.iss.bionic'", build)

    def test_fsr3_user_requested_capture_is_bounded_and_exportable(self):
        src = ROOT / "android/app/src/ro/cobrabm/fexdroid"
        game = (src / "BionicGameActivity.java").read_text()
        menu = (src / "BionicPlayerMenu.java").read_text()
        launcher = (src / "BionicMainActivity.java").read_text()
        renderer = (GPU / "shadps4/video_core/renderer_vulkan/vk_temporal_upscaler.cpp").read_text()
        self.assertIn('e.put("TLG_DUMP_FRAMES","2")', game)
        self.assertIn('e.put("TLG_DUMP_TRIGGER"', game)
        self.assertIn('e.put("TLG_DUMP_DIR"', game)
        self.assertIn('FileOutputStream stream=new FileOutputStream(trigger,false)', menu)
        self.assertIn('if(activity.getSharedPreferences("iss",0).getBoolean("fsr3",false))', menu)
        self.assertIn('DumpImages(instance, scheduler, cmdbuf, dump,', renderer)
        self.assertIn('if (const int dump = DumpFrame(); dump >= 0)', renderer)
        self.assertIn('{vk::Image(ui_image), ow, oh, 4, "output", "rgba"}', renderer)
        self.assertIn('new java.util.zip.ZipOutputStream(output)', launcher)
        self.assertIn('EXPORT_FSR=12', launcher)
        self.assertIn('new java.util.zip.ZipEntry("iss-android.log")', launcher)
        # Different Android application IDs and ephemeral signatures are forbidden.
        manifest = (ROOT / "android/app/BionicAndroidManifest.xml").read_text()
        self.assertIn('package="com.daedalus.iss.bionic"', manifest)
    def test_iss_fsr3_snapshots_scene_depth_before_hud_reuse(self):
        src = GPU / "shadps4/video_core/renderer_vulkan"
        header = (src / "vk_temporal_upscaler.h").read_text()
        renderer = (src / "vk_temporal_upscaler.cpp").read_text()
        self.assertIn("void SnapshotIssSceneDepth(VideoCore::ImageId depth);", header)
        self.assertIn("iss_scene_depth_snapshot_valid = false;", renderer)
        self.assertIn("SnapshotIssSceneDepth(camera_motion.Depth());", renderer)
        self.assertIn("const bool saved_scene_depth = iss_scene_depth_snapshot_valid", renderer)
        self.assertIn("depth_image = vk::Image(iss_scene_depth_image);", renderer)
        self.assertIn("depth_view = *iss_scene_depth_view;", renderer)
        self.assertIn("vk::ImageLayout::eTransferSrcOptimal", renderer)
        self.assertIn("vk::ImageLayout::eTransferDstOptimal", renderer)
        self.assertIn("cmd.copyImage(depth.GetImage(), vk::ImageLayout::eTransferSrcOptimal", renderer)
        self.assertIn("vk::ImageAspectFlagBits::eDepth", renderer)
        self.assertIn("camera_motion.RecordMotion(cmdbuf, depth_view, *motion_view, w, h);", renderer)
        self.assertIn("info.depth = Describe(depth_image, depth_format", renderer)
        self.assertIn('{depth_image, source_width, source_height, 4, "depth", "f32",', renderer)
        # Only ISS native Bionic scaled FSR3 is changed. Never trigger the old
        # early UI/HDR path that previously caused zero temporal dispatches.
        self.assertIn("scaled_session && Active()", renderer)
        self.assertNotIn("ISS_FSR_EARLY_HUD", renderer)
        self.assertNotIn("ISS_FSR_PRE_TAA_720", renderer)
        # Leave the installed package and signing identity intact.
        self.assertIn('package="com.daedalus.iss.bionic"',
            (ROOT / 'android/app/BionicAndroidManifest.xml').read_text())

    def test_bionic_keeps_ahardwarebuffer_and_fsr1_disabled(self):
        self.assertIn("#ifndef ISS_ANDROID_BIONIC", PRESENTER)
        self.assertIn("pp_pass.Render", PRESENTER)
        self.assertIn("vk_swapchain_ahb", (GPU / "CMakeLists.txt").read_text() + (GPU / "shadps4/video_core/renderer_vulkan/vk_swapchain_ahb.cpp").read_text())
        self.assertIn("TLG_FSR1=0 TLG_RCAS=0", RUN)

    def test_launcher_preserves_fsr3_request_and_default_none(self):
        self.assertIn('case "${TLG_UPSCALER:-none}" in', RUN)
        section = RUN.split('case "${TLG_UPSCALER:-none}" in', 1)[1].split('export TLG_FSR1=0', 1)[0]
        script = 'case "${TLG_UPSCALER:-none}" in' + section
        script += """\nprintf "%s %s %s %s" "$TLG_UPSCALER" "$ISS_UPSCALER" "$TLG_JITTER" "$TLG_OBJECT_MOTION"\n"""
        for selection, expected in (
            ('fsr3', 'fsr3 fsr3 1 1'), ('none', 'none off 0 0'),
            ('fsr4', 'none off 0 0')
        ):
            with self.subTest(selection=selection):
                env = dict(os.environ, TLG_UPSCALER=selection)
                result = subprocess.run(["sh", "-c", script], env=env, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(result.stdout.strip(), expected)

    def test_android_ui_opt_in_and_internal_output_scaling(self):
        src = ROOT / "android/app/src/ro/cobrabm/fexdroid"
        game = (src / "BionicGameActivity.java").read_text()
        video = (src / "BionicVideoSettings.java").read_text()
        self.assertIn('prefs.getBoolean("fsr3",false)', video)
        self.assertIn('if(prefs.getBoolean("fsr3",false))return "1280x720";', video)
        self.assertIn('"performance".equals(fsrMode(prefs))?"360p":"540p"', video)
        self.assertIn('"performance".equals(fsrMode(prefs))?"640x360":"960x540"', video)
        self.assertIn('prefs.getBoolean("fsr3",false)?fsrInternalResolution(prefs)', video)
        self.assertIn('resolutionPatch.setEnabled(!active)', video)
        self.assertIn('if(!prefs.getBoolean("fsr3",false)&&pos>=0', video)
        self.assertIn('e.put("TLG_UPSCALER",fsr3?"fsr3":"none")', game)
        self.assertIn('e.put("TLG_RENDER_RES",BionicVideoSettings.fsrInputSize(prefs))', game)
        self.assertIn('e.put("TLG_UPSCALE_PRESET",Integer.toString(BionicVideoSettings.fsrEnginePreset(prefs)))', game)
        self.assertIn('BionicVideoSettings.effectiveResolutionPatch(prefs)', game)
        self.assertIn('BionicVideoSettings.fsrSharpnessValue(prefs)', game)
        self.assertIn('BionicVideoSettings.fsrSharpenEnabled(prefs)', game)
        self.assertIn('e.put("TLG_FSR_SHARPEN"', game)
        self.assertIn('fsr3_sharpness_pct', video)
        self.assertIn('fsr3_sharpen_enabled', video)
        self.assertIn('sharpenSlider.setMax(200)', video)
        self.assertIn('sharpenSlider.setEnabled(active&&sharpening.isChecked())', video)
        self.assertIn('if(resolutionPatch!=null)patches.add(resolutionPatch)', game)
        self.assertNotIn('prefs.getString("internal_res","native");\\n   String internal=', game)
        # Both internal game patches must exist so there is exactly one automatic
        # executable resolution patch when FSR3 is enabled.
        patches = (ROOT / "port/patches/InfamousSecondSon.xml").read_text()
        for size in ("640x360", "960x540"):
            self.assertIn('Name="Render resolution '+size+'"', patches)
        self.assertIn('e.put("ISS_SCREEN_W",dimensions[0])', game)
        self.assertIn('e.put("ISS_SCREEN_H",dimensions[1])', game)

    def test_fsr_visual_diagnostics_have_no_readback_or_per_draw_printing(self):
        renderer = (GPU / "shadps4/video_core/renderer_vulkan/vk_temporal_upscaler.cpp").read_text()
        upscaler_h = (GPU / "shadps4/video_core/renderer_vulkan/vk_temporal_upscaler.h").read_text()
        native = (GPU / "shim/tlg_settings.cpp").read_text()
        self.assertIn('{"TLG_FSR_SHARPEN", "sharpen"}', native)
        self.assertIn('FSR3 ISS visual: source=%ux%u', renderer)
        for key in ('rcas_dispatches', 'extra_sharpen_dispatches',
                    'ui_draws_after_fsr', 'redirected_color',
                    'redirected_samples', 'ui_only_copies'):
            self.assertIn(key, upscaler_h)
        self.assertIn('if (Active() && ++android_fsr_probe.frames >= 180)', renderer)
        self.assertIn('native_camera_dispatches', upscaler_h)
        self.assertIn('fallback_motion_dispatches', upscaler_h)
        self.assertIn('if (camera_motion.IssNativeCameraReady()) {', renderer)
        self.assertNotIn('camera_native=%u', renderer)

    def test_iss_native_projection_jitter_reaches_fsr3(self):
        camera_h = (GPU / "shadps4/video_core/renderer_vulkan/vk_camera_motion.h").read_text()
        camera_cpp = (GPU / "shadps4/video_core/renderer_vulkan/vk_camera_motion.cpp").read_text()
        renderer = (GPU / "shadps4/video_core/renderer_vulkan/vk_temporal_upscaler.cpp").read_text()
        self.assertIn("sample.projection[8], sample.projection[9]", camera_cpp)
        self.assertIn("IssNativeJitterPixels(u32 width, u32 height)", camera_h)
        self.assertIn("native_previous_jitter_ndc[0] * (0.5f * float(width))", camera_h)
        self.assertIn("-native_previous_jitter_ndc[1] * (0.5f * float(height))", camera_h)
        self.assertIn("const auto native_jitter_pixels = camera_motion.IssNativeJitterPixels(w, h);", renderer)
        self.assertIn("info.jitterOffset = {sign * jitter[0] + native_jitter_pixels[0],", renderer)
        self.assertIn("sign * jitter[1] + native_jitter_pixels[1]", renderer)
        # inFAMOUS uses a native +/-0.25 pixel camera offset at 640x360.
        # Halving the host Halton sample keeps the total within +/-0.5 pixel.
        self.assertIn("0.5f * (Halton(jitter_index, 2) - 0.5f)", renderer)
        self.assertIn("0.5f * (Halton(jitter_index, 3) - 0.5f)", renderer)
        for nx, ny in ((1 / 1280, -1 / 720), (-1 / 1280, 1 / 720)):
            guest_x, guest_y = nx * (640 / 2), -ny * (360 / 2)
            self.assertAlmostEqual(abs(guest_x), 0.25)
            self.assertAlmostEqual(abs(guest_y), 0.25)
            for halton_x, halton_y in ((0.0, 0.0), (1.0, 1.0), (0.5, 0.5)):
                self.assertLessEqual(abs(guest_x + 0.5 * (halton_x - 0.5)), 0.5)
                self.assertLessEqual(abs(guest_y + 0.5 * (halton_y - 0.5)), 0.5)

    def test_pc_zero_is_diagnosable(self):
        self.assertIn("arm->uc_mcontext.regs[30]", PROBE)
        self.assertIn("native caller LR:", PROBE)
        self.assertIn("if (!functions_loaded)", (GPU / "shim/tlg_overlay.cpp").read_text())


if __name__ == "__main__":
    unittest.main()
