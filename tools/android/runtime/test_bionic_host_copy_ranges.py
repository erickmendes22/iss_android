"""Compile production source tracking and copy/wait methods against controlled host copies."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_bionic_resource_reuse import method

ROOT = Path(__file__).resolve().parents[3]
GPU = ROOT / "port/gpu/shadps4/video_core"
CXX = shutil.which("c++")


@unittest.skipUnless(CXX, "host C++ compiler unavailable")
class HostCopyRangeTests(unittest.TestCase):
    def run_cpp(self, code, flags=(), cases=((),)):
        with tempfile.TemporaryDirectory() as directory:
            src = Path(directory) / "copy.cpp"
            exe = Path(directory) / "copy"
            src.write_text(code)
            result = subprocess.run([CXX, "-std=c++20", "-O2", "-pthread", *flags,
                                     "-I", str(ROOT / "port/gpu/shadps4"),
                                     "-I", str(ROOT / "port/gpu/shim"), str(src), "-o", str(exe)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for args in cases:
                result = subprocess.run([str(exe), *args], capture_output=True, text=True, timeout=8)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_ranges_collisions_overflow_and_retirement(self):
        self.run_cpp(r'''
#include <cassert>
#include "video_core/renderer_vulkan/host_copy_sources.h"
using Vulkan::HostCopySources;
int main() {
    HostCopySources sources;
    sources.Note(0x1ffff,2); // crosses a 64 KiB boundary
    assert(sources.Overlaps(0x10000,1) && sources.Overlaps(0x20000,1));
    assert(!sources.Overlaps(0x30000,4) && !sources.Overlaps(0x10000,0));
    auto seen=sources.Snapshot();
    sources.Note(0x10000,8); // newer note in an older slot survives retirement
    sources.RetireThrough(seen);
    assert(sources.Overlaps(0x10000,1) && !sources.Overlaps(0x20000,1));
    sources.RetireThrough(sources.Snapshot());
    assert(!sources.Overlaps(0x10000,1));
    unsigned a=0,b=1;
    while(HostCopySources::SlotOf(a)!=HostCopySources::SlotOf(b)) ++b;
    sources.Note(uint64_t(a)<<16,8);sources.Note(uint64_t(b)<<16,8);
    assert(sources.Overlaps(uint64_t(a)<<16,8));
    assert(sources.Overlaps(uint64_t(b)<<16,8));
    sources.RetireThrough(sources.Snapshot());
    sources.Note(0,5ull<<20); // oversized range conservatively overlaps every destination
    assert(sources.Overlaps(0xf0000000,8));
    sources.RetireThrough(sources.Snapshot());
    sources.Note(UINT64_MAX-3,8); // overflowing note also becomes an everywhere hazard
    assert(sources.Overlaps(0x20000,8));
    sources.RetireThrough(sources.Snapshot());
    assert(sources.Overlaps(UINT64_MAX-3,8)); // overflowing query always waits
    sources.Note(0x10000,0);assert(!sources.Overlaps(0x10000,8));
}
''')

    def test_production_selective_wait_preserves_copy_source_and_new_notes(self):
        source = (GPU / "renderer_vulkan/vk_scheduler.cpp").read_text()
        bodies = "\n".join(method(source, sig) for sig in (
            "bool Scheduler::HostCopyRangesEnabled()", "void Scheduler::NoteHostCopySource(u64 address, u64 size)",
            "void Scheduler::WaitHostCopiesFor(u64 address, u64 size)", "void Scheduler::WaitHostCopies()"))
        code = r'''
#include <cassert>
#include <future>
#include <functional>
#include <thread>
#include "iss_bionic_perf.h"
#include "video_core/renderer_vulkan/tlou_host_copy_wait.h"
namespace Detail = Vulkan::Detail;
#include "video_core/renderer_vulkan/host_copy_sources.h"
using u64=uint64_t;using namespace std::chrono_literals;
namespace TlgStats {
inline int host_copies_wait_ns,copy_threads_wait_ns;
inline std::atomic<unsigned> host_copy_waits{0};
struct WaitTimer {WaitTimer(int&) {}};
}
namespace TlgCopy {
std::function<void()> waiter=[]{};std::atomic<unsigned> waits{0};
void WaitAsync() {++waits;waiter();}
}
using Vulkan::HostCopySources;
struct Scheduler {
    HostCopySources host_copy_sources;u64 host_copies_issued=0;
    std::atomic<u64> host_copies_done{0};
    static bool HostCopyRangesEnabled();
    void NoteHostCopySource(u64,u64);void WaitHostCopiesFor(u64,u64);void WaitHostCopies();
    void KickRecording(bool) {host_copies_done.store(host_copies_issued);}
};
''' + bodies + r'''
int main(int argc,char**) {
    unsetenv("ISS_COMPARE_STATS");unsetenv("TLG_HOST_COPY_RANGES");
    if(argc>1) setenv("TLG_HOST_COPY_RANGES","0",1);
    Scheduler scheduler;
    if(!Scheduler::HostCopyRangesEnabled()) {
        scheduler.WaitHostCopiesFor(0x20000,8);assert(TlgCopy::waits==1);return 0;
    }
    int source=7,snapshot=0;
    scheduler.NoteHostCopySource(0x10000,4);
    scheduler.WaitHostCopiesFor(0x20000,4); // disjoint WRITE_DATA need not flush other work
    assert(TlgCopy::waits==0);
    std::promise<void> entered,allow_copy;
    auto entered_wait=entered.get_future();auto release=allow_copy.get_future();
    TlgCopy::waiter=[&]{entered.set_value();release.wait();snapshot=source;};
    auto writer=std::async(std::launch::async,[&] {
        scheduler.WaitHostCopiesFor(0x10000,4);source=9;
    });
    assert(entered_wait.wait_for(2s)==std::future_status::ready);
    assert(writer.wait_for(0s)!=std::future_status::ready);
    scheduler.NoteHostCopySource(0x30000,4); // registered after the wait's snapshot
    allow_copy.set_value();writer.get();
    assert(snapshot==7 && source==9 && TlgCopy::waits==1);
    assert(!scheduler.host_copy_sources.Overlaps(0x10000,4));
    assert(scheduler.host_copy_sources.Overlaps(0x30000,4));
    TlgCopy::waiter=[]{};
    scheduler.WaitHostCopiesFor(0x30000,4);assert(TlgCopy::waits==2);
    scheduler.WaitHostCopiesFor(0x30000,4);assert(TlgCopy::waits==2);
}
'''
        self.run_cpp(code, ["-D__ANDROID__=1", "-DISS_ANDROID_BIONIC=1"], cases=((), ("off",)))
        self.run_cpp(code)  # Glibc/desktop keep the original full-wait policy

    def test_direct_small_copy_snapshots_before_source_overwrite(self):
        body = method((GPU / "buffer_cache/buffer_cache.cpp").read_text(),
                      "void BufferCache::SmallGuestCopy(const TlgCopy::Item& item)")
        self.run_cpp(r'''
#include <cassert>
#include <cstdint>
#include <functional>
#include <vector>
namespace TlgCopy {
struct Item {void (*run)(const Item&);uint64_t source,size;};
std::vector<Item> queued;
void QueueCopy(const Item& item) {queued.push_back(item);}
}
namespace TlgToggle {enum {PoolSmallCopies};bool disabled=false;bool Disabled(int){return disabled;}}
namespace Vulkan {
struct Scheduler {
    static inline bool enabled=true;bool deferred=false;unsigned notes=0;
    std::vector<std::function<void()>> work;
    static bool HostCopyRangesEnabled(){return enabled;}
    bool IsRecordingDeferred(){return deferred;}
    template<class F> void RecordHostCopy(F&& f){work.emplace_back(std::forward<F>(f));}
    void NoteHostCopySource(uint64_t,uint64_t){++notes;}
};
}
struct BufferCache {Vulkan::Scheduler scheduler;void SmallGuestCopy(const TlgCopy::Item&);};
int source=7,snapshot=0;
void Copy(const TlgCopy::Item&){snapshot=source;}
''' + body + r'''
int main(){
    TlgCopy::Item item{Copy,0x10000,4};BufferCache cache;
    cache.SmallGuestCopy(item);source=9;
    assert(snapshot==7 && TlgCopy::queued.empty() && cache.scheduler.work.empty());
    cache.scheduler.deferred=true;snapshot=0;
    cache.SmallGuestCopy(item);assert(snapshot==0 && cache.scheduler.notes==1);
    cache.scheduler.work.front()();assert(snapshot==9);
    TlgToggle::disabled=true;cache.SmallGuestCopy(item);
    assert(TlgCopy::queued.size()==1 && cache.scheduler.notes==2);
    Vulkan::Scheduler::enabled=false;cache.scheduler.deferred=false;
    cache.SmallGuestCopy(item);assert(TlgCopy::queued.size()==2);
}
''')

    def test_write_data_keeps_fences_and_waits_for_its_destination(self):
        body = method((GPU / "amdgpu/liverpool.cpp").read_text(),
                      "void RunWriteData(Vulkan::Rasterizer& rasterizer, const u8* data)")
        self.run_cpp(r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <type_traits>
using u8=uint8_t;using u32=uint32_t;using u64=uint64_t;using VAddr=u64;
u32 destination=7;int phase=0;
struct PM4Header {struct {struct {u32 Value() const{return 3;}} count;} type3;};
struct PM4CmdWriteData {
    PM4Header header;u64 address;u32 data[1];
    template<class T> T Address() const {
        if constexpr(std::is_pointer_v<T>) return reinterpret_cast<T>(address);
        else return static_cast<T>(address);
    }
};
namespace Vulkan {
struct Rasterizer {
    void WaitHostCopies(){assert(false && "global wait would stall unrelated copies");}
    void WaitHostCopiesFor(VAddr address,u64 size) {
        assert(address==reinterpret_cast<VAddr>(&destination) && size==4 && phase==0);phase=1;
    }
    void WaitDeferredSignals(){assert(phase==1 && destination==7);phase=2;}
};
}
namespace TlgWriteLog {
enum {WriteData};
void Note(u64,const void*,u64,int){assert(phase==2 && destination==7);phase=3;}
}
''' + body + r'''
int main(){
    Vulkan::Rasterizer rasterizer;
    PM4CmdWriteData packet{{},reinterpret_cast<u64>(&destination),{99}};
    RunWriteData(rasterizer,reinterpret_cast<const u8*>(&packet));
    assert(destination==99 && phase==3);
}
''')
