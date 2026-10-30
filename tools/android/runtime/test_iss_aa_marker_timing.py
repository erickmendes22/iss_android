"""Compile the production AA marker against fake cache/camera boundaries."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / 'port/gpu/shadps4/video_core/renderer_vulkan/vk_temporal_upscaler.cpp'

class AaMarkerTiming(unittest.TestCase):
    def test_actual_marker_with_delayed_camera(self):
        source = SOURCE.read_text()
        marker = source[source.index('void TemporalUpscaler::OnIssAaOutputMarker('):
                        source.index('void TemporalUpscaler::RunScaled()')]
        harness = r'''
#include <cassert>
#include <cstdio>
#include <cstdint>
using u32=uint32_t; using u64=uint64_t;
namespace vk {
enum class Format { eR8G8B8A8Unorm=37 };
namespace ImageUsageFlagBits { enum { eSampled=1, eTransferSrc=2 }; }
}
namespace FrameCapture { bool IsDisplayBuffer(u64) { return false; } }
namespace TlgSettings {
constexpr int UpscalerFsr3=3;
struct Settings { int upscaler=3; };
Settings& Get(){static Settings s; return s;}
}
struct Image {
 struct { u64 guest_address; vk::Format pixel_format=vk::Format::eR8G8B8A8Unorm;
          struct {u32 width=640,height=360;} size; } info;
 unsigned usage_flags=3;
};
struct Cache {
 Image images[4]{};
 Cache(){images[1].info.guest_address=0x100;images[2].info.guest_address=0x200;}
 int FindImageFromRange(u64 address,int,bool){return address==0x100?1:address==0x200?2:0;}
 Image& GetImage(int id){return images[id];}
};
struct Camera {
 bool ready=false, ready_after_latch=false, object_ready=false; int depth=3,latched=0;
 bool Ready(){return ready;} int Depth(){return depth;}
 bool IssObjectMotionReady(u32 w,u32 h){return depth && object_ready && w==640 && h==360;}
 void PrepareIssNativeCamera(){++latched;ready=ready_after_latch;}
};
struct Encoding {int bytes;};
Encoding AaEncoding(vk::Format f){return {f==vk::Format::eR8G8B8A8Unorm?4:0};}
class TemporalUpscaler {
public:
 bool scaled_session=true,done_this_frame=false,failed=false,ui_phase=false;
 u32 render_width=640,render_height=360;
 int iss_aa_prefilter=0,ldr_target=0,dispatches=0;
 struct {u64 address=0;u32 format=0,width=0,height=0;} iss_aa_capture;
 Camera camera_motion; Cache texture_cache;
 bool Scaled(){return true;} bool Active(){return true;}
 void RunScaled(){assert(camera_motion.Ready() || camera_motion.IssObjectMotionReady(640,360));++dispatches;ui_phase=true;}
 void OnIssAaOutputMarker(u64,u32,u32,u64);
};
'''
        cases = r'''
int main(){
 // Capture19 route: unavailable camera must not discard this frame's source.
 TemporalUpscaler delayed;
 delayed.OnIssAaOutputMarker(0x100,640,360,0x200);
 assert(delayed.camera_motion.latched==1);
 assert(delayed.iss_aa_prefilter==2 && delayed.iss_aa_capture.address==0x200);
 assert(delayed.ldr_target==1 && delayed.dispatches==0);
 // Current completed sample may be ready even when the prior boundary was not.
 TemporalUpscaler recovered;recovered.camera_motion.ready_after_latch=true;
 recovered.OnIssAaOutputMarker(0x100,640,360,0x200);
 assert(recovered.dispatches==1 && recovered.iss_aa_prefilter==2);
 // Conversely stale Ready() must not authorize a dispatch after latch rejects it.
 TemporalUpscaler stale;stale.camera_motion.ready=true;
 stale.OnIssAaOutputMarker(0x100,640,360,0x200);
 assert(stale.dispatches==0 && stale.iss_aa_prefilter==2);
 // Existing object-only fallback must run before HUD, not at display time.
 TemporalUpscaler objects;objects.camera_motion.object_ready=true;
 objects.OnIssAaOutputMarker(0x100,640,360,0x200);
 assert(objects.dispatches==1 && objects.iss_aa_prefilter==2 && objects.ui_phase);
 // Unknown input must not borrow a previous address; invalid target stays inert.
 recovered.done_this_frame=false;
 recovered.OnIssAaOutputMarker(0x100,640,360,0x999);
 assert(recovered.iss_aa_prefilter==0);
 TemporalUpscaler bad;bad.camera_motion.ready_after_latch=true;
 bad.OnIssAaOutputMarker(0x999,640,360,0x200);
 assert(bad.dispatches==0 && bad.iss_aa_prefilter==0);
 TemporalUpscaler mismatch;mismatch.camera_motion.ready_after_latch=true;
 mismatch.OnIssAaOutputMarker(0x100,1280,720,0x200);
 assert(mismatch.dispatches==0 && mismatch.camera_motion.latched==0);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            p=Path(directory)
            (p/'test.cpp').write_text(harness+marker+cases)
            subprocess.run(['c++','-std=c++20','-DISS_ANDROID_BIONIC','-Wall','-Wextra','-Werror',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)

if __name__=='__main__':
    unittest.main()
