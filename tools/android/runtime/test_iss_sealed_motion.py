"""Run actual ObjectMotion inline guards with production history across a sealed frame."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[3]
GPU=ROOT/'port/gpu/shadps4/video_core/renderer_vulkan'
def method(text, marker):
    start=text.index(marker);brace=text.index('{',start);depth=1;end=brace+1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
class SealedMotion(unittest.TestCase):
    def test_late_draws_cannot_advance_bone_history(self):
        header=(GPU/'vk_object_motion.h').read_text()
        methods='\n'.join(method(header,m) for m in ['bool Collecting()','bool Moving(','void SealIssScene()'])
        code='''#include <cassert>
#include "motion_history.h"
using u64=uint64_t;
namespace Vulkan { struct Fixture {
 bool enabled=true,scene_sealed=false;
 Motion::History history{4096,true};
'''+methods+'''
}; }
int main(){
 Vulkan::Fixture c;
 Vulkan::Motion::History::GateKey key{.shader=1,.geometry=2,.indices=3,.index_count=6,.instances=1};
 assert(c.Collecting());assert(!c.Moving(key,10));
 auto before=c.history.stats.still;
 c.SealIssScene();assert(!c.Collecting());
 // These late draws must neither grow occurrence/history tables nor pollute stats.
 for(int i=0;i<1000;i++)assert(!c.Moving(key,20+i));
 assert(c.history.stats.still==before);
 c.history.NextFrame();c.scene_sealed=false;
 assert(c.Collecting());assert(c.Moving(key,11));
 // A second occurrence has no matching opaque history: late passes cannot supply it.
 assert(!c.Moving(key,20));
 c.enabled=false;before=c.history.stats.still;
 assert(!c.Collecting());assert(!c.Moving(key,99));assert(c.history.stats.still==before);
}'''
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp);(p/'test.cpp').write_text(code)
            subprocess.run(['c++','-std=c++20','-DISS_ANDROID_BIONIC','-I'+str(GPU),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)
    def test_rasterizer_neutralizes_and_skips_late_cpu_reads(self):
        s=(GPU/'vk_rasterizer.cpp').read_text()
        draw=s[s.index('void Rasterizer::DrawRecord('):s.index('void Rasterizer::DrawRecord(')+16000]
        guard='if (motion_draw && motion_geometry && object_motion->Collecting() &&'
        self.assertLess(draw.index('push_data.motion_param = 0;'),draw.index(guard))
        self.assertLess(draw.index(guard),draw.index('palette = XXH3_64bits_withSeed'))
        self.assertLess(draw.index(guard),draw.index('object_motion->IndexRange('))
        self.assertIn('if (motion_draw && object_motion->Collecting() && !TlgToggle::Disabled(1u << 29) &&',s)
        self.assertIn('if (!Collecting() || TlgToggle::Disabled(1u << 29)',(GPU/'vk_object_motion.cpp').read_text())
