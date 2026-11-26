"""Exercise production write merge, byte uploads, and the native mapping preflight."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_bionic_resource_reuse import method
from test_bionic_relaxed_deferred import PREAMBLE

ROOT = Path(__file__).resolve().parents[3]
CXX = shutil.which('c++')
SOURCE = ROOT / 'port/gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp'

@unittest.skipUnless(CXX, 'C++ compiler required')
class RelaxedWriteMerge(unittest.TestCase):
    def run_source(self, source, modes, bionic=True):
        with tempfile.TemporaryDirectory() as temp:
            src = Path(temp) / 'test.cpp'
            exe = Path(temp) / 'test'
            src.write_text(source)
            args = [CXX, '-std=c++20', '-O1', '-pthread', '-fsanitize=undefined',
                    '-fno-sanitize-recover=all', '-I', str(ROOT/'port/gpu/shim')]
            if bionic:
                args += ['-D__ANDROID__', '-DISS_ANDROID_BIONIC']
            compiled = subprocess.run(args + [str(src), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            for mode in modes:
                with self.subTest(mode=mode):
                    subprocess.run([str(exe), mode], check=True, capture_output=True, timeout=10)

    def test_merge_and_exact_upload(self):
        source = SOURCE.read_text()
        fixture = (ROOT / 'tools/android/runtime/cpp/relaxed_write_merge.cpp').read_text()
        fixture = fixture.replace('// PRODUCTION_TEST_QUEUE', PREAMBLE[PREAMBLE.index('struct Queue {'):PREAMBLE.index('struct Semaphore {')])
        fixture = fixture.replace('// PRODUCTION_MERGE_METHODS', '\n'.join(method(source, signature) for signature in [
            'bool BufferCache::CpuWriteMergeRelaxed(', 'bool BufferCache::CpuWriteMerge(',
            'void BufferCache::AddMergedWriteRange(', 'void BufferCache::FlushMergedWrites(']))
        self.run_source(fixture, ['short','copy','fill','snapshot','image','hostcopy','split','many','atomic',
                                 'failure','off','globaloff','cpuonly','dma','large','overlap','overflow','precise'])
        self.run_source(fixture, ['nonbionic'], bionic=False)

    def test_native_mapping_preflight(self):
        source = (ROOT / 'port/src/runtime_memory.c').read_text()
        fixture = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <sys/mman.h>
#include <algorithm>
struct Vma {uintptr_t start,end;int kind,prot;uint64_t phys;};
Vma vmas[8];size_t vma_count;constexpr int KIND_RESERVED=0;
unsigned char storage[16384];unsigned char* backing_base=storage;
int exclusive_depth=0;std::mutex table;
void read_lock(){table.lock();}void read_unlock(){table.unlock();}
size_t vma_index(uintptr_t a){for(size_t i=0;i<vma_count;++i)if(vmas[i].end>a)return i;return vma_count;}
// METHODS
void reset(){vma_count=1;vmas[0]={0x1000,0x1100,1,0x33,0};std::fill_n(storage,sizeof(storage),7);}
int main(int,char**){
 unsigned char data[512];std::fill_n(data,512,55);reset();
 assert(runtime_memory_cpu_write_can_queue());exclusive_depth=1;assert(!runtime_memory_cpu_write_can_queue());exclusive_depth=0;
 assert(runtime_memory_write_backing_cpu(0x1040,data,64));
 for(unsigned i=0;i<sizeof(storage);++i)assert(storage[i]==(i>=64&&i<128?55:7));
 reset();vma_count=2;vmas[1]={0x1100,0x1200,1,0x33,4096};
 assert(runtime_memory_write_backing_cpu(0x10c0,data,128));
 for(unsigned i=0;i<sizeof(storage);++i)assert(storage[i]==((i>=192&&i<256)||(i>=4096&&i<4160)?55:7));
 // Entire destination is preflighted: late gaps, reservations, permissions and aliases never write a prefix.
 for(int mode=0;mode<4;++mode){reset();vma_count=2;vmas[1]={0x1100,0x1200,1,0x33,4096};
  if(mode==0)vmas[1].start=0x1110;
  if(mode==1)vmas[1].kind=KIND_RESERVED;
  if(mode==2)vmas[1].prot=0x11;
  if(mode==3){vma_count=3;vmas[2]={0x2000,0x2100,1,0x33,4096};}
  assert(!runtime_memory_write_backing_cpu(0x10c0,data,128));for(auto x:storage)assert(x==7);
 }
 reset();vma_count=2;vmas[1]={0x2000,0x2040,1,0x33,64};
 assert(!runtime_memory_write_backing_cpu(0x1040,data,64));for(auto x:storage)assert(x==7);
 assert(runtime_memory_write_backing_cpu(0x1080,data,64)); // physical alias outside the written bytes
 reset();assert(!runtime_memory_write_backing_cpu(UINTPTR_MAX-31,data,64));
 assert(!runtime_memory_write_backing_cpu(0x1000,data,0));for(auto x:storage)assert(x==7);
}
'''
        fixture = fixture.replace('// METHODS', '\n'.join(method(source, signature) for signature in [
            'static int host_prot(', 'int runtime_memory_cpu_write_can_queue(',
            'int runtime_memory_write_backing_cpu(']))
        self.run_source(fixture, ['mapping'])

    def test_entrypoint_deadlock_guards(self):
        source = (ROOT / 'port/gpu/shim/tlggpu.cpp').read_text()
        fixture = r'''
#include <cstdint>
#include <cassert>
using VAddr=uint64_t;
enum class GpuReadbacksMode{Relaxed,Precise};
struct {GpuReadbacksMode GetReadbacksMode(){return GpuReadbacksMode::Relaxed;}}EmulatorSettings;
bool can_queue=true;int queue_checks=0;
int runtime_memory_cpu_write_can_queue(){++queue_checks;return can_queue;}
struct Cache{bool dirty=true;int merges=0,queries=0;bool IsRegionGpuModified(VAddr,uint64_t){++queries;return dirty;}
 bool CpuWriteMerge(VAddr,const void*,int,uint64_t){++merges;return true;}};
struct Raster{Cache cache;bool owner=false;int owner_checks=0;Cache& GetBufferCache(){return cache;}
 bool IsGpuSideThread(){++owner_checks;return owner;}};
namespace Core {struct Memory{Raster* raster=nullptr;static Memory* Instance(){static Memory m;return &m;}
 Raster* GetRasterizer(){return raster;}};}
// METHOD
int main(int,char**){
 assert(!tlggpu_cpu_write(nullptr,nullptr,0,64));Raster r;Core::Memory::Instance()->raster=&r;
 assert(!tlggpu_cpu_write(reinterpret_cast<void*>(UINT64_MAX-31),nullptr,0,64));
 assert(!tlggpu_cpu_write(reinterpret_cast<void*>(1ULL<<40),nullptr,0,64));
 assert(!tlggpu_cpu_write(reinterpret_cast<void*>((1ULL<<40)-32),nullptr,0,64));
 assert(!tlggpu_cpu_write(nullptr,nullptr,0,0));assert(r.cache.queries==0);
 r.cache.dirty=false;assert(!tlggpu_cpu_write(nullptr,nullptr,0,64));assert(!r.owner_checks&&!queue_checks);
 r.cache.dirty=true;r.owner=true;assert(!tlggpu_cpu_write(nullptr,nullptr,0,64));assert(!queue_checks);
 r.owner=false;can_queue=false;assert(!tlggpu_cpu_write(nullptr,nullptr,0,64));assert(!r.cache.merges);
 can_queue=true;assert(tlggpu_cpu_write(nullptr,nullptr,0,64));assert(r.cache.merges==1);
}
'''
        self.run_source(fixture.replace('// METHOD', method(source, 'extern "C" int tlggpu_cpu_write(')), ['guards'])

if __name__ == '__main__':
    unittest.main()
