"""Exercise production eligibility rules: only a verified, matching LDR scene may bypass FXAA."""
import pathlib,tempfile,subprocess,unittest
ROOT=pathlib.Path(__file__).resolve().parents[3]
class InputPolicy(unittest.TestCase):
 def test_guarded_source_and_fallback(self):
  code=r'''
#include <cassert>
#include "iss_fsr_input_policy.h"
int main(){
 using namespace IssFsrInput;const Image good{true,true,37,640,360};
 // Even unchanged original input cannot reuse history without camera vectors.
 assert(ResetHistory(false,false));
 assert(ResetHistory(true,false));
 assert(ResetHistory(true,true));
 assert(!ResetHistory(false,true));
 assert(UseOriginal(true,true,true,good,good,37));
 assert(!UseOriginal(false,true,true,good,good,37));
 assert(!UseOriginal(true,false,true,good,good,37));
 assert(!UseOriginal(true,true,false,good,good,37));
 Image bad=good;bad.available=false;assert(!UseOriginal(true,true,true,bad,good,37));
 bad=good;bad.sampled=false;assert(!UseOriginal(true,true,true,bad,good,37));
 bad=good;bad.format=97;assert(!UseOriginal(true,true,true,bad,good,37));
 assert(!UseOriginal(true,true,true,good,bad,37));
 bad=good;bad.width=1280;assert(!UseOriginal(true,true,true,bad,good,37));
 bad=good;bad.height=720;assert(!UseOriginal(true,true,true,bad,good,37));
 bad=good;bad.height=0;assert(!UseOriginal(true,true,true,bad,bad,37));
}
'''
  with tempfile.TemporaryDirectory() as d:
   p=pathlib.Path(d);(p/'test.cpp').write_text(code)
   subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror','-I'+str(ROOT/'port/gpu/shim'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
 def test_sdk_source_only_changes_target_and_motion_are_preserved(self):
  code=(ROOT/'port/gpu/shadps4/video_core/renderer_vulkan/vk_temporal_upscaler.cpp').read_text()
  block=code[code.index('void TemporalUpscaler::RunScaled()'):code.index('vk::ImageView TemporalUpscaler::Mirror')]
  self.assertIn('color_image = original.GetImage();',block)
  self.assertIn('color_usage = original.usage_flags;',block)
  self.assertIn('original_aa_input != iss_fsr_used_original_aa',block)
  self.assertIn('reset = true;',block)
  self.assertIn('IssFsrInput::ResetHistory(reset, camera_motion.IssNativeCameraReady())',block)
  self.assertNotIn('ldr_target = iss_aa_prefilter',code)
  self.assertIn('ui_color = ldr_target;',block)
  self.assertIn('camera_motion.RecordMotion(cmdbuf, depth_view, *motion_view, w, h);',block)
  self.assertIn('"aa_after_fxaa"',block)
if __name__=='__main__':unittest.main()
