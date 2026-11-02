"""Match submission-owned cameras to verified ISS material-instance transforms."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


class SceneCamera(unittest.TestCase):
    def test_captured_instances_auxiliary_camera_and_ambiguity(self):
        program = r'''
#include <cassert>
#include <limits>
#include "gpu/shim/iss_scene_camera.h"
static void invert(TlgIssCameraSnapshot& c) {
 double a[4][8]{};
 for(int i=0;i<4;++i)for(int j=0;j<4;++j){a[i][j]=c.composite[i*4+j];a[i][j+4]=i==j;}
 for(int i=0;i<4;++i){
  int best=i;for(int j=i+1;j<4;++j)if(std::abs(a[j][i])>std::abs(a[best][i]))best=j;
  assert(std::abs(a[best][i])>1e-10);
  for(int k=0;k<8;++k)std::swap(a[i][k],a[best][k]);
  const double d=a[i][i];for(int k=0;k<8;++k)a[i][k]/=d;
  for(int j=0;j<4;++j)if(j!=i){const double x=a[j][i];for(int k=0;k<8;++k)a[j][k]-=x*a[i][k];}
 }
 for(int i=0;i<4;++i)for(int j=0;j<4;++j)c.inverse[i*4+j]=a[i][j+4];
}
int main(){
 // Two different static objects from capture (3), not generated from the selector.
 std::array<float,53> first{-1,-1.19209e-7f,0,1.19209e-7f,-1,0,0,0,1,
  938.712f,-6.64553f,2.21591e-5f,-22.1591f,
  29.7173f,209.92f,-.000699963f,699.962f,
  -2.05244e-6f,1656.4f,8.87971e-5f,-88.797f};
 std::array<float,53> second{0,1,0,-1,0,0,0,0,1,
  -24.5672f,-173.539f,.000578654f,-578.653f,
  776.026f,-5.49383f,1.83188e-5f,-18.3188f,
  -1.69673e-6f,1369.33f,7.34079e-5f,-73.4078f};
 TlgIssCameraSnapshot main{};main.sequence=100;main.guest_object=0x100;
 main.near_plane=10;main.far_plane=10000000;main.vertical_fov=.8f;main.aspect=16.f/9;
 for(int r=0;r<3;++r)for(int c=0;c<4;++c)
  main.composite[r*4+c]=first[9+r*4+c]*(r<2?-1.f:1.f)/705.92f;
 main.composite[14]=10.0000095f;
 invert(main);std::memcpy(main.view_transform,main.composite,sizeof(main.composite));
 assert(IssSceneCamera::Matches(main,first.data()));
 assert(IssSceneCamera::Matches(main,second.data()));
 IssSceneCamera::Bank bank;bank.Publish(main);
 auto auxiliary=main;auxiliary.guest_object=0x200;auxiliary.sequence=101;
 for(int i=0;i<4;++i)auxiliary.composite[i]*=-1;
 invert(auxiliary);std::memcpy(auxiliary.view_transform,auxiliary.composite,sizeof(auxiliary.composite));
 bank.Publish(auxiliary);const auto submitted=bank.Read();TlgIssCameraSnapshot chosen{};
 assert(IssSceneCamera::Select(submitted,0x100,0x40b18d54,0,212,second.data(),chosen));
 assert(chosen.guest_object==0x100&&chosen.sequence==100);
 assert(!IssSceneCamera::Matches(auxiliary,second.data()));
 // Later CPU publication cannot change a captured submission's candidates.
 main.sequence=102;bank.Publish(main);assert(submitted.samples[0].sequence==100);
 // Indistinguishable parallel cameras at different origins are not guessed.
 auto ambiguous=main;ambiguous.sequence=103;ambiguous.guest_object=0x300;
 for(int j=0;j<4;++j)ambiguous.view_transform[12+j]-=20*ambiguous.composite[j];
 bank.Publish(ambiguous);
 assert(!IssSceneCamera::Select(bank.Read(),0x100,0x40b18d54,0,212,second.data(),chosen));
 // Equivalent aliases retain the established scene object, despite publication order.
 ambiguous.view_transform[0]=main.view_transform[0];
 std::memcpy(ambiguous.view_transform,main.view_transform,sizeof(main.view_transform));
 ambiguous.sequence=104;bank.Publish(ambiguous);
 assert(IssSceneCamera::Select(bank.Read(),0x100,0x40b18d54,0,212,second.data(),chosen));
 assert(chosen.guest_object==0x100);
 assert(!IssSceneCamera::Select(submitted,0,0x123,0,212,second.data(),chosen));
 assert(!IssSceneCamera::Select(submitted,0,0x40b18d54,1,212,second.data(),chosen));
 assert(!IssSceneCamera::Select(submitted,0,0x40b18d54,0,211,second.data(),chosen));
 auto bad=second;bad[0]=std::numeric_limits<float>::quiet_NaN();
 assert(!IssSceneCamera::Matches(main,bad.data()));
 bad=second;for(int i=0;i<9;++i)bad[i]=0;
 assert(!IssSceneCamera::Matches(main,bad.data()));
 auto stale=submitted;stale.sequence=200;
 assert(!IssSceneCamera::Select(stale,0,0x40b18d54,0,212,second.data(),chosen));
 auto invalid=submitted;invalid.samples[0].inverse[0]=0;
 assert(!IssSceneCamera::Select(invalid,0,0x40b18d54,0,212,second.data(),chosen));
 // Bounded storage: publish more camera objects than fit; no allocation or growth.
 for(unsigned i=0;i<100;++i){main.sequence=200+i;main.guest_object=0x1000+i;bank.Publish(main);}
 assert(bank.Read().count==TLG_ISS_CAMERA_CAPACITY);
 assert(bank.Read().sequence==299);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory)
            (p / 'test.cpp').write_text(program)
            subprocess.run(['c++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(ROOT / 'port'), str(p / 'test.cpp'),
                            '-o', str(p / 'test')], check=True)
            subprocess.run([str(p / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()
