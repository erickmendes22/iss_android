"""Run production readback dispatch/publication against a controlled GPU and command queue."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
from test_bionic_resource_reuse import method

ROOT = Path(__file__).resolve().parents[3]
SOURCE = ROOT / "port/gpu/shadps4/video_core/buffer_cache/buffer_cache.cpp"
CXX = shutil.which("c++")

PREAMBLE = r'''
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <tuple>
#include <vector>
#include "video_core/buffer_cache/readback_copy_pages.h"
using u8=uint8_t;using u64=uint64_t;using s64=int64_t;using VAddr=u64;
using namespace std::chrono_literals;
using VideoCore::ForEachReadbackCopyPageGroup;
enum class GpuReadbacksMode {Disabled,Relaxed,Precise};
struct {GpuReadbacksMode mode=GpuReadbacksMode::Relaxed;
    auto GetReadbacksMode() {return mode;}} EmulatorSettings;
namespace TlgToggle {enum {DeferredReadback}; bool Disabled(int) {return false;}}
bool ReadbackStatsEnabled() {return false;} s64 NowNs() {return 0;}
struct Counters {std::atomic<u64> count{},queue_ns{},flush_ns{},stale{},deferred_ns{},applied{},shared_completions{},mapped_stale{},mapped_applied{},mapped_bytes{},mapped_partial{},mapped_partial_kept{},mapped_partial_retry{},deferred_retries{},retry_applied{};};
Counters& Readbacks() {static Counters c;return c;} void ReportReadbacks() {}
struct Queue {
    std::mutex mutex;std::condition_variable cv;
    std::deque<std::function<void()>> jobs;bool stop=false;
    std::thread::id owner;std::atomic<unsigned> submitted{0};
    std::thread worker;
    Queue() {
        std::promise<void> ready;auto f=ready.get_future();
        worker=std::thread([this,&ready] {
            owner=std::this_thread::get_id();ready.set_value();
            std::unique_lock lock(mutex);
            for(;;) {
                cv.wait(lock,[&]{return stop||!jobs.empty();});
                if(stop&&jobs.empty()) return;
                auto job=std::move(jobs.front());jobs.pop_front();
                lock.unlock();job();lock.lock();
            }
        }); f.get();
    }
    ~Queue() {{std::lock_guard lock(mutex);stop=true;} cv.notify_one();worker.join();}
    void CheckOwner() {assert(std::this_thread::get_id()==owner);}
    template<bool wait,bool ordered,class F> void SendCommand(F&& func) {
        static_assert(wait && ordered);
        assert(std::this_thread::get_id()!=owner);
        auto job=std::make_shared<std::packaged_task<void()>>(std::forward<F>(func));
        auto done=job->get_future();
        {std::lock_guard lock(mutex);++submitted;jobs.emplace_back([job]{(*job)();});}
        cv.notify_one();assert(done.wait_for(3s)==std::future_status::ready);done.get();
    }
};
struct Semaphore {
    std::mutex mutex;std::condition_variable cv;u64 complete_tick=0;unsigned waiters=0;
    Queue* queue;
    void Wait(u64 tick) {
        assert(tick==1 || tick==2);assert(std::this_thread::get_id()!=queue->owner);
        std::unique_lock lock(mutex);++waiters;cv.notify_all();
        assert(cv.wait_for(lock,3s,[&]{return complete_tick>=tick;}));
    }
    void WaitForGuests(unsigned count) {
        std::unique_lock lock(mutex);
        assert(cv.wait_for(lock,2s,[&]{return waiters>=count;}));
    }
    void Complete(u64 tick=1) {std::lock_guard lock(mutex);complete_tick=std::max(complete_tick,tick);cv.notify_all();}
};
struct Scheduler {Semaphore timeline;auto GetWorkSemaphore() {return &timeline;}};
struct Tracker {
    Queue* queue;std::mutex mutex;bool gpu=true,cpu=false;unsigned marks=0;
    bool IsRegionGpuModified(VAddr,u64){std::lock_guard lock(mutex);return gpu;}
    void MarkRegionAsCpuModified(VAddr,u64) {queue->CheckOwner();cpu=true;++marks;}
    void MarkRegionAsCpuModifiedUnlessGpu(VAddr,u64) {
        // Same per-region locked GPU-bit recheck as production, also safe on guest threads.
        std::lock_guard lock(mutex);if(!gpu) {cpu=true;++marks;}
    }
    template<class P,class W> bool ApplyGpuCopyIf(VAddr start,u64 size,P&& pred,W&& write) {
        if(EmulatorSettings.mode==GpuReadbacksMode::Relaxed) queue->CheckOwner();
        std::lock_guard lock(mutex);
        if(!pred()) return false;
        if(gpu) write(start,start+size);
        gpu=false;return true;
    }
};
struct Memory {
    std::array<u8,16> bytes{};unsigned publications=0;
    void TryWriteBacking(void* dst,const void* src,u64 size) {
        assert(reinterpret_cast<uintptr_t>(dst)==0x1000 && size==16);
        std::memcpy(bytes.data(),src,size);++publications;
    }
};
struct Buffer {};
struct BufferCache {
    struct Copy {u64 srcOffset=0,dstOffset=0,size=16;};
    struct DeferredReadback {
        bool mapped=false,page_scoped=false,partial=false;u64 snapshot_size=0;std::vector<u8> mapped_snapshot;
        std::array<u8,16> snapshot;
        struct Staging {u8* mapped;u64 offset=0;void Invalidate() {}} staging;
        VAddr arena_base=0x1000,start=0x1000,end=0x1010;u64 tick=1;
        std::atomic<bool> applied{},recovered{},stale{};std::mutex mutex;
        std::once_flag relaxed_completion;
        std::vector<Copy> copies{Copy{}},publication_copies,retry_copies;
        DeferredReadback():staging{snapshot.data()} {snapshot.fill(7);}
    };
    struct Ranges {void Add(VAddr,u64){assert(false);}} gpu_modified_ranges;
    Queue* liverpool;Scheduler scheduler;Tracker* memory_tracker;Memory* memory;
    Buffer arena;std::shared_ptr<DeferredReadback> pending=std::make_shared<DeferredReadback>();
    bool CaptureMappedReadback(DeferredReadback&) {assert(false);return false;}
    bool empty=false,replace_stale=false,replace_partial=false,empty_on_retry=false;unsigned downloads=0;std::atomic<unsigned> starts{0};
    std::mutex starts_mutex;std::condition_variable starts_cv;
    BufferCache(Queue* q,Tracker* t,Memory* m):liverpool(q),memory_tracker(t),memory(m) {
        scheduler.timeline.queue=q;
    }
    auto GetReadbackWindow(VAddr,u64) {liverpool->CheckOwner();return std::tuple{&arena,0x1000ull,0x2000ull};}
    void DownloadMemory(const Buffer*,VAddr,u64) {
        liverpool->CheckOwner();++downloads;memory->bytes.fill(9);memory_tracker->gpu=false;
    }
    auto StartDeferredReadback(VAddr,u64) {
        liverpool->CheckOwner();
        if(pending->stale && empty_on_retry) {
            empty=true;memory_tracker->gpu=false;memory->bytes.fill(13);
        }
        if((pending->stale && replace_stale) || (starts && replace_partial && pending->partial)) {
            pending=std::make_shared<DeferredReadback>();pending->tick=2;
            pending->snapshot.fill(11); // fresh GPU generation, not the old copy's bytes
        }
        liverpool->CheckOwner();{std::lock_guard lock(starts_mutex);++starts;}starts_cv.notify_all();
        return empty ? std::shared_ptr<DeferredReadback>{} : pending;
    }
    void WaitForStarts(unsigned count) {
        std::unique_lock lock(starts_mutex);
        assert(starts_cv.wait_for(lock,2s,[&]{return starts>=count;}));
    }
    static bool DeferredReadbackEnabled();
    bool FinishDeferredReadback(DeferredReadback&,bool);
    void ReadMemory(VAddr,u64,bool,bool,const std::function<bool()>& = {});
};
'''

MAIN = r'''
int main(int argc,char** argv) {
    assert(argc==2);std::string scenario=argv[1];
    setenv("TLG_DEFERRED_READBACK","1",1);unsetenv("TLG_HOT_PAGES");
    if(scenario=="disabled") EmulatorSettings.mode=GpuReadbacksMode::Disabled;
    if(scenario=="precise") EmulatorSettings.mode=GpuReadbacksMode::Precise;
    if(scenario=="off") setenv("TLG_DEFERRED_READBACK","0",1);
    if(scenario=="hot") setenv("TLG_HOT_PAGES","1",1);
    if(scenario=="retry_off")setenv("TLG_READBACK_RETRY","0",1);
    bool expect=EmulatorSettings.mode==GpuReadbacksMode::Precise ||
        (NATIVE_BIONIC && scenario!="disabled" && scenario!="off" && scenario!="hot");
    assert(BufferCache::DeferredReadbackEnabled()==expect);
    Queue queue;Tracker tracker{&queue};Memory memory;BufferCache cache{&queue,&tracker,&memory};
    const bool retry=scenario=="retry" || scenario=="retry_join";
    cache.replace_stale=retry;
    cache.empty_on_retry=scenario=="retry_empty";
    const bool write=scenario!="read";
    if(scenario=="inline") {
        queue.SendCommand<true,true>([&]{cache.ReadMemory(0x1000,8,true,true);});
        assert(cache.starts==0 && cache.downloads==1 && tracker.cpu);return 0;
    }
    if(!expect) {
        cache.ReadMemory(0x1000,8,true,false);
        assert(cache.starts==0 && cache.downloads==1 && tracker.cpu);return 0;
    }
    if(scenario=="empty") {
        cache.empty=true;cache.ReadMemory(0x1000,8,true,false);
        assert(cache.starts==1 && cache.downloads==0 && tracker.cpu);return 0;
    }
    if(scenario=="obsolete" || scenario=="recovered") {
        if(scenario=="obsolete")cache.pending->stale=true;
        else cache.pending->recovered=true;
        cache.ReadMemory(0x1000,8,true,false);
        assert(cache.scheduler.timeline.waiters==0 && cache.starts==2 && cache.downloads==1);
        assert(memory.publications==0 && tracker.cpu);return 0;
    }
    if(scenario=="ready" || scenario=="ready_new_write") {
        // Model Start's already published mapped result. A subsequent GPU write
        // must keep its GPU bit even though this older request completed inline.
        cache.pending->mapped=true;cache.pending->applied=true;
        tracker.gpu=scenario=="ready_new_write";
        cache.ReadMemory(0x1000,8,true,false);
        assert(cache.starts==1 && cache.downloads==0 && queue.submitted==1);
        assert(cache.scheduler.timeline.waiters==0 && memory.publications==0);
        assert(tracker.cpu==(scenario=="ready"));
        assert(tracker.gpu==(scenario=="ready_new_write"));return 0;
    }
    if(scenario=="partial_valid" || scenario=="partial_retry") {
        const bool invalid=scenario=="partial_retry";
        if(!NATIVE_BIONIC)return 0;
        cache.pending->mapped=true;cache.pending->partial=true;cache.pending->applied=true;
        tracker.gpu=invalid;
        if(!invalid){cache.ReadMemory(0x1000,8,true,false);
            assert(queue.submitted==1&&tracker.cpu&&memory.publications==0);return 0;}
        // Start's first completed partial snapshot excluded the requested page.
        // The next Start returns the newer GPU generation, still incomplete.
        cache.replace_partial=true;
        auto fault=std::async(std::launch::async,[&]{cache.ReadMemory(0x1000,8,true,false);});
        cache.WaitForStarts(2);cache.scheduler.timeline.WaitForGuests(1);
        queue.SendCommand<true,true>([&]{assert(cache.downloads==0&&memory.publications==0);});
        assert(fault.wait_for(0s)!=std::future_status::ready);
        cache.scheduler.timeline.Complete(2);
        assert(fault.wait_for(3s)==std::future_status::ready);fault.get();
        assert(cache.starts==2&&cache.downloads==0&&memory.publications==1&&tracker.cpu&&!tracker.gpu);
        for(u8 b:memory.bytes)assert(b==11);return 0;
    }
    if(scenario=="prepare_accept" || scenario=="prepare_reject" || scenario=="prepare_retry" || scenario=="prepare_exhaust") {
        unsigned proofs=0;
        const bool accept=scenario=="prepare_accept",retry_prepare=scenario=="prepare_retry",
                   exhaust=scenario=="prepare_exhaust";
        if(retry_prepare&&!NATIVE_BIONIC)return 0;
        cache.replace_stale=retry_prepare;
        auto fault=std::async(std::launch::async,[&]{
            cache.ReadMemory(0x1000,8,true,false,[&] {
                queue.CheckOwner();++proofs;return accept;
            });
        });
        if(accept) {
            assert(fault.wait_for(3s)==std::future_status::ready);fault.get();
            assert(proofs==1&&queue.submitted==1&&cache.starts==0&&cache.downloads==0);
            assert(memory.publications==0&&tracker.gpu&&!tracker.cpu);return 0;
        }
        cache.scheduler.timeline.WaitForGuests(1);
        assert(cache.starts==1&&queue.submitted==1); // proof and recording are ONE command
        queue.SendCommand<true,true>([&]{
            assert(cache.downloads==0&&memory.publications==0);
            if(retry_prepare||exhaust)cache.pending->stale=true;
        });
        assert(fault.wait_for(0s)!=std::future_status::ready); // GPU owner remains available
        cache.scheduler.timeline.Complete();
        if(retry_prepare){cache.WaitForStarts(2);cache.scheduler.timeline.WaitForGuests(2);
            cache.scheduler.timeline.Complete(2);}
        assert(fault.wait_for(3s)==std::future_status::ready);fault.get();
        assert(proofs==1&&cache.downloads==unsigned(exhaust)&&memory.publications==unsigned(!exhaust));
        assert(cache.starts==((retry_prepare||(exhaust&&NATIVE_BIONIC))?2u:1u));
        assert(queue.submitted==((retry_prepare||(exhaust&&NATIVE_BIONIC))?4u:3u));
        assert(!tracker.gpu&&tracker.cpu);
        for(u8 b:memory.bytes)assert(b==(exhaust?9:retry_prepare?11:7));return 0;
    }
    auto fault=std::async(std::launch::async,[&]{cache.ReadMemory(0x1000,8,write,false);});
    cache.scheduler.timeline.WaitForGuests(1);
    std::vector<std::future<void>> others;
    const unsigned guests=scenario=="many" || scenario=="retry_join" ? 16 : scenario=="join" ? 2 : 1;
    for(unsigned i=1;i<guests;++i) {
        others.push_back(std::async(std::launch::async,[&]{cache.ReadMemory(0x1008,8,true,false);}));
    }
    cache.WaitForStarts(guests);
    // Crucial regression: command recording progresses while the faulting guest waits.
    bool progressed=false;
    queue.SendCommand<true,true>([&] {
        progressed=true;
        if(scenario=="stale" || scenario=="retry_off" || scenario=="retry_empty" || retry) cache.pending->stale.store(true);
    });
    assert(progressed && fault.wait_for(0s)!=std::future_status::ready);
    if(scenario=="race") {
        // Delay the publication request behind a GPU write that invalidates the snapshot
        // *after* the guest's semaphore completes. The real Finish must recheck staleness.
        queue.SendCommand<true,true>([&] {
            cache.scheduler.timeline.Complete();
            cache.pending->stale.store(true);
        });
    } else cache.scheduler.timeline.Complete();
    if(retry) {
        cache.WaitForStarts(2*guests);
        cache.scheduler.timeline.WaitForGuests(2);
        // The replacement GPU generation is incomplete, yet the real owner still
        // processes commands. A synchronous DownloadMemory fallback would block it.
        queue.SendCommand<true,true>([&]{assert(cache.downloads==0 && tracker.gpu);});
        assert(fault.wait_for(0s)!=std::future_status::ready);
        cache.scheduler.timeline.Complete(2);
    }
    assert(fault.wait_for(3s)==std::future_status::ready);fault.get();
    for(auto& other:others) {assert(other.wait_for(3s)==std::future_status::ready);other.get();}
    if(scenario=="retry_empty") {
        assert(cache.starts==2 && cache.downloads==0 && !tracker.gpu && tracker.cpu);
        assert(memory.publications==0);for(u8 b:memory.bytes)assert(b==13);return 0;
    }
    if(guests>1) {
        // One start request per guest, a single publication and the progress probe above.
        assert(cache.scheduler.timeline.waiters==(retry?2u:1u));
        assert(queue.submitted==(retry?2*guests+3:guests+2));
    }
    bool stale=scenario=="stale"||scenario=="race"||scenario=="retry_off";
    if(stale)assert(cache.starts==(NATIVE_BIONIC && scenario!="retry_off"?2u:1u));
    assert(cache.downloads==unsigned(stale));
    assert(memory.publications==unsigned(!stale));
    assert(!tracker.gpu && tracker.cpu==write);
    for(u8 b:memory.bytes) assert(b==(stale?9:retry?11:7));
}
'''


@unittest.skipUnless(CXX, "host C++ compiler unavailable")
class RelaxedDeferredTests(unittest.TestCase):
    def run_config(self, bionic):
        source = SOURCE.read_text()
        bodies = "\n".join(method(source, signature) for signature in (
            "bool BufferCache::DeferredReadbackEnabled()",
            "bool BufferCache::FinishDeferredReadback(DeferredReadback& readback, bool wait)",
            "void BufferCache::ReadMemory(VAddr device_addr, u64 size, bool is_write, bool assume_locks,"))
        with tempfile.TemporaryDirectory() as directory:
            src = Path(directory) / "readback.cpp"
            binary = Path(directory) / "readback"
            src.write_text(PREAMBLE + bodies + MAIN)
            flags = ["-D__ANDROID__=1", "-DISS_ANDROID_BIONIC=1"] if bionic else []
            result = subprocess.run([CXX, "-std=c++20", "-O2", "-pthread", *flags,
                                     f"-DNATIVE_BIONIC={int(bionic)}", "-I",
                                     str(ROOT / "port/gpu/shadps4"), str(src), "-o", str(binary)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            for case in ("success", "read", "join", "many", "stale", "race", "inline", "empty",
                         "disabled", "off", "hot", "precise", "ready", "ready_new_write",
                         "retry", "retry_join", "retry_off", "retry_empty", "obsolete", "recovered",
                         "prepare_accept", "prepare_reject", "prepare_retry", "prepare_exhaust", "partial_valid", "partial_retry"):
                with self.subTest(bionic=bionic, case=case):
                    result = subprocess.run([str(binary), case], capture_output=True,
                                            text=True, timeout=10)
                    self.assertEqual(result.returncode, 0, result.stderr)

    def test_bionic_relaxed_does_not_block_gpu_recording_and_revalidates_copies(self):
        self.run_config(True)

    def test_other_platforms_keep_precise_only_policy(self):
        self.run_config(False)

    def test_guest_cpu_transition_cannot_unprotect_a_newer_gpu_write(self):
        source = (ROOT / "port/gpu/shadps4/video_core/buffer_cache/memory_tracker.h").read_text()
        body = method(source, "void MarkRegionAsCpuModifiedUnlessGpu(VAddr dirty_cpu_addr, u64 query_size)")
        code = r'''
#include <cassert>
#include <cstdint>
#include <mutex>
#include <future>
#include <chrono>
using u64=uint64_t;using VAddr=u64;using namespace std::chrono_literals;
enum class Type {GPU,CPU};
struct RegionManager {
    std::mutex lock;bool gpu=true,cpu=false,writeable=false;
    u64 GetCpuAddr() {return 0;}
    template<Type t> bool IsRegionModified(u64,u64) {
        static_assert(t==Type::GPU);return gpu;
    }
    template<Type t,bool enable> void ChangeRegionState(u64,u64) {
        static_assert(t==Type::CPU && enable);cpu=true;writeable=true;
    }
};
struct MemoryTracker {
    RegionManager region;
    template<bool create,class F> void IteratePages(VAddr addr,u64 size,F&& func) {
        static_assert(!create);func(&region,addr,size);
    }
''' + body + r'''
};
int main() {
    MemoryTracker tracker;
    tracker.MarkRegionAsCpuModifiedUnlessGpu(0,8);
    assert(!tracker.region.cpu && !tracker.region.writeable);
    tracker.region.gpu=false; // completed snapshot has been published
    tracker.MarkRegionAsCpuModifiedUnlessGpu(0,8);
    assert(tracker.region.cpu && tracker.region.writeable);
    tracker.region.cpu=false;tracker.region.writeable=false;
    std::unique_lock owner_lock(tracker.region.lock);
    std::promise<void> started;auto ready=started.get_future();
    auto guest=std::async(std::launch::async,[&] {
        started.set_value();tracker.MarkRegionAsCpuModifiedUnlessGpu(0,8);
    });
    ready.get();assert(guest.wait_for(0s)!=std::future_status::ready);
    tracker.region.gpu=true; // GPU reused the page before the guest could mark it
    owner_lock.unlock();
    assert(guest.wait_for(2s)==std::future_status::ready);guest.get();
    assert(!tracker.region.cpu && !tracker.region.writeable);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            src = Path(directory) / "transition.cpp"
            binary = Path(directory) / "transition"
            src.write_text(code)
            result = subprocess.run([CXX, "-std=c++20", "-O2", "-pthread", str(src), "-o", str(binary)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == "__main__":
    unittest.main()
