"""Exercise the production sequence policy without Vulkan or a device."""
import pathlib, shutil, subprocess, tempfile, unittest
ROOT = pathlib.Path(__file__).resolve().parents[3]
class TemporalCapture(unittest.TestCase):
 def test_bounded_sequence_and_owned_tokens(self):
  compiler=shutil.which('c++')
  if not compiler:self.skipTest('C++ compiler required')
  source=r'''
#include "iss_temporal_capture.h"
#include <cassert>
#include <array>
int main() {
 using C=Vulkan::IssTemporalCapture;
 C capture;
 assert(capture.Advance().index==-1);
 capture.Begin();
 std::array<C::Sample,3> displays{};
 for(int i=0;i<C::Frames;i++) {
  const auto sample=capture.Advance();
  assert(sample.index==i && sample.serial==unsigned(i+1));
  displays[i%3]=sample;
  if(i>2) assert(displays[(i+1)%3].index==i-2); // image tokens survive later scenes
 }
 for(int i=0;i<300;i++)assert(capture.Advance().index==-1);
 const auto old=displays[0];
 capture.Begin();auto first=capture.Advance();
 assert(first.index==0 && first.serial>old.serial);
 const auto hd=C::Center(1280,720);
 assert(hd.x==320 && hd.y==180 && hd.width==640 && hd.height==360);
 const auto odd=C::Center(1921,1081);
 assert(odd.x==640 && odd.y==360 && odd.width==640 && odd.height==360);
 const auto small=C::Center(320,180);
 assert(small.x==0 && small.y==0 && small.width==320 && small.height==180);
}
'''
  with tempfile.TemporaryDirectory() as tmp:
   p=pathlib.Path(tmp);(p/'test.cpp').write_text(source)
   subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror','-I',str(ROOT/'port/gpu/shadps4/video_core/renderer_vulkan'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
