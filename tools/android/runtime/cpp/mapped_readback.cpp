// Real buffer-cache readback methods and real Vulkan-Hpp, with delayed device completion.
#include <array>
#include <atomic>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <set>
#include <tuple>
#include <utility>
#include "video_core/renderer_vulkan/vk_common.h"
#include "common/interval_set.h"
#include "video_core/buffer_cache/readback_copy_pages.h"
#include "video_core/buffer_cache/write_tick_tracker.h"
#include "video_core/buffer_cache/hybrid_staging_window.h"
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
using u8=uint8_t; using u32=uint32_t; using u64=uint64_t; using VAddr=u64;
namespace boost::container {template<class T,size_t N> using small_vector=std::vector<T>;}
namespace Common {
template<class T> T AlignDown(T x,T a){return x/a*a;}
template<class T> T AlignUp(T x,T a){return (x+a-1)/a*a;}
}
enum class GpuReadbacksMode {Disabled,Relaxed,Precise};
struct Settings {
    GpuReadbacksMode mode=GpuReadbacksMode::Relaxed; bool dma=false;
    auto GetReadbacksMode(){return mode;}
    bool IsDirectMemoryAccessEnabled(){return dma;}
} EmulatorSettings;
struct Stats {
    std::atomic<u64> mapped_prefetch_narrowed{},mapped_wider_wait_bypassed{},mapped_coherent_skipped{},
        mapped_current{},mapped_unavailable{},mapped_scheduled{},mapped_applied{},
        mapped_bytes{},mapped_partial{},mapped_partial_kept{},mapped_partial_retry{},mapped_stale{},mapped_inline{},mapped_submitted{},joined{},deferred{},deferred_bytes{};
};
Stats& Readbacks(){static Stats s;return s;}
bool ReadbackStatsEnabled(){return true;}
static std::map<VkDeviceMemory,std::vector<u8>> device_memory;
static bool fail_map=false,fail_invalidate=false;
static unsigned map_calls=0,invalidations=0,barriers=0;
static VkDeviceMemory forbidden_memory{};static u64 forbidden_start=0,forbidden_end=0;
static std::vector<std::string> allocation_events;
static VKAPI_ATTR VkResult VKAPI_CALL Allocate(VkDevice,const VkMemoryAllocateInfo* info,
    const VkAllocationCallbacks*,VkDeviceMemory* out) {
    assert(info->memoryTypeIndex==2 && info->allocationSize==(256ull<<20));
    *out=reinterpret_cast<VkDeviceMemory>(3);allocation_events.push_back("allocate");return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL Unmap(VkDevice,VkDeviceMemory memory) {
    assert(memory==reinterpret_cast<VkDeviceMemory>(3));allocation_events.push_back("unmap");
}
static VKAPI_ATTR void VKAPI_CALL Free(VkDevice,VkDeviceMemory memory,const VkAllocationCallbacks*) {
    assert(memory==reinterpret_cast<VkDeviceMemory>(3));allocation_events.push_back("free");
}
#define LOG_WARNING(...) ((void)0)
static VKAPI_ATTR VkResult VKAPI_CALL Map(VkDevice,VkDeviceMemory memory,VkDeviceSize offset,
    VkDeviceSize size,VkMemoryMapFlags,void** out) {
    ++map_calls;assert(offset==0 && size==VK_WHOLE_SIZE);
    if(fail_map && memory==reinterpret_cast<VkDeviceMemory>(2))return VK_ERROR_MEMORY_MAP_FAILED;
    *out=device_memory.at(memory).data();return VK_SUCCESS;
}
static VKAPI_ATTR VkResult VKAPI_CALL Invalidate(VkDevice,uint32_t count,const VkMappedMemoryRange* ranges) {
    ++invalidations;assert(count==1);
    const auto& r=ranges[0];assert(r.offset%256==0);
    assert(r.size%256==0 || r.offset+r.size==device_memory.at(r.memory).size());
    assert(r.offset+r.size<=device_memory.at(r.memory).size());
    if(r.memory==forbidden_memory)assert(r.offset>=forbidden_end||r.offset+r.size<=forbidden_start);
    return fail_invalidate?VK_ERROR_MEMORY_MAP_FAILED:VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL Barrier(VkCommandBuffer,const VkDependencyInfo* dep) {
    ++barriers;assert(dep->memoryBarrierCount==1);
    const auto& b=dep->pMemoryBarriers[0];
    assert(b.srcStageMask==VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    assert(b.srcAccessMask==VK_ACCESS_2_MEMORY_WRITE_BIT);
    assert(b.dstStageMask==VK_PIPELINE_STAGE_2_HOST_BIT);
    assert(b.dstAccessMask==VK_ACCESS_2_HOST_READ_BIT);
}
namespace Vulkan {
template<class T>auto Check(T result){assert(result.result==vk::Result::eSuccess);return result.value;}
struct Instance {
    vk::Device GetDevice()const{return vk::Device{reinterpret_cast<VkDevice>(1)};}
    u64 NonCoherentAtomSize()const{return 256;}
};
struct Semaphore {
    u64 completed=5,waited=0;
    bool IsFree(u64 tick)const{return completed>=tick;}
    void Wait(u64 tick){assert(tick<=10);waited=tick;completed=std::max(completed,tick);}
};
struct Scheduler {
    Semaphore sem;u64 current=10;unsigned flushes=0;
    bool host_read_barrier=false,complete_on_flush=false;
    vk::PipelineStageFlags2 host_read_src_stages{};vk::AccessFlags2 host_read_src_access{};
    vk::CommandBuffer current_cmdbuf{reinterpret_cast<VkCommandBuffer>(1)};
    u64 CurrentTick()const{return current;}
    bool IsFree(u64 tick)const{return sem.IsFree(tick);}
    auto* GetWorkSemaphore(){return &sem;}
    void Flush(){++flushes;FlushHostReadBarrier();if(complete_on_flush)sem.completed=current; ++current;}
    void FlushHostReadBarrier();
    // PRODUCTION_REQUIRE_BARRIER
};
struct StagingBufferRef {
    std::vector<u8>* storage=nullptr;u8* mapped=nullptr;u64 offset=128;void* buffer=nullptr;
    void Invalidate()const{}
};
// PRODUCTION_SCHEDULER
}
namespace VideoCore {
struct RegionManager {
    static inline bool hybrid_enabled=false;
    static bool HybridReadProtectionEnabled() { return hybrid_enabled; }
};
constexpr u64 RESIDENCY_CHUNK_SIZE=256ull<<20;
enum class MemoryType {Sparse,HostCached};
struct Buffer {VAddr cpu_addr=0x10000000;u64 size_bytes=0x80000;};
struct Ranges {
    std::vector<std::pair<VAddr,VAddr>> intervals;
    void Normalize(){
        std::sort(intervals.begin(),intervals.end());
        std::vector<std::pair<VAddr,VAddr>> merged;
        for(auto [a,b]:intervals){
            if(!merged.empty() && merged.back().second>=a)merged.back().second=std::max(merged.back().second,b);
            else merged.emplace_back(a,b);
        }
        intervals=std::move(merged);
    }
    void Add(VAddr start,u64 size){intervals.emplace_back(start,start+size);Normalize();}
    void Subtract(VAddr start,u64 size) {
        auto old=std::exchange(intervals,{});
        for(auto [a,b]:old) {
            if(b<=start || start+size<=a)intervals.emplace_back(a,b);
            else {if(a<start)intervals.emplace_back(a,start);if(start+size<b)intervals.emplace_back(start+size,b);}
        }
        Normalize();
    }
    template<class F>void ForEachInRange(VAddr a,u64 size,F&& f)const {
        for(auto [lo,hi]:intervals)if(lo<a+size && a<hi)f(std::max(a,lo),std::min(a+size,hi));
    }
};
struct Tracker {
    std::vector<std::pair<VAddr,u64>> ranges;bool gpu=true;unsigned publications=0;
    std::set<VAddr> coherent_pages;
    bool IsRegionGpuModified(VAddr a,u64 n){
        for(auto p=a&~u64{4095};p<a+n;p+=4096)if(!coherent_pages.contains(p))return gpu;
        return false;
    }
    template<bool Clear,class F> void ForEachDownloadRange(VAddr query,u64 bytes,F&& f){
        for(auto [a,n]:ranges){const u64 lo=std::max(a,query),hi=std::min(a+n,query+bytes);if(lo<hi)f(lo,hi-lo);}
    }
    template<class P,class W>bool ApplyGpuCopyIf(VAddr a,u64 n,P&& p,W&& w){
        if(!p())return false;
        if(gpu){
            for(auto page=a&~u64{4095};page<a+n;page+=4096){
                if(coherent_pages.contains(page))continue;
                w(std::max(a,page),std::min(a+n,page+4096));coherent_pages.insert(page);
            }
            ++publications;
        }return true;
    }
};
struct Memory {
    std::map<VAddr,u8> published;
    bool TryWriteBacking(void* dest,const void* source,u64 size){
        auto a=reinterpret_cast<uintptr_t>(dest);const auto* b=static_cast<const u8*>(source);
        for(u64 i=0;i<size;++i)published[a+i]=b[i];return true;
    }
};
struct Pool {
    std::vector<u8> data;unsigned allocations=0,freed=0;
    Vulkan::StagingBufferRef Request(u64 bytes,MemoryType,int,bool){
        ++allocations;data.resize(bytes+128);std::fill(data.begin(),data.end(),77);
        return {&data,data.data()+128,128,reinterpret_cast<void*>(1)};
    }
    void FreeDeferred(const Vulkan::StagingBufferRef& s){assert(s.buffer);++freed;}
};
struct Runtime {
    unsigned copies=0;
    template<class Copies>void CopyBuffer(const Buffer*,void*,const Copies&){++copies;}
};
struct BufferCache {
    // PRODUCTION_READBACK_TYPE
    // PRODUCTION_MAPPING_TYPE
    // PRODUCTION_BACKING_TYPE
    // PRODUCTION_ATTEMPT_TYPE
    Vulkan::Instance instance;Vulkan::Scheduler scheduler;Runtime runtime;Pool staging_pool;
    Tracker tracker;Tracker* memory_tracker=&tracker;Memory mem;Memory* memory=&mem;
    Buffer arena;Ranges gpu_modified_ranges;IntervalList<Backing> resident_ranges;
    WriteTickTracker write_ticks;
    std::map<vk::DeviceMemory,ResidencyMapping> residency_mappings;
    bool mapped_readbacks_enabled=true,residency_coherent=true;
    u32 block_shift=14;
    u32 arena_memory_type_index=2;
    vk::DeviceMemory residency_chunk{};u64 residency_chunk_used=0,residency_allocations=0;
    std::optional<std::pair<vk::DeviceMemory,u64>> AllocateResidency(u64,ResidencyAttempt&);
    void UndoResidency(const ResidencyAttempt&);
    HybridStagingWindow hybrid_staging_hot_pages;
    std::mutex deferred_mutex,eager_mutex;
    std::vector<std::shared_ptr<DeferredReadback>> deferred_readbacks;
    std::atomic<u32> deferred_count{},eager_count{};
    static bool DeferredReadbackEnabled(){return true;}
    static bool MappedReadbackPolicy();
    auto GetReadbackWindow(VAddr,u64){return std::tuple{&arena,arena.cpu_addr,arena.cpu_addr+0x80000};}
    void ApplyEagerSubtractsLocked(){}
    void FlushMergedWrites(const Buffer*,VAddr,u64){}
    void NoteDeviceWrite(VAddr,u64,u64);
    void MarkDeferredStale(VAddr,u64,bool);
    bool PrepareMappedReadback(DeferredReadback&,u64,bool=false,u64=0);
    bool CaptureMappedReadback(DeferredReadback&);
    u64 CollectDownloads(const Buffer*,VAddr,u64,boost::container::small_vector<vk::BufferCopy,8>&);
    std::shared_ptr<DeferredReadback> StartDeferredReadback(VAddr,u64);
    bool FinishDeferredReadback(DeferredReadback&,bool);
    void RecoverDeferredReadback(DeferredReadback&);
    void SettleDeferredReadbacks(VAddr,VAddr,u64=0);
    void PruneDeferredReadbacks();
};
// PRODUCTION_BUFFER_METHODS
} // namespace VideoCore

int main(int argc,char** argv) {
    const std::string mode=argc>1?argv[1]:"coherent";
    const bool targeted=mode=="hybrid_targeted_pages";
    if (targeted) setenv("TLG_ISS_HYBRID_TARGETED_STAGE","1",1);
    const bool hybrid_stage=mode=="hybrid_stage_write" || mode=="hybrid_stage_gap";
    VideoCore::RegionManager::hybrid_enabled=hybrid_stage || targeted;
    const bool partial=mode.starts_with("partial_");
    if(!partial)setenv("TLG_MAPPED_READBACK_PARTIAL","0",1);
    if(mode=="off")setenv("TLG_MAPPED_READBACK","0",1);
    if(mode=="currentoff")setenv("TLG_MAPPED_READBACK_CURRENT","0",1);
    if(mode=="dma")EmulatorSettings.dma=true;
    if(mode=="precise")EmulatorSettings.mode=GpuReadbacksMode::Precise;
    const bool policy=VideoCore::BufferCache::MappedReadbackPolicy();
    if(mode=="off" || mode=="dma" || mode=="precise" || mode=="nonbionic"){
        assert(!policy);return 0;
    }
    assert(policy);
    VULKAN_HPP_DEFAULT_DISPATCHER.vkMapMemory=Map;
    VULKAN_HPP_DEFAULT_DISPATCHER.vkInvalidateMappedMemoryRanges=Invalidate;
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier2=Barrier;
    VULKAN_HPP_DEFAULT_DISPATCHER.vkAllocateMemory=Allocate;
    VULKAN_HPP_DEFAULT_DISPATCHER.vkUnmapMemory=Unmap;
    VULKAN_HPP_DEFAULT_DISPATCHER.vkFreeMemory=Free;
    VideoCore::BufferCache c;const VAddr base=c.arena.cpu_addr;
    {
        VideoCore::BufferCache::ResidencyAttempt attempt{};
        auto memory=c.AllocateResidency(16384,attempt);assert(memory && memory->second==0);
        auto& mapping=c.residency_mappings.at(memory->first);
        assert(mapping.size==(256ull<<20) && !mapping.data && !mapping.failed);
        mapping.data=reinterpret_cast<const u8*>(1); // cleanup must unmap before free/erase
        c.UndoResidency(attempt);
        assert(c.residency_mappings.empty() && c.residency_allocations==0 && !c.residency_chunk);
        assert((allocation_events==std::vector<std::string>{"allocate","unmap","free"}));
    }
    c.scheduler.FlushHostReadBarrier();assert(barriers==0);
    c.scheduler.RequireHostReadBarrier();c.scheduler.RequireHostReadBarrier();
    c.scheduler.FlushHostReadBarrier();c.scheduler.FlushHostReadBarrier();assert(barriers==1);
    const auto m1=reinterpret_cast<VkDeviceMemory>(1),m2=reinterpret_cast<VkDeviceMemory>(2);
    device_memory[m1].resize(65536);device_memory[m2].resize(65536);
    for(size_t i=0;i<65536;++i){device_memory[m1][i]=u8(i*3+17);device_memory[m2][i]=u8(i*7+31);}
    c.residency_mappings[vk::DeviceMemory{m1}]={.size=65536};
    c.residency_mappings[vk::DeviceMemory{m2}]={.size=65536};
    const u64 block=base>>14;
    c.resident_ranges.Add({{block,block+1},vk::DeviceMemory{m1},2});
    if(mode!="hole")c.resident_ranges.Add({{block+1,block+2},vk::DeviceMemory{m2},1});
    c.tracker.ranges={{base+0x3ff0,32},{base+0x4100,16}};
    if(mode=="partial_gap")c.tracker.ranges={{base+0x3000,3*4096}};
    for(auto [a,n]:c.tracker.ranges){c.gpu_modified_ranges.Add(a,n);c.write_ticks.Write(a,n,7);}
    if(mode.starts_with("priority_")) {
        c.resident_ranges.Add({{block+2,block+3},vk::DeviceMemory{m2},2});
        const u64 neighbor_tick=mode=="priority_same_tick"?7:mode=="priority_current"?10:9;
        c.tracker.ranges.emplace_back(base+0x9000,16);
        c.gpu_modified_ranges.Add(base+0x9000,16);c.write_ticks.Write(base+0x9000,16,neighbor_tick);
        if(mode=="priority_ready")c.scheduler.sem.completed=7;
        if(mode=="priority_noncoherent") {
            c.residency_coherent=false;
            c.residency_mappings[vk::DeviceMemory{m2}].data=reinterpret_cast<const u8*>(1);
            forbidden_memory=m2;forbidden_start=0;forbidden_end=65536;
        }
        if(mode=="priority_neighbor_ready")c.scheduler.sem.completed=9;
        if(mode=="priority_coherent") {
            c.tracker.coherent_pages.insert(base+0x3000);
            assert(!c.StartDeferredReadback(base+0x3ff0,4));
            assert(Readbacks().mapped_coherent_skipped==1 && c.scheduler.sem.waited==0);
            assert(c.mem.published.empty() && c.runtime.copies==0 && c.scheduler.flushes==0);
            assert(c.gpu_modified_ranges.intervals.size()==3);return 0;
        }
        std::shared_ptr<VideoCore::BufferCache::DeferredReadback> old;
        if(mode=="priority_join" || mode=="priority_join_ready") {
            // Another guest requested the newer neighbor, collecting the whole window.
            old=c.StartDeferredReadback(base+0x9000,4);
            assert(old && old->mapped && old->tick==9 && !old->applied);
            assert(old->start==base && old->end==base+65536);
            if(mode=="priority_join_ready")c.scheduler.sem.completed=9;
        }
        auto r=c.StartDeferredReadback(base+0x3ff0,4);assert(r && r->mapped);
        const bool wide=mode=="priority_same_tick" || mode=="priority_neighbor_ready" || mode=="priority_join_ready";
        assert(r->start==base+(wide?0:0x3000) && r->end==base+(wide?65536:0x4000));
        assert(r->tick==(mode=="priority_neighbor_ready" || mode=="priority_join_ready"?9:7));
        assert(c.scheduler.sem.waited==0); // creation must never wait on the cache owner
        if(old) {
            if(mode=="priority_join") {
                assert(old->recovered && !old->applied && r!=old);
                assert(Readbacks().mapped_wider_wait_bypassed==1);
                assert(!c.FinishDeferredReadback(*old,false));
            }else assert(r==old && !old->recovered);
        }
        if(mode=="priority_ready" || mode=="priority_neighbor_ready")assert(r->applied);
        assert(c.FinishDeferredReadback(*r,!r->applied));
        assert(c.mem.published.size()==(wide?64:16));
        assert(!c.tracker.IsRegionGpuModified(base+0x3ff0,4));
        if(!wide) {
            assert(c.tracker.IsRegionGpuModified(base+0x4100,16));
            assert(c.tracker.IsRegionGpuModified(base+0x9000,16));
            assert(!c.mem.published.contains(base+0x4100));
            assert(!c.mem.published.contains(base+0x9000));
            assert((c.gpu_modified_ranges.intervals==std::vector<std::pair<VAddr,VAddr>>{
                {base+0x4000,base+0x4010},{base+0x4100,base+0x4110},{base+0x9000,base+0x9010}}));
        }
        for(u64 i=0;i<16;++i)assert(c.mem.published.at(base+0x3ff0+i)==device_memory[m1][32768+0x3ff0+i]);
        assert(c.runtime.copies==0 && c.scheduler.flushes==0);
        assert(Readbacks().mapped_prefetch_narrowed==(!wide && neighbor_tick==9?1u:0u));
        assert(c.scheduler.sem.waited==(mode=="priority_ready" || mode=="priority_neighbor_ready" || mode=="priority_join_ready"?0u:7u));
        if(mode=="priority_noncoherent")assert(invalidations==1);
        if(mode=="priority_join") {
            // The original guest still needs tick 9. Its replacement must publish
            // that neighbor after completion without replaying the CPU's new byte.
            c.mem.published[base+0x3ff0]=201;c.scheduler.sem.completed=9;
            auto neighbor=c.StartDeferredReadback(base+0x9000,4);
            assert(neighbor && neighbor->applied && neighbor->tick==9 && neighbor!=old);
            assert(c.mem.published.at(base+0x3ff0)==201);
            for(u64 i=0;i<16;++i)assert(c.mem.published.at(base+0x9000+i)==device_memory[m2][36864+i]);
            assert(!c.tracker.IsRegionGpuModified(base+0x9000,4));
            assert(c.gpu_modified_ranges.intervals.empty() && c.scheduler.sem.waited==7);
        }
        return 0;
    }
    if(mode=="outsidecurrent") {
        c.tracker.ranges.emplace_back(base+0x20000,16);
        c.gpu_modified_ranges.Add(base+0x20000,16);
        c.write_ticks.Write(base+0x20000,16,10);
    }
    if(mode=="inside64current" || mode=="onepage") {
        c.tracker.ranges.emplace_back(base+0x9000,16);
        c.gpu_modified_ranges.Add(base+0x9000,16);
        c.write_ticks.Write(base+0x9000,16,10);
    }
    const bool current=mode=="current" || mode=="currentoff" || mode=="currentready" ||
        mode=="currentstale" || mode=="currentmapfail" || mode=="currentnoncoherent";
    if(current)c.write_ticks.Write(base+0x3ff0,32,10);
    if(current)c.scheduler.RequireHostReadBarrier(); // real recorded writes require visibility
    if(mode=="unknown")c.write_ticks=VideoCore::WriteTickTracker{};
    if(mode=="mapfail" || mode=="currentmapfail")fail_map=true;
    if(mode=="noncoherent" || mode=="currentnoncoherent" || mode=="invalidatefail" || mode=="readyinvalidatefail")c.residency_coherent=false;
    if(mode=="invalidatefail" || mode=="readyinvalidatefail")fail_invalidate=true;
    if(mode=="ready" || mode=="readyinvalidatefail")c.scheduler.sem.completed=7;
    if(hybrid_stage || targeted)c.mapped_readbacks_enabled=false;
    if(mode=="currentready")c.scheduler.complete_on_flush=true;
    const u64 required_size=(mode=="inside64current" || mode=="hole" || mode=="mapfail")?32:4;
    auto r=c.StartDeferredReadback(base+0x3ff0,required_size);assert(r);
    const bool mapped=!hybrid_stage && !targeted && mode!="currentoff" && mode!="currentmapfail" && mode!="unknown" && mode!="hole" && mode!="mapfail";
    assert(r->mapped==mapped);
    if(mode=="outsidecurrent") {
        assert(r->start==base && r->end==base+65536);
        assert(c.gpu_modified_ranges.intervals.size()==1);
        assert(c.gpu_modified_ranges.intervals[0].first==base+0x20000);
    }
    if(mode=="inside64current") {
        assert(r->start==base+0x3000 && r->end==base+0x5000);
        assert(c.gpu_modified_ranges.intervals.size()==1);
        assert(c.gpu_modified_ranges.intervals[0].first==base+0x9000);
    }
    if(mode=="onepage")assert(r->start==base+0x3000 && r->end==base+0x4000);
    assert(c.runtime.copies==(mapped?0u:1u));
    assert(c.scheduler.flushes==(mapped && !current?0u:1u));
    if(current && mapped)assert(c.scheduler.CurrentTick()==11 && Readbacks().mapped_submitted==1);
    if(current)assert(barriers==2 && !c.scheduler.host_read_barrier);
    assert(c.staging_pool.allocations==(mapped?0u:1u));
    if (targeted) {
        assert(!r->mapped && !r->applied && r->copies.size()==1);
        assert(r->copies[0].srcOffset==0x3ff0 && r->copies[0].size==16);
        auto other=c.StartDeferredReadback(base+0x4100,4);
        assert(other && other!=r && !other->mapped);
        assert(c.deferred_count==2);
        assert(!other->copies.empty());
        assert(c.tracker.IsRegionGpuModified(base+0x4100,4));
        return 0;
    }
    if (hybrid_stage) {
        // Real Start/NoteDeviceWrite/Finish methods: no test-only stale logic.
        assert(!r->mapped && !r->applied && c.deferred_count==1);
        assert(r->tick==10);
        if (mode=="hybrid_stage_write") {
            c.NoteDeviceWrite(base+0x3ff0,4,r->tick+1);
            assert(r->stale);
            assert(!c.FinishDeferredReadback(*r,true));
            assert(c.mem.published.empty());
            c.RecoverDeferredReadback(*r);
            assert(r->recovered && !c.gpu_modified_ranges.intervals.empty());
        } else {
            // A write on a clean page inside the same 512-KiB window
            // must not invalidate a copy of unrelated GPU-dirty islands.
            c.NoteDeviceWrite(base+0x9000,4,r->tick+1);
            assert(!r->stale);
            assert(c.FinishDeferredReadback(*r,true));
            assert(c.mem.published.size()==48);
        }
        c.PruneDeferredReadbacks();
        assert(c.deferred_readbacks.empty());
        assert(c.staging_pool.freed==1);
        return 0;
    }
    if(mode=="ready" || mode=="currentready") {
        // Actual Start -> Finish -> Capture publishes both sparse allocations on the
        // first owner request, with neither a wait nor an outstanding deferred object.
        assert(r->applied && c.deferred_readbacks.empty() && c.deferred_count==0);
        assert(c.scheduler.sem.waited==0 && Readbacks().mapped_inline==1);
        for(u64 j=0;j<32;++j)assert(c.mem.published.at(base+0x3ff0+j)==
            (j<16?device_memory[m1][32768+0x3ff0+j]:device_memory[m2][16384+j-16]));
        for(u64 j=0;j<16;++j)assert(c.mem.published.at(base+0x4100+j)==device_memory[m2][16384+0x100+j]);
        assert(c.mem.published.size()==48);
        const u64 next=current?11:10;
        c.NoteDeviceWrite(base+0x4100,16,next);
        assert(c.write_ticks.Latest(base+0x4100,16)==next && !r->stale);
        return 0;
    }
    // A failed immediate capture must remain recoverable through the old path.
    if(mode=="readyinvalidatefail")assert(!r->applied && c.deferred_count==1);
    auto joined=c.StartDeferredReadback(base+(mode=="onepage"?0x3ff4:0x4100),4);assert(joined==r);
    if(mapped){assert(r->tick==(current?10u:7u));assert(c.mem.published.empty());assert(!c.CaptureMappedReadback(*r));}
    if(partial) {
        assert(r->page_scoped);
        if(mode=="partial_clean"||mode=="partial_disjoint") {
            if(mode=="partial_disjoint")c.NoteDeviceWrite(base+0x9000,16,10);
            assert(c.FinishDeferredReadback(*r,true)&&r->applied&&!r->partial&&!r->stale);
            assert(c.mem.published.size()==48&&Readbacks().mapped_partial==0);
            assert(!c.tracker.IsRegionGpuModified(base+0x3ff0,32));return 0;
        }
        if(mode=="partial_gap") {
            c.residency_coherent=false;
            c.NoteDeviceWrite(base+0x4000,4096,10);c.gpu_modified_ranges.Add(base+0x4000,4096);
            forbidden_memory=m2;forbidden_start=16384;forbidden_end=16384+4096;
            assert(c.FinishDeferredReadback(*r,true)&&r->partial);
            assert(c.mem.published.size()==8192&&invalidations==2);
            assert(!c.tracker.IsRegionGpuModified(base+0x3000,4096));
            assert(c.tracker.IsRegionGpuModified(base+0x4000,4096));
            assert(!c.tracker.IsRegionGpuModified(base+0x5000,4096));
            assert(Readbacks().mapped_partial_kept==8192&&Readbacks().mapped_partial_retry==4096);
            for(u64 j=0;j<4096;++j){
                assert(c.mem.published.at(base+0x3000+j)==device_memory[m1][32768+0x3000+j]);
                assert(c.mem.published.at(base+0x5000+j)==device_memory[m2][16384+4096+j]);
            }
            assert(c.gpu_modified_ranges.intervals.size()==1);
            assert((c.gpu_modified_ranges.intervals[0]==std::pair{base+0x4000,base+0x5000}));
            c.mem.published[base+0x3000]=201;forbidden_memory={};
            auto fresh=c.StartDeferredReadback(base+0x4000,4);assert(fresh&&fresh!=r);
            assert(c.FinishDeferredReadback(*fresh,true));
            assert(c.mem.published.size()==12288&&c.mem.published.at(base+0x3000)==201);
            for(u64 j=0;j<4096;++j)assert(c.mem.published.at(base+0x4000+j)==device_memory[m2][16384+j]);
            return 0;
        }
        const bool whole=mode=="partial_all",cpu=mode=="partial_cpu";
        if(mode=="partial_noncoherent")c.residency_coherent=false;
        if(mode=="partial_recheck")c.write_ticks.Write(base+0x4200,4,10);
        else if(mode=="partial_note")c.NoteDeviceWrite(base+0x4200,4,10);
        else c.MarkDeferredStale(whole?base:base+0x4200,whole?65536:4,cpu);
        if(cpu){assert(r->stale);assert(!c.FinishDeferredReadback(*r,true));
            c.RecoverDeferredReadback(*r);assert(c.mem.published.empty());return 0;}
        assert(!r->stale);
        c.gpu_modified_ranges.Add(base+0x4200,4);c.tracker.ranges.emplace_back(base+0x4200,4);
        if(mode=="partial_new_request"||mode=="partial_new_request_ready") {
            c.NoteDeviceWrite(base+0x4200,4,10);
            const bool ready=mode=="partial_new_request_ready";
            if(ready)c.scheduler.sem.completed=7;
            auto fresh=c.StartDeferredReadback(base+0x4100,4);
            assert(fresh&&fresh!=r&&fresh->tick==10&&c.scheduler.flushes==1);
            assert(c.scheduler.sem.waited==(ready?7u:0u));
            assert(r->applied==ready&&r->recovered==!ready);
            assert(c.mem.published.size()==(ready?16u:0u));
            if(ready)c.mem.published[base+0x3ff0]=201;
            assert(c.FinishDeferredReadback(*fresh,true));
            assert(c.mem.published.size()==52&&c.scheduler.sem.waited==10);
            if(ready)assert(c.mem.published.at(base+0x3ff0)==201);
            return 0;
        }
        if(mode=="partial_inflight") {
            c.SettleDeferredReadbacks(base,base+65536);
            assert(r->recovered&&!r->applied&&c.scheduler.sem.waited==0);
            assert(c.mem.published.empty()&&!c.gpu_modified_ranges.intervals.empty());return 0;
        }
        // The invalid page's mapping must never be dereferenced at this older tick.
        // Poison its spans: reading even one rejected byte fails this regression.
        for(auto& span:r->mapped_spans)if(span.memory==vk::DeviceMemory{m2})
            span.source=reinterpret_cast<const u8*>(1);
        if(whole)for(auto& span:r->mapped_spans)span.source=reinterpret_cast<const u8*>(1);
        const bool ok=c.FinishDeferredReadback(*r,true);
        if(whole){assert(!ok&&r->stale&&c.mem.published.empty());
            c.RecoverDeferredReadback(*r);return 0;}
        assert(ok&&r->applied&&r->partial&&!r->recovered);
        assert(c.mem.published.size()==16&&Readbacks().mapped_partial==1);
        assert(Readbacks().mapped_partial_kept==16&&Readbacks().mapped_partial_retry==32);
        assert(!c.tracker.IsRegionGpuModified(base+0x3ff0,4));
        assert(c.tracker.IsRegionGpuModified(base+0x4100,4));
        for(u64 j=0;j<16;++j)assert(c.mem.published.at(base+0x3ff0+j)==device_memory[m1][32768+0x3ff0+j]);
        u64 dirty=0;c.gpu_modified_ranges.ForEachInRange(base,0x80000,[&](VAddr a,VAddr b){
            assert(a>=base+0x4000);dirty+=b-a;});assert(dirty==36);
        if(mode=="partial_noncoherent")assert(invalidations==1);
        // A CPU write to the already coherent page survives the newer page's retry.
        c.mem.published[base+0x3ff0]=201;
        c.write_ticks.Write(base+0x4100,16,10);
        auto fresh=c.StartDeferredReadback(base+0x4100,4);assert(fresh&&fresh!=r);
        assert(c.FinishDeferredReadback(*fresh,true));
        assert(c.mem.published.at(base+0x3ff0)==201&&c.mem.published.size()==52);
        for(u64 j=0;j<4;++j)assert(c.mem.published.at(base+0x4200+j)==device_memory[m2][16384+0x200+j]);
        assert(c.scheduler.sem.waited==10&&!c.tracker.IsRegionGpuModified(base+0x4100,4));
        for(u64 j=0;j<16;++j)assert(c.mem.published.at(base+0x4100+j)==device_memory[m2][16384+0x100+j]);
        c.PruneDeferredReadbacks();assert(c.deferred_readbacks.empty());return 0;
    }
    if(mode=="replacement" || mode=="replacement_stage" || mode=="replacement_unrelated") {
        // Exercise the actual prune/recover/recollect path used by ReadMemory's
        // retry. The old pending copy removed two dirty islands; none may be lost.
        c.NoteDeviceWrite(base+0x4200,4,10);assert(r->stale);
        c.tracker.ranges.emplace_back(base+0x4200,4);
        c.gpu_modified_ranges.Add(base+0x4200,4);
        if(mode=="replacement_stage")c.write_ticks=VideoCore::WriteTickTracker{};
        const bool unrelated=mode=="replacement_unrelated";
        auto fresh=c.StartDeferredReadback(base+(unrelated?0x3ff0:0x4100),4);
        assert(fresh && fresh!=r && r->recovered && !r->applied);
        if(unrelated) {
            // An overwrite on the prefetched neighbor page invalidated the old
            // envelope, but retrying the requested page still needs only tick 7.
            assert(fresh->mapped && fresh->tick==7 && c.scheduler.flushes==0);
            assert(c.FinishDeferredReadback(*fresh,true) && c.mem.published.size()==16);
            u64 remaining=0;c.gpu_modified_ranges.ForEachInRange(base,0x80000,
                [&](VAddr a,VAddr b){remaining+=b-a;});
            assert(remaining==36 && c.runtime.copies==0);
            return 0;
        }
        assert(fresh->mapped==(mode=="replacement") && fresh->tick==10);
        assert(c.deferred_count==1 && c.scheduler.flushes==1 && c.scheduler.sem.waited==0);
        assert(c.mem.published.empty());
        assert(c.FinishDeferredReadback(*fresh,true));
        assert(c.scheduler.sem.waited==10 && fresh->applied && c.mem.published.size()==52);
        for(u64 j=0;j<32;++j)assert(c.mem.published.at(base+0x3ff0+j)==
            (fresh->mapped?(j<16?device_memory[m1][32768+0x3ff0+j]:device_memory[m2][16384+j-16]):77));
        for(u64 j=0;j<16;++j)assert(c.mem.published.at(base+0x4100+j)==
            (fresh->mapped?device_memory[m2][16384+0x100+j]:77));
        for(u64 j=0;j<4;++j)assert(c.mem.published.at(base+0x4200+j)==
            (fresh->mapped?device_memory[m2][16384+0x200+j]:77));
        c.PruneDeferredReadbacks();assert(c.deferred_readbacks.empty());
        assert(c.runtime.copies==(fresh->mapped?0u:1u));return 0;
    }
    if(mode=="disjoint") {
        c.NoteDeviceWrite(base+0x9000,16,10);
        c.NoteDeviceWrite(base+0x9001,14,10); // memo hit cannot stale unrelated copied pages
        assert(!r->stale);
    }
    if(mode=="stale")c.NoteDeviceWrite(base+0x4200,4,10); // same copied 4K page
    if(mode=="currentstale")c.NoteDeviceWrite(base+0x4200,4,11);
    if(mode=="recheck")c.write_ticks.Write(base+0x4100,4,10); // revalidation catches missing stale mark
    if(mode=="disjoint")c.NoteDeviceWrite(base+0x9000,16,10); // same prefetch window, different page
    const bool success=c.FinishDeferredReadback(*r,true);
    if(mode=="stale" || mode=="currentstale" || mode=="recheck" || mode=="invalidatefail" || mode=="readyinvalidatefail"){
        assert(!success && c.mem.published.empty());
        c.RecoverDeferredReadback(*r);assert(r->recovered && !c.gpu_modified_ranges.intervals.empty());
    } else {
        assert(success);
        for(u64 j=0;j<(mode=="onepage"?16u:32u);++j){
            const u8 expected=!mapped?77:(j<16?device_memory[m1][32768+0x3ff0+j]:device_memory[m2][16384+j-16]);
            assert(c.mem.published.at(base+0x3ff0+j)==expected);
        }
        if(mode=="onepage") {
            assert(c.mem.published.size()==16);
            u64 remaining=0;c.gpu_modified_ranges.ForEachInRange(base,0x80000,
                [&](VAddr a,VAddr b){remaining+=b-a;});
            assert(remaining==48); // neighbor page and unrelated new write remain GPU dirty
        } else {
            for(u64 j=0;j<16;++j)assert(c.mem.published.at(base+0x4100+j)==(!mapped?77:device_memory[m2][16384+0x100+j]));
            assert(c.mem.published.size()==48); // untouched bytes/pages are never published
        }
        assert(r->applied);
    }
    assert(c.scheduler.sem.waited==(mapped && !current?7u:10u));
    if(mode=="noncoherent" || mode=="currentnoncoherent")assert(invalidations==3);
    if(mapped && c.residency_coherent)assert(invalidations==0);
    c.PruneDeferredReadbacks();assert(c.deferred_readbacks.empty());
    assert(c.staging_pool.freed==(mapped?0u:1u));
}
