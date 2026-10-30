"""Execute the real draw-pipe handoff for submission-owned ISS cameras."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]
GPU = ROOT / 'port/gpu/shadps4/video_core'


class SubmissionCameraOrder(unittest.TestCase):
    def test_actual_ordered_handoff_owns_its_bytes(self):
        source = (GPU / 'renderer_vulkan/vk_rasterizer.cpp').read_text()
        ordered = source[source.index('bool Rasterizer::RunInOrder('):
                         source.index('\n#ifdef ISS_ANDROID_BIONIC\nvoid Rasterizer::SetIssSubmissionCamera(')]
        setter = source[source.index('void Rasterizer::SetIssSubmissionCamera('):
                        source.index('\n#endif\n\nnamespace {\nvoid RunCommandTask')]
        harness = r'''
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>
#include "gpu/tlggpu.h"
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;
namespace TlgToggle {
constexpr u64 PipelinedTasks=1;
bool disabled=false;
bool Disabled(u64){return disabled;}
}
struct Camera {
 TlgIssCameraSnapshot selected{};
 void SetIssSubmissionCamera(const TlgIssCameraSnapshot& s){selected=s;}
};
struct DrawPipe {
 static constexpr u32 ReasonRasterizer=1;
 std::vector<u64> pending;
 std::deque<std::vector<u64>> queue;
 u8* Begin(u32 n){pending.assign((n+7)/8,0);return reinterpret_cast<u8*>(pending.data());}
 void Commit(u32){queue.push_back(std::move(pending));}
};
class Rasterizer {
public:
 using OrderedTask=void(*)(Rasterizer&,const u8*);
 Camera camera;Camera* camera_motion=&camera;
 DrawPipe pipe;DrawPipe* draw_pipe=&pipe;
 bool enabled=true;
 std::vector<u64> observed;
 bool UseDrawPipe()const{return enabled;}
 void DrainDrawPipe(u32);
 bool RunInOrder(OrderedTask,const void*,u32,u64=TlgToggle::PipelinedTasks);
 void SetIssSubmissionCamera(const TlgIssCameraSnapshot&);
};
enum class PacketKind:u32{Task};
struct TaskPacket{PacketKind kind;u32 size;Rasterizer::OrderedTask task;};
constexpr u32 AlignPacket(u32 n){return (n+7)&~7u;}
void Rasterizer::DrainDrawPipe(u32){
 while(!pipe.queue.empty()){
  auto bytes=std::move(pipe.queue.front());pipe.queue.pop_front();
  const auto* data=reinterpret_cast<const u8*>(bytes.data());
  const auto* packet=reinterpret_cast<const TaskPacket*>(data);
  packet->task(*this,data+sizeof(TaskPacket));
 }
}
ORDERED
SETTER
static void draw(Rasterizer& r,const u8*){r.observed.push_back(r.camera.selected.sequence);}
int main(){
 Rasterizer r;
 const u8 payload=0;
 TlgIssCameraSnapshot a{};a.sequence=100;a.guest_object=0x100;a.composite[12]=3;
 r.SetIssSubmissionCamera(a);r.RunInOrder(draw,&payload,sizeof(payload));
 a.sequence=999;a.composite[12]=999;
 TlgIssCameraSnapshot b{};b.sequence=101;b.guest_object=0x100;b.composite[12]=5;
 r.SetIssSubmissionCamera(b);r.RunInOrder(draw,&payload,sizeof(payload));
 // Nested IB draws do not select the CPU publisher again.
 r.RunInOrder(draw,&payload,sizeof(payload));b.sequence=1000;
 assert(r.observed.empty());r.DrainDrawPipe(1);
 assert((r.observed==std::vector<u64>{100,101,101}));
 assert(r.camera.selected.sequence==101&&r.camera.selected.composite[12]==5);
 // Switching to direct recording drains earlier work before the new camera.
 b.sequence=102;r.SetIssSubmissionCamera(b);r.RunInOrder(draw,&payload,sizeof(payload));
 r.enabled=false;a.sequence=200;r.SetIssSubmissionCamera(a);
 assert(r.observed.back()==102&&r.camera.selected.sequence==200);
 r.RunInOrder(draw,&payload,sizeof(payload));assert(r.observed.back()==200);
 // Missing cameras stay missing; disabling queued tasks uses the same fallback.
 r.enabled=true;TlgToggle::disabled=true;r.SetIssSubmissionCamera({});
 assert(r.camera.selected.sequence==0);r.RunInOrder(draw,&payload,sizeof(payload));
 assert(r.observed.back()==0&&r.pipe.queue.empty());
}
'''.replace('ORDERED', ordered).replace('SETTER', setter)
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            (p / 'test.cpp').write_text(harness)
            subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(ROOT / 'port'), str(p / 'test.cpp'),
                            '-o', str(p / 'test')], check=True)
            subprocess.run([str(p / 'test')], check=True)

    def test_submission_capture_precedes_delayed_execution(self):
        source = (GPU / 'amdgpu/liverpool.cpp').read_text()
        submit = source[source.index('void Liverpool::SubmitGfx('):
                        source.index('void Liverpool::SubmitAsc(')]
        self.assertLess(submit.index('tlggpu_iss_camera_read(&iss_camera)'),
                        submit.index('draw_prep->Build(dcb)'))
        self.assertIn(', iss_camera', submit)
        process = source[source.index('Liverpool::Task Liverpool::ProcessGraphics('):]
        self.assertIn('std::optional<TlgIssCameraSnapshot> iss_camera', process)
        self.assertLess(process.index('rasterizer->SetIssSubmissionCamera(*iss_camera)'),
                        process.index('draw_prep->BeginSubmission'))
        # The default optional is empty for nested IBs. Never use a coroutine
        # reference to SubmitGfx's stack-local snapshot or re-read on resume.
        header = (GPU / 'amdgpu/liverpool.h').read_text()
        self.assertIn('std::optional<TlgIssCameraSnapshot> iss_camera = std::nullopt', header)
        camera = (GPU / 'renderer_vulkan/vk_camera_motion.cpp').read_text()
        self.assertNotIn('tlggpu_iss_camera_read(', camera)
        self.assertIn('const TlgIssCameraSnapshot sample = native_submission_camera;', camera)


if __name__ == '__main__':
    unittest.main()
