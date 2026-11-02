"""Verify that the Bionic no-worker stage memo maintains the normal coherency path."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[3]
GPU = ROOT / "port/gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp"
GPU_H = ROOT / "port/gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.h"
JAVA = ROOT / "android/app/src/ro/cobrabm/fexdroid/BionicRendererSettings.java"
DRAW = ROOT / "port/gpu/shadps4/video_core/renderer_vulkan/vk_draw_prep.cpp"

class StageBindMemoTests(unittest.TestCase):
    def test_unprepared_bionic_stage_texture_and_sampler_reuse(self):
        src = GPU.read_text()
        start = src.index("bool Rasterizer::BindTexturesFromSet(")
        end = src.index("RenderState Rasterizer::BeginRendering(", start)
        body = src[start:end]
        self.assertIn("ISS_STAGE_BIND_MEMO", body)
        self.assertIn("enable_unprepared", body)
        self.assertNotIn("if (!prepared || count", body)
        self.assertIn("live_hashes[i] = ImageDescHash(sharp, stage.images[i]);", body)
        self.assertIn("const u64* hashes = prepared ? prepared->image_hashes : live_hashes.data();", body)
        self.assertIn("texture_cache.IsUpToDate(entry.id)", body)
        self.assertIn("image.binding.needs_rebind", body)
        self.assertIn("scene_targets->ProxyCurrent", body)
        self.assertIn("runtime.Transit(&image", body)
        self.assertIn("texture_cache.MarkFound(entry.id)", body)
        self.assertIn("texture_cache.RegistryGeneration()", body)
        self.assertIn("stage_bind_memo_hits.fetch_add(", body)
        self.assertIn("stage_bind_memo_misses.fetch_add(", body)
        sampler = src[src.index("u64 Rasterizer::SamplerSetKey("):start]
        self.assertIn("stage.samplers[i].GetSharp(stage)", sampler)
        self.assertIn("texture_cache.TouchSamplers(", sampler)
        self.assertNotIn("prepared != nullptr &&", sampler)

    def test_feature_switch_and_worker_policy(self):
        java = JAVA.read_text()
        self.assertIn('ISS_STAGE_BIND_MEMO', java)
        self.assertIn('renderer_stage_bind_memo', java)
        self.assertIn('return 0;', DRAW.read_text())
        self.assertIn('std::array<TextureSet, 32768>', GPU_H.read_text())

if __name__ == "__main__":
    unittest.main()
