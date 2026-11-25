#include <array>
#include <cassert>
#include <vector>
#include <utility>
#include <span>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <memory>
#include "common/interval_set.h"
#include "video_core/buffer_cache/readback_copy_pages.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/buffer_cache/write_tick_tracker.h"
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
using u8=uint8_t;
using u64=uint64_t;using u32=uint32_t;using VAddr=u64;
namespace boost::container {template<class T,size_t N> using small_vector=std::vector<T>;}
namespace Common {
template<class T>T AlignDown(T x,T a){return x/a*a;}
template<class T>T AlignUp(T x,T a){return (x+a-1)/a*a;}
}
enum class GpuReadbacksMode {Relaxed,Precise};
struct Settings {
    GpuReadbacksMode mode=GpuReadbacksMode::Relaxed;bool dma=false;
    auto GetReadbacksMode(){return mode;}
    bool IsDirectMemoryAccessEnabled(){return dma;}
} EmulatorSettings;
namespace TlgToggle {
enum {CoarseReadTracking,AccessMemo};
bool Disabled(int){return false;}
}
namespace VideoCore {
struct RegionManager {
    static inline bool hybrid_enabled=false;
    static bool HybridReadProtectionEnabled() { return hybrid_enabled; }
};
enum class MemoryType {Sparse,DeviceLocal};
struct Buffer {
    MemoryType mem_type=MemoryType::Sparse;VAddr cpu_addr=0x10000000;
    vk::Buffer Handle()const{return vk::Buffer{reinterpret_cast<VkBuffer>(cpu_addr)};}
};
}
namespace Vulkan {
enum class Access {Read,Write};
struct AddressRange {u64 resource,range_start,range_end;};
struct BarrierTracker {
    unsigned inserts=0;std::vector<std::pair<AddressRange,Access>> ranges;
    void InsertRange(const AddressRange& r,Access a){++inserts;ranges.emplace_back(r,a);}
};
struct StagingBufferRef {};
struct Scheduler {
    u64 tick=7;bool host_read_barrier=false;
    vk::PipelineStageFlags2 host_read_src_stages{};vk::AccessFlags2 host_read_src_access{};
    u64 CurrentTick()const{return tick;}
    unsigned ended=0;void EndRendering(){++ended;}
    template<class T>auto RecordData(std::span<const T> values){return std::vector<T>(values.begin(),values.end());}
    template<class F>void Record(F&& f){f(vk::CommandBuffer{reinterpret_cast<VkCommandBuffer>(1)});}
    // PRODUCTION_REQUIRE_BARRIER
};
struct Runtime {
    Scheduler scheduler;
    void (*buffer_write_hook)(void*,VAddr,u64,u64)=nullptr;
    void* buffer_write_context=nullptr;
    bool buffer_write_needs_host_barrier=true;
    BarrierTracker barrier_tracker;
    vk::MemoryBarrier2 memory_barrier;
    struct AccessMemo {u64 resource,start,end;u32 epoch,access;};
    std::array<AccessMemo,512> access_memo{};u32 access_epoch=1;
    struct Query {const VideoCore::Buffer* buffer;u64 offset,size;bool read;};
    std::vector<Query> queries;bool conflict=false;unsigned flushes=0;
    bool IsBufferAccessed(const VideoCore::Buffer* b,u64 o,u64 n,bool read=false){
        queries.push_back({b,o,n,read});return conflict;
    }
    void FlushBarriers(){++flushes;}
    void CopyBuffer(const VideoCore::Buffer*,const VideoCore::Buffer*,std::span<const vk::BufferCopy>);
    void NoteBufferWrite(const VideoCore::Buffer*,u64,u64,vk::PipelineStageFlags2,vk::AccessFlags2);
    void AccessBufferImpl(const VideoCore::Buffer*,u64,u64,vk::PipelineStageFlags2,vk::AccessFlags2,bool);
    // PRODUCTION_HOOK_SETTER
    void AccessBuffer(const VideoCore::Buffer*,u64,u64,vk::PipelineStageFlags2,vk::AccessFlags2);
};
// PRODUCTION_ACCESS_BUFFER
}
namespace VideoCore {
struct BufferCache {
    // PRODUCTION_READBACK_TYPE
    bool mapped_readbacks_enabled = true;
    WriteTickTracker write_ticks;
    std::atomic<u32> deferred_count{};std::mutex deferred_mutex;
    std::vector<std::shared_ptr<DeferredReadback>> deferred_readbacks;
    void NoteDeviceWrite(VAddr,u64,u64);
};
// PRODUCTION_NOTE_WRITE
}
static std::vector<vk::BufferCopy> recorded;
static VkBuffer recorded_src{},recorded_dst{};
static VKAPI_ATTR void VKAPI_CALL Copy(VkCommandBuffer,VkBuffer src,VkBuffer dst,
    uint32_t count,const VkBufferCopy* copies) {
    recorded_src=src;recorded_dst=dst;recorded.clear();
    for(unsigned i=0;i<count;++i)recorded.emplace_back(copies[i].srcOffset,copies[i].dstOffset,copies[i].size);
}
int main() {
    Vulkan::Runtime runtime;
    struct Observed {u64 address,size,tick,calls=0;VideoCore::WriteTickTracker writes;};
    Observed observed;
    runtime.SetBufferWriteHook([](void* data,VAddr a,u64 n,u64 tick){
        auto& o=*static_cast<Observed*>(data);o.address=a;o.size=n;o.tick=tick;++o.calls;
        o.writes.Write(a,n,tick);
    },&observed);
    VideoCore::Buffer arena;
    for(auto access:{vk::AccessFlagBits2::eShaderWrite,vk::AccessFlagBits2::eTransferWrite,
                     vk::AccessFlagBits2::eMemoryWrite,vk::AccessFlagBits2::eTransformFeedbackWriteEXT}) {
        runtime.scheduler.host_read_barrier=false;
        runtime.scheduler.host_read_src_stages={};runtime.scheduler.host_read_src_access={};
        runtime.AccessBuffer(&arena,64,128,vk::PipelineStageFlagBits2::eAllCommands,access);
        assert(runtime.scheduler.host_read_barrier);
        assert(runtime.scheduler.host_read_src_stages==vk::PipelineStageFlagBits2::eAllCommands);
        assert(runtime.scheduler.host_read_src_access==access);
        assert(observed.address==arena.cpu_addr+64 && observed.size==128);
        assert(observed.tick==runtime.scheduler.tick);
        ++runtime.scheduler.tick; // same memoized barrier range must still get a new write tick
    }
    assert(observed.calls==4);
    assert(observed.writes.Latest(arena.cpu_addr,4096)==10);
    runtime.scheduler.host_read_barrier=false;
    runtime.AccessBuffer(&arena,64,128,vk::PipelineStageFlagBits2::eAllGraphics,vk::AccessFlagBits2::eShaderRead);
    assert(!runtime.scheduler.host_read_barrier && observed.calls==4);
    const auto previous_inserts=runtime.barrier_tracker.inserts;
    runtime.AccessBuffer(&arena,8192,16,vk::PipelineStageFlagBits2::eDrawIndirect,
                         vk::AccessFlagBits2::eIndirectCommandRead);
    assert(runtime.barrier_tracker.inserts==previous_inserts+1);
    assert(runtime.barrier_tracker.ranges.back().second==Vulkan::Access::Read);
    assert(!runtime.scheduler.host_read_barrier && observed.calls==4);
    VideoCore::Buffer staging{VideoCore::MemoryType::DeviceLocal,0};
    runtime.AccessBuffer(&staging,64,128,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite);
    assert(!runtime.scheduler.host_read_barrier && observed.calls==4);
    runtime.SetBufferWriteHook(nullptr,nullptr);
    runtime.AccessBuffer(&arena,64,128,vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderWrite);
    assert(!runtime.scheduler.host_read_barrier && observed.calls==4);
    assert(runtime.barrier_tracker.inserts>0); // ordinary device barrier tracking remains active
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdCopyBuffer=Copy;
    // Contiguous batches need only one notification, including reverse order.
    for(bool reverse:{false,true}) {
        Vulkan::Runtime r;unsigned calls=0;
        r.SetBufferWriteHook([](void* p,VAddr a,u64 n,u64 tick){
            ++*static_cast<unsigned*>(p);assert(a==0x10000000 && n==6*4096 && tick==7);
        },&calls);
        std::vector<vk::BufferCopy> copies;
        for(unsigned i=0;i<6;++i)copies.emplace_back(i*4096,i*4096,4096);
        if(reverse)std::reverse(copies.begin(),copies.end());
        r.CopyBuffer(&staging,&arena,copies);assert(calls==1);
        assert(r.scheduler.host_read_src_stages==vk::PipelineStageFlagBits2::eCopy);
        assert(r.scheduler.host_read_src_access==vk::AccessFlagBits2::eTransferWrite);
    }
    // Hybrid staging: a newer actual Vulkan write must invalidate the old
    // snapshot while leaving a disjoint snapshot intact. Avoid HOST_READ
    // barriers on staging-only copies; they already wait on the transfer fence.
    {
        VideoCore::RegionManager::hybrid_enabled=true;
        Vulkan::Runtime r;
        VideoCore::BufferCache cache;
        auto staged=std::make_shared<VideoCore::BufferCache::DeferredReadback>();
        staged->mapped=false;staged->arena_base=arena.cpu_addr;
        staged->copies.emplace_back(4096,0,4096);staged->tick=5;
        auto unrelated=std::make_shared<VideoCore::BufferCache::DeferredReadback>();
        unrelated->mapped=false;unrelated->arena_base=arena.cpu_addr;
        unrelated->copies.emplace_back(16384,0,4096);unrelated->tick=5;
        cache.mapped_readbacks_enabled=false;
        cache.deferred_readbacks={staged,unrelated};cache.deferred_count=2;
        cache.write_ticks.Write(arena.cpu_addr,65536,5);
        r.SetBufferWriteHook([](void* c,VAddr a,u64 n,u64 tick){
            static_cast<VideoCore::BufferCache*>(c)->NoteDeviceWrite(a,n,tick);
        },&cache,false);
        r.scheduler.tick=6;
        r.AccessBuffer(&arena,4096+128,32,vk::PipelineStageFlagBits2::eComputeShader,
                       vk::AccessFlagBits2::eShaderWrite);
        assert(staged->stale && !unrelated->stale);
        assert(!r.scheduler.host_read_barrier);
        assert(cache.write_ticks.Latest(arena.cpu_addr+4096,4096)==5);
        assert(cache.write_ticks.Latest(arena.cpu_addr+16384,4096)==5);
        VideoCore::RegionManager::hybrid_enabled=false;
    }
    // Exercise the actual recorded CopyBuffer, AccessBuffer and cache notification.
    for(unsigned count:{1u,4u,5u,9u})for(bool precise:{false,true}) {
        Vulkan::Runtime r;VideoCore::BufferCache cache;
        EmulatorSettings.mode=precise?GpuReadbacksMode::Precise:GpuReadbacksMode::Relaxed;
        auto hole=std::make_shared<VideoCore::BufferCache::DeferredReadback>();
        hole->mapped=true;hole->page_scoped=true;hole->arena_base=arena.cpu_addr;
        hole->copies.emplace_back(4096,0,4096);hole->tick=3;
        auto written=std::make_shared<VideoCore::BufferCache::DeferredReadback>();
        written->mapped=true;written->page_scoped=true;written->arena_base=arena.cpu_addr;
        written->copies.emplace_back(8192,0,4096);written->tick=3;
        cache.deferred_readbacks={hole,written};cache.deferred_count=2;
        cache.write_ticks.Write(arena.cpu_addr,1<<20,3);
        r.SetBufferWriteHook([](void* c,VAddr a,u64 n,u64 tick){
            static_cast<VideoCore::BufferCache*>(c)->NoteDeviceWrite(a,n,tick);
        },&cache);
        std::vector<vk::BufferCopy> copies;
        // Reversed destination order, sparse holes, one cross-page unaligned copy.
        for(unsigned i=0;i<count;++i)copies.emplace_back(i*128,(count-i)*8192,32);
        copies.back().dstOffset=8192-8;copies.back().size=32;
        for(u64 tick:{7u,8u}) {
            r.scheduler.tick=tick;r.queries.clear();r.scheduler.host_read_barrier=false;
            r.CopyBuffer(&staging,&arena,copies);
            assert(recorded.size()==copies.size());
            assert(recorded_src==VkBuffer(staging.Handle()) && recorded_dst==VkBuffer(arena.Handle()));
            for(unsigned i=0;i<count;++i) {
                assert(recorded[i].srcOffset==copies[i].srcOffset);
                assert(recorded[i].dstOffset==copies[i].dstOffset && recorded[i].size==copies[i].size);
                assert(cache.write_ticks.Latest(arena.cpu_addr+copies[i].dstOffset,copies[i].size)==tick);
            }
            // The cross-page write touches the final 8 bytes of the pending hole:
            // conservatively invalidate that whole page, but never other gap pages.
            assert(cache.write_ticks.Latest(arena.cpu_addr,4096)==3);
            assert(cache.write_ticks.Latest(arena.cpu_addr+4096,4096)==tick);
            if(count>1)assert(cache.write_ticks.Latest(arena.cpu_addr+12288,4096)==3);
            assert(r.scheduler.host_read_barrier);
            if(precise)assert(written->stale && hole->stale);
            else assert(!written->invalid_pages.Empty() && !hole->invalid_pages.Empty());
            if(count>4) {
                assert(r.queries.size()==2);
                assert(r.queries[1].offset==8192-8);
                assert(r.queries[1].size==count*8192+32-(8192-8));
            }else assert(r.queries.size()==2*count);
        }
        // A pending snapshot strictly inside an untouched gap survives the next batch.
        auto gap=std::make_shared<VideoCore::BufferCache::DeferredReadback>();
        gap->mapped=true;gap->page_scoped=true;gap->arena_base=arena.cpu_addr;
        gap->copies.emplace_back(12288,0,4096);gap->tick=3;
        cache.deferred_readbacks.push_back(gap);cache.deferred_count=3;
        r.scheduler.tick=9;r.CopyBuffer(&staging,&arena,copies);
        assert(!gap->stale && gap->invalid_pages.Empty());
        assert(cache.write_ticks.Latest(arena.cpu_addr+12288,4096)==3);
        const auto& bound=r.barrier_tracker.ranges.back();
        if(count>4) {
            assert(bound.second==Vulkan::Access::Write);
            assert(bound.first.range_start==8192-8);
            assert(bound.first.range_end==count*8192+31);
        }
        // Ordinary device destinations still retain transfer-write barriers, no HOST_READ.
        r.scheduler.host_read_barrier=false;r.CopyBuffer(&arena,&staging,copies);
        assert(!r.scheduler.host_read_barrier);
        r.SetBufferWriteHook(nullptr,nullptr);r.CopyBuffer(&staging,&arena,copies);
        assert(!r.scheduler.host_read_barrier);
        assert(bool(r.memory_barrier.srcAccessMask & vk::AccessFlagBits2::eTransferWrite));
        r.conflict=true;r.CopyBuffer(&staging,&arena,copies);assert(r.flushes==1);
    }
}
