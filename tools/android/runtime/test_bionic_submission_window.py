"""Compile real scheduler methods with a deterministic Vulkan timeline.

The mock driver can finish batches or lag behind. Reuse and callback execution
must remain gated by confirmed completion, with no per-draw driver polling.
"""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from test_bionic_resource_reuse import method

ROOT = Path(__file__).resolve().parents[3]
CXX = shutil.which("c++")
VK = ROOT / "port/gpu/shadps4/video_core/renderer_vulkan"

PREAMBLE = r'''
#include <array>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <functional>
#include <limits>
#include <mutex>
#include <queue>
#include <source_location>
#include <span>
#include <string>
#include <vector>
#include "iss_bionic_perf.h"
#include "vk_wait_diagnostics.h"
using u64=uint64_t;
using u32=uint32_t;
#define ASSERT_MSG(condition, ...) assert(condition)
namespace vk {
enum class Result { eSuccess, eErrorDeviceLost };
using Semaphore=int;
using Fence=int;
enum class PipelineStageFlagBits { eAllCommands, eColorAttachmentOutput };
using PipelineStageFlags=PipelineStageFlagBits;
struct SemaphoreWaitInfo { unsigned semaphoreCount; const Semaphore* pSemaphores; const u64* pValues; };
struct TimelineSemaphoreSubmitInfo {
    unsigned waitSemaphoreValueCount; const u64* pWaitSemaphoreValues;
    unsigned signalSemaphoreValueCount; const u64* pSignalSemaphoreValues;
};
struct CommandBuffer { Result end() {return Result::eSuccess;} };
struct SubmitInfo {
    const TimelineSemaphoreSubmitInfo* pNext;
    unsigned waitSemaphoreCount; const Semaphore* pWaitSemaphores;
    const PipelineStageFlags* pWaitDstStageMask;
    unsigned commandBufferCount; const CommandBuffer* pCommandBuffers;
    unsigned signalSemaphoreCount; const Semaphore* pSignalSemaphores;
};
}
struct Driver {
    u64 submitted=0, completed=0, queries=0, waits=0, max_backlog=0;
    bool finish_batch=true;unsigned buffers_last=0;
    std::pair<vk::Result,u64> getSemaphoreCounterValue(vk::Semaphore) {
        ++queries;
        return {vk::Result::eSuccess,completed};
    }
    vk::Result waitSemaphores(const vk::SemaphoreWaitInfo* info,u64) {
        ++waits;
        assert(*info->pValues<=submitted); // never wait for an unsignaled tick
        completed=std::max(completed,*info->pValues);
        return vk::Result::eSuccess;
    }
    vk::Result submit(const vk::SubmitInfo& info,vk::Fence) {
        assert(info.signalSemaphoreCount==1);
        assert(info.commandBufferCount==1 || info.commandBufferCount==2);
        buffers_last=info.commandBufferCount;
        assert(*info.pNext->pSignalSemaphoreValues==submitted+1);
        submitted=*info.pNext->pSignalSemaphoreValues;
        max_backlog=std::max(max_backlog,submitted-completed);
        if(finish_batch) completed=submitted;
        return vk::Result::eSuccess;
    }
};
struct Instance {
    Driver* driver;
    Driver& GetDevice() const {return *driver;}
    Driver& GetGraphicsQueue() const {return *driver;}
};
namespace TlgToggle { enum {PendingPollLimit=1}; bool Disabled(int) {return false;} }
namespace ImGui::Core::TextureManager { void Submit() {} }
namespace Vulkan {
constexpr u64 WAIT_TIMEOUT=std::numeric_limits<u64>::max();
struct MockHandle { int value=1; const int& get() const {return value;} int operator*() const {return value;} };
struct Semaphore {
    const Instance& instance;
    MockHandle semaphore;
    std::atomic<u64> gpu_tick{0}, current_tick{1};
    explicit Semaphore(const Instance& i):instance{i} {}
    u64 CurrentTick() const {return current_tick.load();}
    u64 NextTick() {return current_tick.fetch_add(1);}
    u64 KnownGpuTick() const {return gpu_tick.load();}
    bool IsFree(u64 tick) const {return KnownGpuTick()>=tick;}
    int Handle() const {return semaphore.get();}
    void Refresh();
    void Wait(u64,std::source_location caller=std::source_location::current());
};
struct SubmitInfo {
    std::array<int,4> wait_semas{},signal_semas{};
    std::array<u64,4> wait_ticks{},signal_ticks{};
    int fence=0;
    unsigned num_wait_semas=0,num_signal_semas=0;
    void AddSignal(int sem,u64 tick) {signal_semas[num_signal_semas]=sem;signal_ticks[num_signal_semas++]=tick;}
};
struct GpuProfiler {
    static GpuProfiler* Get() {return nullptr;}
    bool Records(void*) {return false;}
    template<class F> void Mark(u64,F) {}
};
struct Scheduler {
    const Instance& instance;
    Semaphore work_semaphore;
    std::recursive_mutex submit_mutex,pending_ops_mutex;
    std::atomic<unsigned> num_pending_ops{0},work_since_submit{0};
    unsigned pending_polls=0, allocations=0;
    bool popping_pending_ops=false;
    struct Op {std::function<void()> callback;u64 gpu_tick;};
    std::queue<Op> pending_ops;
    std::function<void(SubmitInfo&)> on_submit;
    vk::CommandBuffer current_cmdbuf;
    explicit Scheduler(const Instance& i):instance{i},work_semaphore{i} {}
    void EndRendering() {}
    void SyncRecording() {}
    void WaitHostCopies() {}
    void FlushHostReadBarrier() {}
    bool parallel=false;std::array<vk::CommandBuffer,2> batch_buffers;
    std::span<const vk::CommandBuffer> EndRecordingBuffers() {
        current_cmdbuf.end();return {batch_buffers.data(),parallel ? 2u : 1u};
    }
    void AllocateWorkerCommandBuffers() {++allocations;}
    void SubmitExecution(SubmitInfo&);
    void PopPendingOperations();
    u64 CurrentTick() const {return work_semaphore.CurrentTick();}
    void Wait(u64 tick,std::source_location,u64) {work_semaphore.Wait(tick);}
    void Finish(std::source_location caller=std::source_location::current());
};
void Check(vk::Result result) {assert(result==vk::Result::eSuccess);}
'''

