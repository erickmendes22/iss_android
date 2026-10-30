"""Execute the production fragment-motion emitter against semantic SPIR-V operations."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[3]
SOURCE=ROOT/'port/gpu/shadps4/shader_recompiler/backend/spirv/emit_spirv_special.cpp'
class FragmentMotionDepth(unittest.TestCase):
 def test_actual_emitter_uses_exported_depth_and_preserves_validity(self):
  source=SOURCE.read_text()
  method=source[source.index('static void EmitFragmentMotion('):source.index('void EmitEpilogue(')]
  harness=r'''
#include <array>
#include <vector>
#include <cassert>
#include <cmath>
using Id=int;using u32=unsigned;
namespace spv {enum class StorageClass{PushConstant};}
namespace Sirit {bool ValidId(Id id){return id!=0;}}
struct PushData{static constexpr u32 XScaleIndex=0,YScaleIndex=1;};
struct Value{std::array<float,4> x{};};
struct EmitContext{
 std::array<Id,5> F32{0,1,2,3,4};std::array<Id,2> U1{0,1};
 std::vector<Value> values{1};
 Id motion_in_cur=0,motion_in_prev=0,frag_coord=0,frag_depth=0;
 Id push_data_block=10000,motion_frag_out=10001;
 Value output{};unsigned exported_loads=0,raster_loads=0;
 Id make(std::array<float,4> x){values.push_back({x});return int(values.size()-1);}
 Id scalar(float x){return make({x,0,0,0});}
 Id Constant(Id,float x){return scalar(x);}Id ConstU32(u32 x){return scalar(float(x));}
 Id TypePointer(spv::StorageClass,Id){return 0;}
 Id OpAccessChain(Id,Id block,Id index){assert(block==push_data_block);return scalar(values[index].x[0]==0?320.f:180.f);}
 Id OpLoad(Id,Id pointer){assert(pointer>0);exported_loads+=pointer==frag_depth;raster_loads+=pointer==frag_coord;return pointer;}
 Id OpCompositeExtract(Id,Id v,u32 component){return scalar(values[v].x[component]);}
 Id OpFDiv(Id,Id a,Id b){return scalar(values[a].x[0]/values[b].x[0]);}
 Id OpFMul(Id,Id a,Id b){return scalar(values[a].x[0]*values[b].x[0]);}
 Id OpFSub(Id,Id a,Id b){return scalar(values[a].x[0]-values[b].x[0]);}
 Id OpFNegate(Id,Id a){return scalar(-values[a].x[0]);}
 Id OpFOrdGreaterThan(Id,Id a,Id b){return scalar(values[a].x[0]>values[b].x[0]);}
 Id OpLogicalAnd(Id,Id a,Id b){return scalar(values[a].x[0]!=0&&values[b].x[0]!=0);}
 Id OpSelect(Id,Id c,Id a,Id b){return values[c].x[0]!=0?a:b;}
 Id OpCompositeConstruct(Id,const std::array<Id,4>& ids){return make({values[ids[0]].x[0],values[ids[1]].x[0],values[ids[2]].x[0],values[ids[3]].x[0]});}
 void OpStore(Id pointer,Id v){assert(pointer==motion_frag_out);output=values[v];}
};
METHOD
int main(){
 constexpr float raster=.04403737932443619f,exported=.04880699887871742f;
 for(bool writes:{false,true})for(float validity:{0.f,.5f,.989f,.995f,1.f})for(bool front:{false,true}){
  EmitContext c;
  c.motion_in_cur=c.make({.2f,.3f,.1f,1.f});
  c.motion_in_prev=c.make({.19f,.31f,validity,front?1.f:-1.f});
  c.frag_coord=c.make({0,0,raster,1});if(writes)c.frag_depth=c.scalar(exported);
  EmitFragmentMotion(c);
  const auto out=c.output.x;
  assert(out[3]==(writes?exported:raster));
  assert(c.exported_loads==unsigned(writes)&&c.raster_loads==unsigned(!writes));
  const float v=front?validity:0.f;
#ifdef ISS_ANDROID_BIONIC
  assert(out[2]==(writes?-v:v));
#else
  assert(out[2]==v);
#endif
  assert((std::abs(out[2])>.99f)==(v>.99f));
  assert(std::abs(out[0]-(front?-3.2f:0.f))<.00002f);
  assert(std::abs(out[1]-(front?1.8f:0.f))<.00002f);
  // Capture22 depth gate: the old raster value fails, the actual export matches.
  const auto matches=[](float a){return std::abs(a-exported)<exported*.005f;};
  assert(matches(out[3])==writes);
 }
}
'''.replace('METHOD',method)
  with tempfile.TemporaryDirectory() as directory:
   p=Path(directory);(p/'test.cpp').write_text(harness)
   for mobile in (False,True):
    executable=p/('android' if mobile else 'host')
    subprocess.run(['c++','-std=c++20','-Wall','-Wextra','-Werror']+(['-DISS_ANDROID_BIONIC'] if mobile else [])+[str(p/'test.cpp'),'-o',str(executable)],check=True)
    subprocess.run([str(executable)],check=True)
 def test_all_compose_paths_keep_the_same_validity_magnitude_threshold(self):
  code=(ROOT/'port/gpu/shadps4/video_core/host_shaders/camera_motion.comp').read_text()
  self.assertEqual(code.count('abs(object_motion.b) > 0.99'),3)
  self.assertIn('max(2e-7, depth * 0.005)',code)
  self.assertIn('max(vec2(4.0), size * 0.0125)',code)
  cmake=(ROOT/'port/gpu/CMakeLists.txt').read_text()
  self.assertIn('shadps4/shader_recompiler/*.cpp',cmake)
  self.assertIn('file(SHA1 ${source} source_hash)',cmake)
if __name__=='__main__':unittest.main()
