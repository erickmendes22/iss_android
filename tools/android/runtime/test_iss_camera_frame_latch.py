"""Execute production camera preparation and frame cleanup without a GPU."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[3]

class CameraFrameLatch(unittest.TestCase):
    def test_actual_camera_across_repeated_and_midframe_updates(self):
        source=(ROOT/'port/gpu/shadps4/video_core/renderer_vulkan/vk_camera_motion.cpp').read_text()
        header=(ROOT/'port/gpu/shadps4/video_core/renderer_vulkan/vk_camera_motion.h').read_text()
        method=source[source.index('void CameraMotion::PrepareIssNativeCamera()'):
                      source.index('\n#endif\n\nvoid CameraMotion::RecordMotion')]
        cleanup=source[source.index('    if (!frame_has_camera) InvalidateHistory();'):
                       source.index('\n}\n\nvoid CameraMotion::Overlay')]
        fields=header[header.index('    std::array<float, 16> native_previous_composite'):
                      header.index('    struct BindingSample')]
        invalidate=header[header.index('    void InvalidateHistory()'):
                          header.index('    void SetObjectMotion')]
        setter=header[header.index('    void SetIssSubmissionCamera('):
                      header.index('    // Latch the submission')]
        harness=r'''
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "gpu/tlggpu.h"
using u32=uint32_t;using u64=uint64_t;
static TlgIssCameraSnapshot latest{};
extern "C" uint64_t tlggpu_iss_camera_read(TlgIssCameraSnapshot* out){*out=latest;return out->sequence;}
class CameraMotion {
public:
 bool for_upscaler=true,frame_has_camera=false;
 int depth_id=1;
 struct {bool valid=false;} current,previous;
 FIELDS
 INVALIDATE
 SETTER
 void PrepareIssNativeCamera();
 void PrepareSubmitted(){PrepareIssNativeCamera();}
 void EndFrame(){
CLEANUP
 }
 void BeginFrame(){depth_id=1;}
};
METHOD
static void publish(u64 seq,float x,u64 object=0x100){
 latest={};latest.sequence=seq;latest.guest_object=object;
 for(int i:{0,5,10,15})latest.composite[i]=latest.inverse[i]=latest.projection[i]=1;
 latest.composite[12]=x;latest.inverse[12]=-x;
 latest.near_plane=10;latest.far_plane=10000000;
 latest.vertical_fov=1;latest.aspect=16.f/9;
}
static bool ready(const CameraMotion& c){return c.native_motion_valid&&c.current.valid&&c.previous.valid;}
int main(int argc,char**argv){
 assert(argc==2);
 if(!std::strcmp(argv[1],"repeat")){
  CameraMotion c;publish(100,0);c.PrepareIssNativeCamera();assert(!ready(c));
  c.EndFrame();c.BeginFrame();c.PrepareIssNativeCamera();
  assert(ready(c)&&c.frame_has_camera);
  for(int i=0;i<16;++i)assert(c.native_reprojection[i]==float(i%5==0));
  assert(c.native_jitter_delta_ndc[0]==0&&c.native_jitter_delta_ndc[1]==0);
  c.EndFrame();assert(c.native_previous_valid);
  c.BeginFrame();publish(101,2);c.PrepareIssNativeCamera();
  assert(ready(c)&&std::abs(c.native_reprojection[3]+2)<1e-6);
  c.EndFrame();c.BeginFrame();c.PrepareIssNativeCamera();
  assert(ready(c)&&c.native_reprojection[3]==0);
 }else if(!std::strcmp(argv[1],"double")){
  CameraMotion c;publish(199,0);c.PrepareIssNativeCamera();c.EndFrame();
  c.BeginFrame();publish(200,2);c.PrepareIssNativeCamera();
  assert(ready(c));const auto transform=c.native_reprojection;
  publish(201,5);latest.near_plane=20;c.PrepareIssNativeCamera();
  assert(c.native_previous_sequence==200&&c.native_reprojection==transform&&c.native_near==10);
  c.EndFrame();c.BeginFrame();c.PrepareIssNativeCamera();
  assert(c.native_previous_sequence==201&&c.native_near==20);
  assert(ready(c)&&std::abs(c.native_reprojection[3]+3)<1e-6);
 }else if(!std::strcmp(argv[1],"validation")){
  CameraMotion c;publish(1,0);c.PrepareIssNativeCamera();c.EndFrame();
  c.BeginFrame();publish(2,1);latest.inverse[0]=0;c.PrepareIssNativeCamera();
  assert(!ready(c)&&!c.native_previous_valid);c.EndFrame();
  c.BeginFrame();c.PrepareIssNativeCamera();assert(!ready(c));c.EndFrame();
  c.BeginFrame();publish(3,1);c.PrepareIssNativeCamera();assert(!ready(c));c.EndFrame();
  c.BeginFrame();publish(4,2);c.PrepareIssNativeCamera();assert(ready(c));c.EndFrame();
  c.BeginFrame();publish(5,3,0x200);c.PrepareIssNativeCamera();assert(!ready(c));c.EndFrame();
  c.BeginFrame();publish(6,4,0x200);c.PrepareIssNativeCamera();assert(ready(c));c.EndFrame();
  c.BeginFrame();publish(71,4,0x200);c.PrepareIssNativeCamera();assert(!ready(c));c.EndFrame();
  c.BeginFrame();publish(72,4,0x200);latest.projection[8]=.03f;
  c.PrepareIssNativeCamera();assert(!ready(c)&&!c.native_previous_valid);
  c.EndFrame();c.BeginFrame();latest.sequence=73;latest.projection[8]=0;
  latest.vertical_fov=NAN;c.PrepareIssNativeCamera();assert(!ready(c)&&!c.native_previous_valid);
  c.EndFrame();c.BeginFrame();latest.sequence=0;c.PrepareIssNativeCamera();
  assert(!ready(c)&&!c.native_previous_valid);
 }else if(!std::strcmp(argv[1],"queued")){
  CameraMotion c;
  publish(100,0);const auto first=latest;
  publish(101,2);const auto second=latest;
  // CPU advances to a different camera before either queued frame executes.
  publish(160,99,0x900);
  c.SetIssSubmissionCamera(first);c.PrepareSubmitted();
  assert(c.native_previous_sequence==100&&c.native_previous_object==0x100);
  c.EndFrame();c.BeginFrame();
  c.SetIssSubmissionCamera(second);c.PrepareSubmitted();
  assert(c.native_previous_sequence==101&&ready(c));
  assert(std::abs(c.native_reprojection[3]+2)<1e-6);
  // Nested work inherits the snapshot; a later publication cannot change it.
  publish(161,-99,0xa00);c.PrepareSubmitted();
  assert(c.native_previous_sequence==101&&ready(c));
  // A genuinely different submitted camera still rejects history.
  c.EndFrame();c.BeginFrame();c.SetIssSubmissionCamera(latest);c.PrepareSubmitted();
  assert(!ready(c)&&c.native_reject_switch==1);
  // An absent submitted snapshot must not fall back to the live publisher.
  c.EndFrame();c.BeginFrame();c.SetIssSubmissionCamera({});c.PrepareSubmitted();
  assert(!ready(c)&&c.native_reject_missing==1);
 }else if(!std::strcmp(argv[1],"gates")){
  CameraMotion c;publish(1,0);
  c.for_upscaler=false;c.PrepareIssNativeCamera();assert(!c.native_frame_prepared);
  c.for_upscaler=true;c.depth_id=0;c.PrepareIssNativeCamera();assert(!c.native_frame_prepared);
  c.depth_id=1;c.PrepareIssNativeCamera();assert(c.native_frame_prepared);
 }else if(!std::strcmp(argv[1],"pause")){
  CameraMotion c;publish(1,0);c.PrepareIssNativeCamera();c.EndFrame();
  c.BeginFrame();publish(2,0);c.PrepareIssNativeCamera();assert(ready(c));c.EndFrame();
  c.BeginFrame();c.EndFrame(); // UI-only frame must invalidate the native pair.
  assert(!c.native_previous_valid);
  c.BeginFrame();c.PrepareIssNativeCamera();assert(!ready(c));
  c.EndFrame();c.BeginFrame();publish(3,0);c.PrepareIssNativeCamera();assert(!ready(c));
  c.EndFrame();c.BeginFrame();publish(4,0);c.PrepareIssNativeCamera();assert(ready(c));
 }else return 2;
}
'''
        # Existing cases explicitly select the command submission before each
        # preparation. A second selection cannot change an already latched pair.
        harness=harness.replace('c.PrepareIssNativeCamera();',
            'c.SetIssSubmissionCamera(latest);c.PrepareIssNativeCamera();')
        harness=harness.replace('FIELDS',fields).replace('INVALIDATE',invalidate).replace('SETTER',setter).replace('CLEANUP',cleanup).replace('METHOD',method)
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory);(p/'test.cpp').write_text(harness)
            subprocess.run(['c++','-std=c++20','-DISS_ANDROID_BIONIC','-Wall','-Wextra','-Werror','-I'+str(ROOT/'port'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
            for case in ('repeat','double','validation','pause','gates','queued'):
                with self.subTest(case=case):
                    subprocess.run([str(p/'test'),case],check=True)

if __name__=='__main__':unittest.main()
