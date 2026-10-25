#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstring>
#include <future>
#include <span>
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/parallel_record_queue.h"
#include "video_core/renderer_vulkan/host_copy_sources.h"
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
using u32=uint32_t;using u64=uint64_t;
using namespace std::chrono_literals;
#define ASSERT(x) assert(x)
namespace TlgToggle {
enum {RecordPrefetch,ThreadedRecording,OrderedGuestWrites};
bool Disabled(int){return false;}
}
namespace IssBionicPerf {
enum class Stage {HostCopies,RecorderSync};
struct Scope {Scope(Stage){}};
bool Enabled(){return true;}
std::atomic<u64> parallel_splits{},parallel_batches{},parallel_deferred_host{};
}
namespace TlgStats {
u64 host_copies_wait_ns{},copy_threads_wait_ns{},sync_recording_ns{};
std::atomic<u64> host_copy_waits{};
struct WaitTimer {WaitTimer(u64&){}};
}
namespace TlgCopy {
void FlushBatch(){}
template<class F>void AfterCopies(F f){f();}
void WaitAsync(){}
}
static std::mutex driver_mutex;
static std::vector<unsigned> ended;
static std::array<bool,3> closed{};
static unsigned host_barrier_buffer=0;
static VKAPI_ATTR void VKAPI_CALL HostBarrier(VkCommandBuffer buffer,const VkDependencyInfo* info) {
    host_barrier_buffer=unsigned(reinterpret_cast<uintptr_t>(buffer));
    assert(!closed[host_barrier_buffer] && info->memoryBarrierCount==1);
    const auto& b=info->pMemoryBarriers[0];
    assert(b.srcStageMask==VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT);
    assert(b.srcAccessMask==VK_ACCESS_2_SHADER_WRITE_BIT);
    assert(b.dstStageMask==VK_PIPELINE_STAGE_2_HOST_BIT && b.dstAccessMask==VK_ACCESS_2_HOST_READ_BIT);
}
static VKAPI_ATTR VkResult VKAPI_CALL End(VkCommandBuffer buffer) {
    std::scoped_lock lock{driver_mutex};
    const auto id=unsigned(reinterpret_cast<uintptr_t>(buffer));
    assert(id && id<3 && !closed[id]);closed[id]=true;ended.push_back(id);return VK_SUCCESS;
}
void Check(vk::Result r){assert(r==vk::Result::eSuccess);}
namespace Vulkan {
// PRODUCTION_CHUNK
struct RenderState {unsigned pass;};
struct Scheduler {
    using Queue=ParallelRecordQueue<RecordChunk,vk::CommandBuffer>;
    std::unique_ptr<Queue> parallel_recording;
    unsigned parallel_lane=0;size_t parallel_bytes=0,parallel_target=64*1024;
    std::array<vk::CommandBuffer,2> submission_buffers{};
    bool host_read_barrier=false;
    vk::PipelineStageFlags2 host_read_src_stages{};vk::AccessFlags2 host_read_src_access{};
    void FlushHostReadBarrier();
    bool recording_threaded=true,direct_mode=false,is_rendering=false;
    bool native_secondary_enabled=false;
    struct NativeScope { bool active=false; vk::CommandBuffer buffer{}; };
    std::array<NativeScope,2> native_secondary_scopes{};
    RenderState render_state{1};
    vk::CommandBuffer current_cmdbuf{reinterpret_cast<VkCommandBuffer>(1)};
    struct State {unsigned invalidations=0;void Invalidate(){++invalidations;}}dynamic_state,vertex_input_state;
    std::unique_ptr<RecordChunk> record_chunk;
    std::vector<std::unique_ptr<RecordChunk>> full_chunks,free_chunks;
    std::deque<std::unique_ptr<RecordChunk>> recorder_queue;
    std::mutex recorder_mutex;
    std::condition_variable_any recorder_cv,recorder_idle_cv;
    bool recorder_sleeping=false,recorder_busy=false;
    std::atomic<size_t> queued_chunks{};
    std::atomic<u64> direct_recordings{},host_copies_done{},deferred_signals_issued{};
    u64 host_copies_issued=0;
    HostCopySources host_copy_sources;
    std::shared_ptr<std::atomic<u64>> deferred_signals_done=std::make_shared<std::atomic<u64>>(0);
    std::array<std::atomic<unsigned>,2> begun{};
    Scheduler() {
        parallel_recording=std::make_unique<Queue>([this](unsigned lane) {
            assert(lane==1);++begun[lane];
            return vk::CommandBuffer{reinterpret_cast<VkCommandBuffer>(lane+1)};
        },[](unsigned){});
        BeginParallelBatch();record_chunk=AcquireChunk();
    }
    ~Scheduler(){SyncRecording();parallel_recording.reset();}
    bool IsRecordingDeferred()const{return recording_threaded&&!direct_mode;}
    bool WillBeginRendering(const RenderState& s)const{return !is_rendering||s.pass!=render_state.pass;}
    bool owner_splits=false;
    void CheckOwner(void*){if(owner_splits){owner_splits=false;SplitRecording();}}
    static void TraceDirectRecording(void*){}
    static bool HostCopyRangesEnabled(){return true;}
    void EndRendering(){if(is_rendering){is_rendering=false;Record([](vk::CommandBuffer){});}}
    std::unique_ptr<RecordChunk> AcquireChunk();
    void HandOverParallel();void SplitRecording();void BeginParallelBatch();
    std::span<const vk::CommandBuffer> EndRecordingBuffers();
    void SplitRecordingAtBoundary(const RenderState&);
    void KickRecording(bool force=false);void SyncRecording();
    void SignalAfterHostCopies(std::function<void()>);
    void WaitHostCopies();void WaitDeferredSignals();
    // PRODUCTION_INLINE
};
// PRODUCTION_METHODS
}

