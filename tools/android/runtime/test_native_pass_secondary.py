"""Source and ABI gates for ARM64 Native Passes secondary Vulkan command buffers."""
from pathlib import Path
import unittest
ROOT=Path(__file__).resolve().parents[3]
class NativeSecondaryIntegration(unittest.TestCase):
 def test_formats_and_primary_secondary_recording_safety(self):
  h=(ROOT/"port/gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.h").read_text()
  c=(ROOT/"port/gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp").read_text()
  r=(ROOT/"port/gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp").read_text()
  p=(ROOT/"port/gpu/shadps4/video_core/renderer_vulkan/vk_resource_pool.cpp").read_text()
  a=(ROOT/"android/app/src/ro/cobrabm/fexdroid/BionicRendererSettings.java").read_text()
  for s in ['native_color_formats','native_depth_format','native_stencil_format',
            'native_eligible', 'native_secondary_scopes','RecordPrimary(', 'EndRendering();',
            'IsNativeSecondaryRendering()']:
   self.assertIn(s,h)
  for s in ['eContentsSecondaryCommandBuffers','vk::CommandBufferInheritanceRenderingInfo',
            'eRenderPassContinue','primary.executeCommands(scope.buffer)',
            'rendering_observer->AllowsSecondaryRendering()',
            'native_secondary_pools[native_lane]->Commit()']:
   self.assertIn(s,c)
  self.assertIn('state.native_eligible = eligible ? 1 : 0;',r)
  self.assertIn('if (arm64_native_secondary_requested &&',r)
  self.assertIn('vk::FormatFeatureFlagBits2::eColorAttachment',r)
  self.assertIn('.level = level,',p)
  self.assertIn('renderer_native_passes_arm64_secondary',a)
  self.assertIn('renderer_native_passes_arm64_color_passes',a)
  self.assertIn('!nativeSecondary && p.getBoolean("renderer_native_passes_arm64_scan"',a)
  self.assertIn('!nativeSecondary && p.getBoolean("renderer_native_passes_arm64_guest_observe"',a)
  self.assertIn('ISS_NATIVE_PASSES_ARM64_COLOR_PASSES',c)
  self.assertIn('WillUseNativeSecondary(new_state)',c)
  self.assertIn('WillUseNativeSecondary(const RenderState& next)',h)
  self.assertIn('next.num_color_attachments != 0 && !native_secondary_color_passes',h)
  self.assertEqual(r.count('scheduler.WillUseNativeSecondary(state)'),2)
  self.assertEqual(r.count('scheduler.IsNativeSecondaryRendering()'),2)
  self.assertIn('if (!native_graphics && scheduler.IsNativeSecondaryRendering())',r)
  self.assertIn('scheduler.EndRendering();',r)
  self.assertNotIn('const bool native_graphics = scheduler.NativeSecondaryRequested();',r)
  self.assertIn('ISS_NATIVE_PASSES_ARM64_SECONDARY',a)
  observer=(ROOT/"port/gpu/shadps4/video_core/renderer_vulkan/vk_occlusion.cpp").read_text()
  self.assertIn('scheduler.IsNativeSecondaryRendering()',observer)
  self.assertIn('scheduler.EndRendering();',observer)
if __name__=='__main__':unittest.main()
