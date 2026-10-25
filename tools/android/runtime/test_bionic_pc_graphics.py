"""Execute the production stride eligibility policy and occlusion query lifetime.

These are host checks, not evidence of Adreno gameplay performance.
"""
from pathlib import Path
import unittest
from test_bionic_resource_reuse import ResourceReuseTests, method

ROOT=Path(__file__).resolve().parents[3]

class PcGraphicsTests(unittest.TestCase):
 def test_dynamic_stride_only_for_valid_compute_descriptors(self):
  source=(ROOT/'port/gpu/shadps4/shader_recompiler/info.h').read_text()
  body=method(source,'inline bool UsesDynamicStride(')
  ResourceReuseTests().run_cpp(r'''
#include <cassert>
#include <cstdint>
#include <initializer_list>
using u32=uint32_t;
constexpr u32 UNKNOWN_LOCATION=~0u;
enum class HwStage{Compute,Vertex,Fragment};
struct Info{HwStage hw_stage;};
struct BufferResource{
 struct{u32 load_mask,offsets[4];}sharp_fetch;
 bool special;
 bool IsSpecial()const{return special;}
};
namespace Toggles{bool enabled=true;bool DynamicBufferStride(){return enabled;}}
''' + body + r'''
int main(){
 Info info{HwStage::Compute};BufferResource d{{2,{0,12,0,0}},false};
 assert(UsesDynamicStride(info,d));
 for(auto stage:{HwStage::Vertex,HwStage::Fragment}){info.hw_stage=stage;assert(!UsesDynamicStride(info,d));}
 info.hw_stage=HwStage::Compute;d.special=true;assert(!UsesDynamicStride(info,d));d.special=false;
 d.sharp_fetch.load_mask=1;assert(!UsesDynamicStride(info,d));d.sharp_fetch.load_mask=2;
 for(u32 invalid:{0u,UNKNOWN_LOCATION}){d.sharp_fetch.offsets[1]=invalid;assert(!UsesDynamicStride(info,d));}
 d.sharp_fetch.offsets[1]=12;Toggles::enabled=false;assert(!UsesDynamicStride(info,d));
}
''')

 def test_query_reuse_never_waits_for_inflight_slot(self):
  source=(ROOT/'port/gpu/shadps4/video_core/renderer_vulkan/vk_occlusion.cpp').read_text()
  body=method(source,'void OcclusionQueries::BeginSegment(')
  # Do not replace a GPU wait with an unconditional reset: a busy slot must be dropped.
  busy=body.index('if (last_use != 0 && !scheduler.IsFree(last_use))')
  reset=body.index('resetQueryPool(')
  self.assertLess(busy,reset)
  self.assertIn('not_counted();\n        return;',body[busy:reset])
  self.assertNotIn('scheduler.Wait(',body)
  scheduler=(ROOT/'port/gpu/shadps4/video_core/renderer_vulkan/vk_scheduler.cpp').read_text()
  end=method(scheduler,'void Scheduler::EndRendering(')
  # The secondary path closes its child before ending primary rendering;
  # the observer must still run first for both the original and secondary paths.
  self.assertLess(end.index('OnEndRendering()'),end.index('primary.endRendering()'))
  self.assertLess(end.index('primary.executeCommands(scope.buffer)'),
                  end.index('primary.endRendering()'))

if __name__=='__main__':unittest.main()