MAIN = r'''
} // namespace Vulkan
int main(int argc,char**) {
    const bool compare=argc>1;
    setenv("ISS_COMPARE_STATS",compare ? "1" : "0",1);
    setenv("TLG_PERF_DIAG","0",1);
    Driver driver;
    Instance instance{&driver};
    Vulkan::Scheduler scheduler{instance};
    std::vector<u64> callbacks;
    scheduler.pending_ops.push({[&] {callbacks.push_back(driver.completed);},1});
    scheduler.num_pending_ops=1;
    // Tens of thousands of draws must not issue timeline queries or run an unfinished callback.
    for(unsigned i=0;i<32000;++i) scheduler.PopPendingOperations();
    assert(driver.queries==0 && callbacks.empty());
    // The first eight submits need no timeline query. The ninth refresh confirms a whole batch.
    for(unsigned i=0;i<8;++i) {Vulkan::SubmitInfo info{};scheduler.SubmitExecution(info);}
    assert(driver.queries==0 && callbacks.empty());
    Vulkan::SubmitInfo info{};scheduler.SubmitExecution(info);
    assert(driver.queries==1 && callbacks.size()==1 && callbacks[0]>=1);
    for(unsigned i=9;i<800;++i) {
        scheduler.parallel=i%2;Vulkan::SubmitInfo next{};scheduler.SubmitExecution(next);
        assert(driver.buffers_last==(scheduler.parallel ? 2u : 1u));
    }
    assert(driver.queries<110 && driver.waits==0 && driver.submitted==800);
    assert(driver.max_backlog<=8 && scheduler.allocations==800);

    // Slow GPU: actual waits are required and the submission backlog stays bounded.
    Driver slow;slow.finish_batch=false;
    Instance slow_instance{&slow};
    Vulkan::Scheduler lagging{slow_instance};
    for(unsigned i=0;i<80;++i) {Vulkan::SubmitInfo next{};lagging.SubmitExecution(next);}
    assert(slow.waits>0 && slow.max_backlog<=8);
    bool recycled=false;
    lagging.pending_ops.push({[&] {assert(slow.completed>=80);recycled=true;},80});
    lagging.num_pending_ops=1;
    lagging.PopPendingOperations();
    assert(!recycled);
    // Completion obtained by an explicit wait makes callbacks reclaimable without another query.
    lagging.Finish();
    const auto queries=slow.queries;
    lagging.PopPendingOperations();
    assert(recycled && slow.queries==queries);

    // A callback can recursively pop; it must only execute once and keep FIFO order.
    unsigned calls=0;
    lagging.pending_ops.push({[&] {++calls;lagging.PopPendingOperations();},80});
    lagging.num_pending_ops=1;
    lagging.PopPendingOperations();
    assert(calls==1 && lagging.num_pending_ops==0);
    // Count actual driver waits, not cached completions or polling. Collection can be off.
    u64 recorded=0;
    for(const auto& row: Vulkan::WaitDiagnostics::Get().Take()) {
        if(!row.calls) continue;
        assert(row.file.find("window.cpp")!=std::string_view::npos && row.line>0);
        recorded+=row.calls;
    }
    assert(recorded==(compare ? slow.waits : 0));
}
'''


@unittest.skipUnless(CXX, "C++ compiler unavailable")
class BionicSubmissionWindowTests(unittest.TestCase):
    def test_batched_queries_preserve_completion_and_bound_gpu_backlog(self):
        scheduler = (VK / "vk_scheduler.cpp").read_text()
        semaphore = (VK / "vk_semaphore.cpp").read_text()
        source = PREAMBLE + "\n".join([
            method(semaphore, "void Semaphore::Refresh()"),
            method(semaphore, "void Semaphore::Wait(u64 tick,"),
            method(scheduler, "void Scheduler::PopPendingOperations()"),
            method(scheduler, "void Scheduler::SubmitExecution(SubmitInfo& info)"),
            method(scheduler, "void Scheduler::Finish(std::source_location caller)"),
        ]) + MAIN
        with tempfile.TemporaryDirectory() as directory:
            cpp = Path(directory) / "window.cpp"
            binary = Path(directory) / "window"
            cpp.write_text(source)
            result = subprocess.run([CXX, "-std=c++20", "-O2", "-pthread",
                                     "-DISS_ANDROID_BIONIC=1", "-I", str(ROOT / "port/gpu/shim"),
                                     "-I", str(VK),
                                     str(cpp), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True, timeout=20)
            subprocess.run([str(binary), "compare"], check=True, timeout=20)


if __name__ == "__main__":
    unittest.main()