int main() {
    VULKAN_HPP_DEFAULT_DISPATCHER.vkEndCommandBuffer=End;
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier2=HostBarrier;
    Vulkan::Scheduler s;
    std::promise<void> first_started,release_first,second_started,after_copy_started;
    auto first=first_started.get_future();
    auto release=release_first.get_future().share();
    auto second=second_started.get_future();
    auto after_copy=after_copy_started.get_future();
    std::array<std::vector<unsigned>,2> recorded;
    s.Record([&](vk::CommandBuffer b){
        assert(VkCommandBuffer(b)==reinterpret_cast<VkCommandBuffer>(1));
        first_started.set_value();release.wait();recorded[0].push_back(1);
    });
    std::array<u64,64> data{};data[0]=77;
    for(unsigned i=0;i<150;++i) {
        auto snapshot=s.RecordData(std::span<const u64>{data});
        s.Record([&,snapshot,i](vk::CommandBuffer){assert(snapshot[0]==77);recorded[0].push_back(2+i);});
    }
    s.is_rendering=true;s.render_state={1};
    s.SplitRecordingAtBoundary({1});assert(s.parallel_lane==0); // never cut a continuing pass
    s.SplitRecordingAtBoundary({2});assert(s.parallel_lane==1 && !s.is_rendering);
    assert(s.dynamic_state.invalidations==1 && s.vertex_input_state.invalidations==1);
    data[0]=99;
    s.Record([&](vk::CommandBuffer b){
        assert(VkCommandBuffer(b)==reinterpret_cast<VkCommandBuffer>(2));
        recorded[1].push_back(500);second_started.set_value();
    });
    int source=7,snapshot=0;std::atomic<unsigned> phase{};
    s.RecordHostCopy([&]{assert(recorded[0].size()==151);snapshot=source;phase=1;});
    s.SignalAfterHostCopies([&]{assert(snapshot==7 && phase==1);phase=2;});
    s.Record([&](vk::CommandBuffer){after_copy_started.set_value();});
    s.KickRecording(true);
    assert(first.wait_for(2s)==std::future_status::ready);
    assert(second.wait_for(2s)==std::future_status::ready); // actual overlap, lane 0 still blocked
    // A pending copy AND signal must not block later Vulkan commands. The old
    // WaitPrefix implementation stalls here until the first lane is released.
    assert(after_copy.wait_for(2s)==std::future_status::ready);
    assert(phase==0 && s.host_copies_done==0 && s.deferred_signals_done->load()==0);
    release_first.set_value();s.WaitHostCopies();source=9;s.WaitDeferredSignals();s.SyncRecording();
    assert(s.parallel_recording->DeferredHost()==2);
    assert(snapshot==7 && phase==2 && recorded[0].size()==151 && recorded[1].size()==1);
    auto raw=s.CommandBuffer();assert(raw==s.current_cmdbuf && VkCommandBuffer(raw)==reinterpret_cast<VkCommandBuffer>(2));
    s.RequireHostReadBarrier(vk::PipelineStageFlagBits2::eComputeShader,vk::AccessFlagBits2::eShaderWrite);
    s.FlushHostReadBarrier();assert(host_barrier_buffer==2);
    const auto buffers=s.EndRecordingBuffers();assert(buffers.size()==2);
    assert(VkCommandBuffer(buffers[0])==reinterpret_cast<VkCommandBuffer>(1));
    assert(VkCommandBuffer(buffers[1])==reinterpret_cast<VkCommandBuffer>(2));
    assert((ended==std::vector<unsigned>{1,2}) && s.begun[1]==1);

    // Lane 0 drains a deferred host copy while lane 1 keeps recording. Copies
    // arriving during that drain must queue behind it, not execute concurrently;
    // Sync must also include the ordered tasks after both Vulkan queues empty.
    closed={};s.current_cmdbuf=vk::CommandBuffer{reinterpret_cast<VkCommandBuffer>(1)};
    s.BeginParallelBatch();s.direct_mode=false;
    std::promise<void> prefix_entered,release_prefix,copy_entered,release_copy,vulkan_after;
    auto prefix_in=prefix_entered.get_future();auto prefix_out=release_prefix.get_future().share();
    auto copy_in=copy_entered.get_future();auto copy_out=release_copy.get_future().share();
    auto advanced=vulkan_after.get_future();std::atomic<unsigned> ordered_phase{};
    s.Record([&](vk::CommandBuffer){prefix_entered.set_value();prefix_out.wait();});
    s.parallel_target=1;s.SplitRecordingAtBoundary({2});
    s.RecordHostCopy([&]{copy_entered.set_value();copy_out.wait();ordered_phase=1;});
    s.Record([&](vk::CommandBuffer){vulkan_after.set_value();});s.KickRecording(true);
    assert(prefix_in.wait_for(2s)==std::future_status::ready);
    assert(advanced.wait_for(2s)==std::future_status::ready && ordered_phase==0);
    release_prefix.set_value();assert(copy_in.wait_for(2s)==std::future_status::ready);
    s.RecordHostCopy([&]{assert(ordered_phase==1);ordered_phase=2;});
    s.SignalAfterHostCopies([&]{assert(ordered_phase==2);ordered_phase=3;});
    std::promise<void> tail_entered;auto tail=tail_entered.get_future();
    s.Record([&](vk::CommandBuffer){tail_entered.set_value();});s.KickRecording(true);
    assert(tail.wait_for(2s)==std::future_status::ready && ordered_phase==0);
    auto syncing=std::async(std::launch::async,[&]{s.SyncRecording();});
    assert(syncing.wait_for(20ms)==std::future_status::timeout);
    release_copy.set_value();assert(syncing.wait_for(2s)==std::future_status::ready);syncing.get();
    s.WaitDeferredSignals();assert(ordered_phase==3);s.EndRecordingBuffers();

    // Recycle the SAME handles: no completion, buffer state or lane can leak from a prior batch.
    for(unsigned iteration=0;iteration<100;++iteration) {
        closed={};ended.clear();
        s.current_cmdbuf=vk::CommandBuffer{reinterpret_cast<VkCommandBuffer>(1)};
        s.BeginParallelBatch();s.direct_mode=false;assert(s.parallel_lane==0);
        std::vector<unsigned> host_order;
        for(unsigned i=0;i<10;++i)s.RecordHostCopy([&,i]{host_order.push_back(i);});
        s.parallel_target=1;s.SplitRecordingAtBoundary({2});assert(s.parallel_lane==1);
        for(unsigned i=10;i<20;++i)s.RecordHostCopy([&,i]{host_order.push_back(i);});
        s.SignalAfterHostCopies([&]{assert(host_order.size()==20);});
        s.SyncRecording();s.WaitDeferredSignals();
        for(unsigned i=0;i<20;++i)assert(host_order[i]==i);
        assert(s.EndRecordingBuffers().size()==2);
    }
    assert(s.begun[1]==102);
    // A cut whose second half first becomes raw must begin exactly once on the producer.
    closed={};s.current_cmdbuf=vk::CommandBuffer{reinterpret_cast<VkCommandBuffer>(1)};
    s.BeginParallelBatch();s.direct_mode=false;
    s.Record([](vk::CommandBuffer){});s.parallel_target=1;s.SplitRecordingAtBoundary({2});
    (void)s.CommandBuffer();assert(s.begun[1]==103);
    s.Record([](vk::CommandBuffer){});s.SyncRecording();assert(s.begun[1]==103);
    s.EndRecordingBuffers();
    // A short batch keeps the original single-command-buffer submission.
    closed={};s.current_cmdbuf=vk::CommandBuffer{reinterpret_cast<VkCommandBuffer>(1)};
    s.BeginParallelBatch();s.direct_mode=false;
    s.Record([](vk::CommandBuffer){});s.SplitRecordingAtBoundary({2});
    assert(s.parallel_lane==0);s.SyncRecording();assert(s.EndRecordingBuffers().size()==1);
    // An owner hook may drain draws and cut before a copy is captured. The copy
    // must capture the NEW lane, so it still waits for the older prefix.
    closed={};s.current_cmdbuf=vk::CommandBuffer{reinterpret_cast<VkCommandBuffer>(1)};
    s.BeginParallelBatch();s.direct_mode=false;
    std::promise<void> hold_started,allow_hold;auto holding=hold_started.get_future();
    auto allowed=allow_hold.get_future().share();std::atomic<bool> copied=false;
    s.Record([&](vk::CommandBuffer){hold_started.set_value();allowed.wait();});
    s.owner_splits=true;
    s.RecordHostCopy([&]{copied=true;});s.KickRecording(true);
    assert(holding.wait_for(2s)==std::future_status::ready);
    assert(s.parallel_lane==1 && !copied);
    allow_hold.set_value();s.SyncRecording();assert(copied);
    s.EndRecordingBuffers();
    // Leave destructor an open batch, just like the real scheduler.
    closed={};s.current_cmdbuf=vk::CommandBuffer{reinterpret_cast<VkCommandBuffer>(1)};
    s.BeginParallelBatch();
}
