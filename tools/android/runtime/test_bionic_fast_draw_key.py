"""Guard Bionic Fast Draw pipeline-key reuse against stale guest shader data."""
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[3]
PC=ROOT/"port/gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.cpp"
PH=ROOT/"port/gpu/shadps4/video_core/renderer_vulkan/vk_pipeline_cache.h"
UI=ROOT/"android/app/src/ro/cobrabm/fexdroid/BionicRendererSettings.java"

class FastDrawKeyTests(unittest.TestCase):
    def test_shader_program_and_register_guards(self):
        s=PC.read_text()
        fast=s[s.index("const GraphicsPipeline* PipelineCache::TryFastGraphicsPipeline("):
               s.index("const GraphicsPipeline* PipelineCache::GetGraphicsPipeline(")]
        for item in ("params.is_indirect", "FrameCapture::Active()",
                     "fast_graphics.tick != scheduler.CurrentTick()",
                     "fast_graphics.frame != TlgStats::gpu_frames.load(",
                     "fast_graphics.checksum != liverpool->gfx_reg_checksum",
                     "regs.ProgramForStage(", "AmdGpu::TryGetParams(*pgm)",
                     "found->hash != remembered.program_hash",
                     "info.RefreshFlatBuf()", "std::memcmp(",
                     "fast_graphics.valid = false;"):
            self.assertIn(item, fast)
        self.assertIn("ISS_FAST_DRAW_KEY", s)
        self.assertIn("fast_graphics.valid = false;\n                graphics_pipelines.erase(graphics_key);", s)
        body=s[s.index("const GraphicsPipeline* PipelineCache::GetGraphicsPipeline("):
               s.index("const ComputePipeline* PipelineCache::GetComputePipeline(")]
        self.assertLess(body.index("TryFastGraphicsPipeline(params, prepared)"),
                        body.index("RefreshGraphicsKey(sel)"))
        self.assertEqual(body.count("RememberFastGraphicsPipeline("),4)

    def test_android_opt_in_and_unmodified_stage_binding(self):
        ui=UI.read_text()
        self.assertIn("renderer_fast_draw_key", ui)
        self.assertIn('p.getBoolean("renderer_fast_draw_key",false)',ui)
        h=PH.read_text()
        self.assertIn("struct FastGraphicsState",h)
        self.assertIn("TryFastGraphicsPipeline",h)
        self.assertIn("RememberFastGraphicsPipeline",h)

if __name__ == "__main__":
    unittest.main()
