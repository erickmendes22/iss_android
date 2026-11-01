"""Guard command buffer state ownership in the Native Passes ARM64 experiment."""
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[3]

class NativeSecondaryGraphicsStateTests(unittest.TestCase):
    def test_draw_state_is_recorded_after_begin_in_native_mode(self):
        src=(ROOT/"port/gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp").read_text()
        start=src.index("void Rasterizer::DrawRecord(")
        end=src.index("void Rasterizer::DrawIndirect(",start)
        direct=src[start:end]
        secondary=direct.index("scheduler.BeginRendering(state);\n    if (native_graphics) {")
        state=direct.index("pipeline->BindResources(set_writes, set_write_index, push_data);",secondary)
        dynamic=direct.index("UpdateDynamicState(pipeline, is_indexed);",secondary)
        vertex=direct.index("EmitVertexBuffers();",secondary)
        self.assertLess(secondary,state)
        self.assertLess(secondary,dynamic)
        self.assertLess(secondary,vertex)
        self.assertIn("if (!native_graphics)",direct)
        start=src.index("void Rasterizer::DrawIndirectRecord(")
        end=src.index("void Rasterizer::SubmitIfWorkPiledUp(",start)
        indirect=src[start:end]
        secondary=indirect.index("scheduler.BeginRendering(state);\n    if (native_graphics) {")
        for cmd in ("EmitVertexBuffers();", "EmitIndexBuffer();",
                    "pipeline->BindResources(set_writes, set_write_index, push_data);",
                    "UpdateDynamicState(pipeline, is_indexed);"):
            self.assertGreater(indirect.index(cmd,secondary),secondary)
    def test_secondary_state_caches_reset_per_pass(self):
        sch=(ROOT/"port/gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp").read_text()
        hd=(ROOT/"port/gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.h").read_text()
        self.assertIn("NativeSecondaryRequested() const noexcept",hd)
        self.assertIn("dynamic_state.Invalidate();",sch)
        self.assertIn("vertex_input_state.Invalidate();",sch)
        self.assertIn("if (native_secondary_enabled) {",sch)

if __name__=="__main__":unittest.main()
