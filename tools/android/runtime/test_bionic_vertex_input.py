"""Verify real recording methods with pinned Vulkan-Hpp and delayed driver execution."""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest
from test_bionic_resource_reuse import method

ROOT = Path(__file__).resolve().parents[3]
GPU = ROOT / "port/gpu/shadps4"
CXX = shutil.which("c++")


@unittest.skipUnless(CXX, "C++ compiler unavailable")
class VertexInputTests(unittest.TestCase):
    def test_production_recording_and_lifetimes(self):
        include = os.environ.get("VULKAN_HEADERS_INCLUDE")
        if not include:
            candidate = ROOT.parent / "vulkan_headers_check/include"
            if (candidate / "vulkan/vulkan.hpp").exists():
                include = str(candidate)
        if not include:
            self.skipTest("pinned Vulkan-Headers checked separately in native CI")
        scheduler = (GPU / "video_core/renderer_vulkan/vk_scheduler.cpp").read_text()
        header = (GPU / "video_core/renderer_vulkan/vk_scheduler.h").read_text()
        rasterizer = (GPU / "video_core/renderer_vulkan/vk_rasterizer.cpp").read_text()
        code = (ROOT / "tools/android/runtime/cpp/vertex_input_cache.cpp").read_text()
        code = code.replace("// PRODUCTION_INLINE_METHODS", "\n".join(method(header, s) for s in (
            "vk::CommandBuffer CommandBuffer()", "void InvalidateVertexInput()")))
        code = code.replace("// PRODUCTION_METHODS", "\n".join((
            method(scheduler, "void Scheduler::RecordVertexInput("),
            method(scheduler, "void Scheduler::AllocateWorkerCommandBuffers()"),
            method(rasterizer, "void Rasterizer::EmitVertexBuffers()"))))
        with tempfile.TemporaryDirectory() as directory:
            source, binary = Path(directory) / "test.cpp", Path(directory) / "test"
            source.write_text(code)
            for flags, modes in (
                (("-D__ANDROID__=1", "-DISS_ANDROID_BIONIC=1"),
                 ("deferred", "direct", "unthreaded", "switchdirect", "off")),
                ((), ("nonbionic",)),
            ):
                result = subprocess.run([CXX, "-std=c++20", "-O1", "-pthread",
                    "-fsanitize=undefined", "-fno-sanitize-recover=all", "-I", str(GPU),
                    "-I", include, *flags, str(source), "-o", str(binary)],
                    capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                for mode in modes:
                    result = subprocess.run([str(binary), mode], capture_output=True,
                                            text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, f"{mode}: {result.stderr}")

    def test_static_helper_invalidation_and_dynamic_pipeline_contract(self):
        # All game pipelines (including square passes) use the same instance-wide dynamic
        # vertex-input policy. The one deferred static graphics helper must invalidate it.
        graphics = (GPU / "video_core/renderer_vulkan/vk_graphics_pipeline.cpp").read_text()
        self.assertIn("dynamic_states.push_back(vk::DynamicState::eVertexInputEXT)", graphics)
        self.assertIn("auto square_info = pipeline_info", graphics)
        scene = (GPU / "video_core/renderer_vulkan/vk_scene_resolution.cpp").read_text()
        invalidated = scene.index("scheduler.InvalidateGuestGraphicsBindings();")
        header = (GPU / "video_core/renderer_vulkan/vk_scheduler.h").read_text()
        invalidate = method(header,"void InvalidateGuestGraphicsBindings()")
        self.assertIn("vertex_input_state.Invalidate();", invalidate)
        self.assertIn("descriptor_stream_epoch.fetch_add", invalidate)
        recorded = scene.index("scheduler.Record([=]", invalidated)
        self.assertLess(recorded, scene.index("cmd.bindPipeline", recorded))


if __name__ == "__main__":
    unittest.main()
