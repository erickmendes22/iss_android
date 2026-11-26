// Production CPU merge and upload, with a serialized owner and byte-accurate GPU model.
#include "iss_session.h"
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
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
using u8=uint8_t;using u64=uint64_t;using VAddr=u64;
using namespace std::chrono_literals;
// PRODUCTION_TEST_QUEUE
namespace boost::container {template<class T,size_t N>using small_vector=std::vector<T>;}
namespace vk {struct BufferCopy {u64 srcOffset,dstOffset,size;};}
namespace Common {template<class T,class U>T AlignUp(T x,U a){return (x+a-1)/a*a;}}
enum class GpuReadbacksMode {Disabled,Relaxed,Precise};
struct {bool dma=false;bool IsDirectMemoryAccessEnabled(){return dma;}
    GpuReadbacksMode mode=GpuReadbacksMode::Relaxed;
    auto GetReadbacksMode(){return mode;}}EmulatorSettings;
struct Stats{std::atomic<u64>cpu_merge_applied{},cpu_merge_bytes{},cpu_merge_rejected{};};
Stats& Readbacks(){static Stats s;return s;}bool ReadbackStatsEnabled(){return true;}
void ReportReadbacks(){}
namespace VideoCore {
enum class MemoryType {HostUncached};enum class ReadbackSource {CpuWrite};
struct Buffer {u64 cpu_addr=0x1000,size_bytes=4096;std::array<u8,4096>gpu;};
struct Memory {
    Queue* queue;std::array<u8,4096> bytes;bool fail=false,block=false;
    std::mutex gate;std::condition_variable cv;bool stored=false,released=false;
    std::function<void()>before_store;
    bool TryWriteBacking(void* dst,const void* data,u64 size){
        const auto offset=reinterpret_cast<VAddr>(dst)-0x1000;
        assert(offset+size<=bytes.size());std::memcpy(bytes.data()+offset,data,size);return true;
    }
    bool TryCpuWriteBacking(void* dst,const void* data,u64 size){
        queue->CheckOwner();if(fail)return false;
        if(before_store)before_store();TryWriteBacking(dst,data,size);
        if(block){std::unique_lock lock(gate);stored=true;cv.notify_all();
            assert(cv.wait_for(lock,3s,[&]{return released;}));}
        return true;
    }
    void CopySparseMemory(VAddr src,u8* dst,u64 size){
        queue->CheckOwner();assert(src>=0x1000 && src+size<=0x2000);
        std::memcpy(dst,bytes.data()+src-0x1000,size);
    }
};
struct Texture {
    Queue* queue;Memory* memory;bool image_pending=false,dirty=false;
    unsigned resolves=0;
    void ResolveReadbacks(VAddr,u64,bool owner,ReadbackSource){
        queue->CheckOwner();assert(owner);++resolves;
        if(image_pending){memory->bytes.fill(99);image_pending=false;}
    }
    void InvalidateMemory(VAddr,u64){queue->CheckOwner();assert(!image_pending);dirty=true;}
};
struct Tracker {bool gpu=true;bool IsRegionGpuModified(VAddr,u64){return gpu;}};
struct Pool {
    Buffer buffer;std::vector<u8>data;unsigned allocations=0;
    struct Ref {u64 offset;u8* mapped;Buffer* buffer;void Flush() const{}};
    Ref Request(u64 size,MemoryType){++allocations;data.resize(size+128);return{128,data.data()+128,&buffer};}
};
struct Runtime {
    Pool* pool;unsigned copies=0;
    template<class Copies>void CopyBuffer(const Buffer* src,const Buffer* dst,const Copies& ranges){
        assert(src==&pool->buffer);++copies;
        for(auto c:ranges){assert(c.dstOffset+c.size<=dst->size_bytes);
            std::memcpy(const_cast<Buffer*>(dst)->gpu.data()+c.dstOffset,pool->data.data()+c.srcOffset,c.size);}
    }
};
struct Scheduler {
    Queue* queue;Memory* memory;unsigned copies_waited=0,signals_waited=0;bool pending_copy=false;
    std::array<u8,128> uploaded{};
    void WaitHostCopiesFor(VAddr a,u64 size){queue->CheckOwner();++copies_waited;
        if(pending_copy){assert(a==0x1040 && size==128);std::memcpy(uploaded.data(),memory->bytes.data()+64,128);pending_copy=false;}}
    void WaitDeferredSignals(){queue->CheckOwner();++signals_waited;}
};
struct BufferCache {
    Queue* liverpool;Memory* memory;Tracker tracker;Tracker* memory_tracker=&tracker;
    Texture texture_cache;Scheduler scheduler;Pool staging_pool;Runtime runtime{&staging_pool};
    std::mutex merge_mutex;std::map<VAddr,VAddr>merged_writes;std::atomic<u64>merged_count{};
    bool stale=false;unsigned eager_marks=0;
    BufferCache(Queue* q,Memory* m):liverpool(q),memory(m),texture_cache{q,m},scheduler{q,m}{}
    void MarkEagerStale(VAddr,u64){if(EmulatorSettings.mode==GpuReadbacksMode::Relaxed)liverpool->CheckOwner();++eager_marks;}
    void MarkDeferredStale(VAddr,u64,bool cpu){assert(cpu);if(EmulatorSettings.mode==GpuReadbacksMode::Relaxed)liverpool->CheckOwner();stale=true;}
    void NoteGpuWriteForEager(VAddr,u64){liverpool->CheckOwner();}
    bool CpuWriteMerge(VAddr,const void*,int,u64);
    bool CpuWriteMergeRelaxed(VAddr,const void*,int,u64);
    void AddMergedWriteRange(VAddr,u64);
    void FlushMergedWrites(const Buffer*,VAddr,u64);
};
// PRODUCTION_MERGE_METHODS
}
int main(int argc,char** argv){
    assert(argc==2);const std::string mode=argv[1];setenv("TLG_WRITE_MERGE","1",1);
    if(mode=="off")setenv("TLG_RELAXED_WRITE_MERGE","0",1);
    if(mode=="globaloff")setenv("TLG_WRITE_MERGE","0",1);
    if(mode=="precise")EmulatorSettings.mode=GpuReadbacksMode::Precise;
    Queue q;VideoCore::Memory memory{&q};memory.bytes.fill(7);
    VideoCore::BufferCache c{&q,&memory};VideoCore::Buffer gpu;gpu.gpu.fill(42);
    std::array<u8,128>source;source.fill(77);
    if (mode=="short") {
        assert(!c.CpuWriteMerge(0x1040,source.data(),0,15));
        for (unsigned len=16; len<64; ++len) {
            gpu.gpu.fill(42);
            assert(c.CpuWriteMerge(0x1040,source.data(),0,len));
            q.SendCommand<true,true>([&]{c.FlushMergedWrites(&gpu,0x1000,4096);});
            for(unsigned i=0;i<4096;++i)
                assert(gpu.gpu[i]==(i>=64 && i<64+len ? 77 : 42));
        }
        memory.fail=true;
        assert(!c.CpuWriteMerge(0x1040,source.data(),0,16));
        assert(IssSession::small_merged==48 && IssSession::small_fallback==1);
        return 0;
    }
    if(mode=="cpuonly")c.tracker.gpu=false;
    if(mode=="dma")EmulatorSettings.dma=true;
    if(mode=="failure")memory.fail=true;
    if(mode=="image")c.texture_cache.image_pending=true;
    if(mode=="hostcopy")c.scheduler.pending_copy=true;
    if(mode=="snapshot")memory.before_store=[&]{source.fill(222);};
    if(mode=="large" || mode=="overlap" || mode=="overflow"){
        const auto dst=mode=="overflow"?~u64{31}:0x1040;
        assert(!c.CpuWriteMerge(dst,mode=="overlap"?reinterpret_cast<void*>(dst):source.data(),0,
            mode=="large"?(4ull<<20)+1:128));
        assert(q.submitted==0);return 0;
    }
    if(mode=="many"){
        std::vector<std::future<bool>> jobs;
        for(unsigned i=0;i<16;++i)jobs.push_back(std::async(std::launch::async,[&,i]{return c.CpuWriteMerge(0x1000+i*64,nullptr,80+i,64);}));
        for(auto& j:jobs)assert(j.get());
        q.SendCommand<true,true>([&]{c.FlushMergedWrites(&gpu,0x1000,4096);});
        for(unsigned i=0;i<4096;++i)assert(gpu.gpu[i]==(i<1024?80+i/64:42));
        assert(c.merged_count==0);return 0;
    }
    if(mode=="atomic"){
        memory.block=true;
        auto writer=std::async(std::launch::async,[&]{return c.CpuWriteMerge(0x1040,source.data(),0,128);});
        {std::unique_lock lock(memory.gate);assert(memory.cv.wait_for(lock,3s,[&]{return memory.stored;}));}
        assert(c.merged_count==0);
        auto reader=std::async(std::launch::async,[&]{q.SendCommand<true,true>([&]{
            assert(c.stale && c.merged_count==1);c.FlushMergedWrites(&gpu,0x1000,4096);});});
        assert(reader.wait_for(20ms)!=std::future_status::ready);
        {std::lock_guard lock(memory.gate);memory.released=true;memory.cv.notify_all();}
        assert(writer.get());reader.get();
    }else{
        const bool result=c.CpuWriteMerge(0x1040,mode=="fill"?nullptr:source.data(),55,128);
        const bool expected=mode!="off" && mode!="globaloff" && mode!="cpuonly" && mode!="failure" && mode!="nonbionic" && mode!="dma";
        assert(result==expected);
        if(!expected){for(auto x:memory.bytes)assert(x==7);assert(c.merged_count==0);return 0;}
        assert(c.merged_count==1 && c.tracker.gpu); // untouched GPU bytes are not marked CPU dirty
        if(mode!="precise")assert(c.texture_cache.dirty && c.texture_cache.resolves==1 &&
            c.scheduler.copies_waited==1 && c.scheduler.signals_waited==1);
        if(mode=="hostcopy")for(auto x:c.scheduler.uploaded)assert(x==7);
        if(mode=="split"){
            q.SendCommand<true,true>([&]{c.FlushMergedWrites(&gpu,0x1060,64);});
            assert(c.merged_count==2);
            for(unsigned i=0;i<4096;++i)assert(gpu.gpu[i]==(i>=96&&i<160?77:42));
        }
        q.SendCommand<true,true>([&]{c.FlushMergedWrites(&gpu,0x1000,4096);});
    }
    for(unsigned i=0;i<4096;++i)assert(gpu.gpu[i]==(i>=64&&i<192?(mode=="fill"?55:77):42));
    assert(c.merged_count==0);
    if(mode=="image")for(unsigned i=0;i<4096;++i)assert(memory.bytes[i]==(i>=64&&i<192?77:99));
}
