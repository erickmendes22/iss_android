"""Production producer selection, private queue submission and stable arena binding."""
from pathlib import Path
import os,shutil,subprocess,tempfile,unittest
from test_bionic_resource_reuse import method
ROOT=Path(__file__).resolve().parents[3]
GPU=ROOT/'port/gpu/shadps4'
BC=GPU/'video_core/buffer_cache/buffer_cache.cpp'
VK=GPU/'video_core/renderer_vulkan'
CXX=shutil.which('c++')
@unittest.skipUnless(CXX,'C++ compiler required')
class ProducerReadbacks(unittest.TestCase):
 def compile_run(self,code,cases=((),)):
  include=os.environ.get('VULKAN_HEADERS_INCLUDE')
  if not include:self.skipTest('Vulkan-Headers checked in native CI')
  with tempfile.TemporaryDirectory() as d:
   p=Path(d);(p/'test.cpp').write_text(code)
   result=subprocess.run([CXX,'-std=c++20','-DISS_ANDROID_BIONIC','-DVULKAN_HPP_NO_CONSTRUCTORS','-DVULKAN_HPP_NO_EXCEPTIONS','-DVULKAN_HPP_DISPATCH_LOADER_DYNAMIC=1','-O1','-Wall','-Wextra','-Werror','-I',str(GPU),'-I',include,str(p/'test.cpp'),'-o',str(p/'test')],capture_output=True,text=True)
   self.assertEqual(result.returncode,0,result.stderr)
   for args in cases:
    with self.subTest(args=args):
     result=subprocess.run([str(p/'test'),*args],capture_output=True,text=True)
     self.assertEqual(result.returncode,0,result.stderr)
 def test_requested_pages_and_stable_small_read(self):
  code=r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <cstdlib>
#include <tuple>
#include <string>
#include <vector>
#include "video_core/buffer_cache/hybrid_readback_policy.h"
#include "video_core/buffer_cache/write_tick_tracker.h"
using u64=uint64_t;using u32=uint32_t;using VAddr=u64;
namespace Common {template<class T>T AlignDown(T n,T a){return n/a*a;}template<class T>T AlignUp(T n,T a){return (n+a-1)/a*a;}}
struct {bool dma=false;bool IsDirectMemoryAccessEnabled(){return dma;}} EmulatorSettings;
namespace VideoCore {
struct RegionManager {static inline bool hybrid=true;static bool HybridReadProtectionEnabled(){return hybrid;}};
struct Buffer {u64 cpu_addr=0x10000000,size_bytes=0x100000;bool IsInBounds(u64 a,u64 n)const{return a>=cpu_addr&&n<=size_bytes&&a-cpu_addr<=size_bytes-n;}u64 Offset(u64 a)const{return a-cpu_addr;}};
struct BufferCache {
 static constexpr u64 ADDRESS_SPACE_BITS=40,STREAM_THRESHOLD=65536;
 struct {u64 done=7;bool IsFree(u64 n)const{return n<=done;}} scheduler;
 struct {bool cpu=false,gpu=false,change=false;unsigned queries=0;
  bool IsRegionCpuModified(u64,u64){return cpu || (change && ++queries==2);}
  bool IsRegionGpuModified(u64,u64){return gpu;}
 } tracker;decltype(tracker)* memory_tracker=&tracker;
 struct {bool hazard=false;bool IsBufferAccessed(const Buffer*,u64,u64){return hazard;}} runtime;
 struct {u64 UniformMinAlignment()const{return 256;}} instance;
 struct {bool hole=false;template<class F>void ForEachGap(u64 a,u64 b,F f){if(hole)f(a,b);}} resident_ranges;
 unsigned arena_page_bits=28,block_shift=12;
 std::array<const Buffer*,4096> address_space{};Buffer arena;
 WriteTickTracker write_ticks;
 BufferCache(){address_space[1]=&arena;}
 const Buffer* GetArena(u64,u64){return &arena;}
 std::tuple<const Buffer*,VAddr,VAddr> GetReadbackWindow(VAddr,u64);
 const Buffer* StableSmallRead(VAddr,u32,bool);
};
BODY
}
int main(int argc,char** argv){
 using namespace VideoCore;
 if(argc==2&&std::string(argv[1])=="off") {setenv("TLG_HYBRID_REQUEST_PAGES","0",1);setenv("TLG_STABLE_SMALL_READ","0",1);setenv("TLG_READBACK_QUEUE","0",1);}
 const bool off=argc==2;
 assert(HybridReadbackPolicy::RequestedQueueCount(1,true)==1);
 assert(HybridReadbackPolicy::RequestedQueueCount(4,false)==1);
 assert(HybridReadbackPolicy::RequestedQueueCount(4,true)==(off?1u:2u));
 BufferCache c;const u64 a=c.arena.cpu_addr;
 c.write_ticks.Write(a+0x3000,4096,7);c.write_ticks.Write(a+0x9000,4096,9);
 auto [arena,first,last]=c.GetReadbackWindow(a+0x3004,4);
 assert(arena==&c.arena);assert(first==a+(off?0:0x3000));assert(last==a+(off?0x80000:0x4000));
 c.scheduler.done=9;assert(std::get<2>(c.GetReadbackWindow(a+0x3004,4))==a+0x80000);
 c.scheduler.done=6;assert(std::get<2>(c.GetReadbackWindow(a+0x3004,4))==a+0x80000);
 c.scheduler.done=7;assert(std::get<2>(c.GetReadbackWindow(a+0x5004,4))==a+0x80000); // unknown requested producer
 EmulatorSettings.dma=true;assert(std::get<2>(c.GetReadbackWindow(a+0x3004,4))==a+0x80000);EmulatorSettings.dma=false;
 RegionManager::hybrid=false;assert(std::get<2>(c.GetReadbackWindow(a+0x3004,4))==a+0x80000);RegionManager::hybrid=true;
 assert(c.StableSmallRead(a+0x1000,32,false)==(off?nullptr:&c.arena));
 if(off)return 0;
 assert(!c.StableSmallRead(a+0x1000,32,true));assert(!c.StableSmallRead(a+0x1001,32,false));
 assert(!c.StableSmallRead(0,32,false));assert(!c.StableSmallRead(a,0,false));
 c.tracker.cpu=true;assert(!c.StableSmallRead(a+0x1000,32,false));c.tracker.cpu=false;
 c.tracker.gpu=true;assert(!c.StableSmallRead(a+0x1000,32,false));c.tracker.gpu=false;
 c.runtime.hazard=true;assert(!c.StableSmallRead(a+0x1000,32,false));c.runtime.hazard=false;
 c.resident_ranges.hole=true;assert(!c.StableSmallRead(a+0x1000,32,false));c.resident_ranges.hole=false;
 c.address_space[1]=nullptr;assert(!c.StableSmallRead(a+0x1000,32,false));c.address_space[1]=&c.arena;
 Buffer replacement;replacement.cpu_addr+=0x100000;c.address_space[1]=&replacement;
 assert(!c.StableSmallRead(a+0x1000,32,false));c.address_space[1]=&c.arena;
 c.tracker.change=true;c.tracker.queries=0;assert(!c.StableSmallRead(a+0x1000,32,false)); // write during proof
}
'''
  source=BC.read_text();code=code.replace('BODY',method(source,'std::tuple<const Buffer*, VAddr, VAddr> BufferCache::GetReadbackWindow(')+'\n'+method(source,'const Buffer* BufferCache::StableSmallRead('))
  self.compile_run(code,((),('off',)))
 def test_staging_queue_eligibility_and_completion_identity(self):
  code=r'''
#include <algorithm>
#include <cassert>
#include <memory>
#include <vector>
#include <cstdint>
#include <vulkan/vulkan.hpp>
#include "video_core/buffer_cache/write_tick_tracker.h"
using u64=uint64_t;using VAddr=u64;
namespace Common {template<class T>T AlignDown(T n,T a){return n/a*a;}}
struct {bool dma=false;bool IsDirectMemoryAccessEnabled(){return dma;}} EmulatorSettings;
namespace Vulkan {
struct Semaphore {vk::Semaphore handle;vk::Semaphore Handle()const{return handle;}};
struct Queue {
 Semaphore sem{vk::Semaphore{reinterpret_cast<VkSemaphore>(30)}};unsigned calls=0;bool blocked=false;
 Semaphore& Timeline(){return sem;}
 template<class C>u64 Submit(vk::Buffer a,vk::Buffer b,const C& copies,vk::Semaphore producer,u64 tick,vk::Semaphore residency,u64 bind){
  assert(a==vk::Buffer{reinterpret_cast<VkBuffer>(1)}&&b==vk::Buffer{reinterpret_cast<VkBuffer>(2)});
  assert(copies.size()==1&&tick==7&&bind==3);
  assert(producer==vk::Semaphore{reinterpret_cast<VkSemaphore>(10)}&&residency==vk::Semaphore{reinterpret_cast<VkSemaphore>(20)});
  ++calls;return blocked ? 0 : 101;
 }
};
}
namespace VideoCore {
struct Buffer {vk::Buffer handle;auto Handle()const{return handle;}};
struct BufferCache {
 struct DeferredReadback {bool queued=false;u64 tick{},source_tick{},arena_base=0x10000000;
  struct {Buffer* buffer;} staging;
  std::vector<vk::BufferCopy> copies{{.srcOffset=4096,.dstOffset=128,.size=32}};
 };
 struct {Vulkan::Semaphore sem{vk::Semaphore{reinterpret_cast<VkSemaphore>(10)}};unsigned flushes=0;u64 submitted=8;
  u64 CurrentTick()const{return 10;}u64 SubmittedTick()const{return submitted;}auto* GetWorkSemaphore(){return &sem;}void Flush(){++flushes;}
 } scheduler;
 struct {unsigned copies=0;template<class C>void CopyBuffer(const Buffer*,Buffer*,const C&){++copies;}} runtime;
 Vulkan::Semaphore memory_semaphore{vk::Semaphore{reinterpret_cast<VkSemaphore>(20)}};
 u64 submitted_residency_tick=3;std::vector<int> pending_binds;
 std::unique_ptr<Vulkan::Queue> readback_queue=std::make_unique<Vulkan::Queue>();
 WriteTickTracker write_ticks;
 void RecordStagingReadback(const Buffer*,DeferredReadback&);
 Vulkan::Semaphore* ReadbackCompletion(const DeferredReadback&);
};
BODY
}
int main(){
 using namespace VideoCore;
 for(unsigned test=0;test<8;++test){
  BufferCache c;Buffer src{vk::Buffer{reinterpret_cast<VkBuffer>(1)}},dst{vk::Buffer{reinterpret_cast<VkBuffer>(2)}};
  BufferCache::DeferredReadback r;r.staging.buffer=&dst;
  if(test!=1)c.write_ticks.Write(r.arena_base+4096,32,7);
  if(test==2)c.scheduler.submitted=6;
  if(test==3)c.pending_binds.push_back(1);
  if(test==4)EmulatorSettings.dma=true;
  if(test==5)c.readback_queue.reset();
  if(test==6)r.copies[0].size=8192;
  if(test==7)c.readback_queue->blocked=true;
  c.RecordStagingReadback(&src,r);assert(r.source_tick==(test==0 ? 7u : 10u));
  if(test==0){assert(r.queued&&r.tick==101&&c.runtime.copies==0&&c.scheduler.flushes==0);assert(c.ReadbackCompletion(r)==&c.readback_queue->sem);}
  else {assert(!r.queued&&r.tick==10&&c.runtime.copies==1&&c.scheduler.flushes==1);assert(c.ReadbackCompletion(r)==&c.scheduler.sem);}
  EmulatorSettings.dma=false;
 }
}
'''
  s=BC.read_text();code=code.replace('BODY',method(s,'void BufferCache::RecordStagingReadback(')+'\n'+method(s,'Vulkan::Semaphore* BufferCache::ReadbackCompletion('));self.compile_run(code)
 def test_private_queue_host_visibility_and_command_lifetime(self):
  code=r'''
#include <algorithm>
#include <array>
#include <cassert>
#include <map>
#include <span>
#include <vector>
#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
using u64=uint64_t;using u32=uint32_t;
static u64 completed=0,submitted=0,waits=0;static unsigned allocations=0,copies=0,barriers=0,resets=0,destroyed=0;
static std::map<VkCommandBuffer,u64> command_ticks;
static VKAPI_ATTR VkResult VKAPI_CALL Create(VkDevice,const VkCommandPoolCreateInfo* p,const VkAllocationCallbacks*,VkCommandPool* out){assert(p->queueFamilyIndex==2&&p->flags==VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT);*out=reinterpret_cast<VkCommandPool>(1);return VK_SUCCESS;}
static VKAPI_ATTR void VKAPI_CALL Destroy(VkDevice,VkCommandPool,const VkAllocationCallbacks*){assert(completed==submitted);++destroyed;}
static VKAPI_ATTR VkResult VKAPI_CALL Allocate(VkDevice,const VkCommandBufferAllocateInfo* p,VkCommandBuffer* out){assert(p->commandBufferCount==1);*out=reinterpret_cast<VkCommandBuffer>(uintptr_t(++allocations));return VK_SUCCESS;}
static VKAPI_ATTR VkResult VKAPI_CALL Reset(VkCommandBuffer c,VkCommandBufferResetFlags){assert(command_ticks.at(c)<=completed);++resets;return VK_SUCCESS;}
static VKAPI_ATTR VkResult VKAPI_CALL Begin(VkCommandBuffer,const VkCommandBufferBeginInfo*){return VK_SUCCESS;}
static VKAPI_ATTR VkResult VKAPI_CALL End(VkCommandBuffer){return VK_SUCCESS;}
static VKAPI_ATTR void VKAPI_CALL Copy(VkCommandBuffer,VkBuffer,VkBuffer,uint32_t n,const VkBufferCopy* p){assert(n==1&&p->srcOffset==4096&&p->dstOffset==128&&p->size==32);++copies;}
static VKAPI_ATTR void VKAPI_CALL Barrier(VkCommandBuffer,const VkDependencyInfo* p){assert(p->memoryBarrierCount==1);auto& b=p->pMemoryBarriers[0];assert(b.srcStageMask==VK_PIPELINE_STAGE_2_COPY_BIT&&b.srcAccessMask==VK_ACCESS_2_TRANSFER_WRITE_BIT);assert(b.dstStageMask==VK_PIPELINE_STAGE_2_HOST_BIT&&b.dstAccessMask==VK_ACCESS_2_HOST_READ_BIT);++barriers;}
static VKAPI_ATTR VkResult VKAPI_CALL Submit(VkQueue q,uint32_t n,const VkSubmitInfo2* p,VkFence){assert(q==reinterpret_cast<VkQueue>(2)&&n==1);assert(p->waitSemaphoreInfoCount==2&&p->commandBufferInfoCount==1&&p->signalSemaphoreInfoCount==1);assert(p->pWaitSemaphoreInfos[0].value==7&&p->pWaitSemaphoreInfos[1].value==3);for(unsigned i=0;i<2;++i)assert(p->pWaitSemaphoreInfos[i].stageMask==VK_PIPELINE_STAGE_2_COPY_BIT);assert(p->pSignalSemaphoreInfos[0].stageMask==VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);assert(p->pSignalSemaphoreInfos[0].value==++submitted);command_ticks[p->pCommandBufferInfos[0].commandBuffer]=submitted;return VK_SUCCESS;}
namespace Vulkan {
struct Instance {vk::Device GetDevice()const{return vk::Device{reinterpret_cast<VkDevice>(1)};}u32 GetGraphicsQueueFamilyIndex()const{return 2;}vk::Queue GetReadbackQueue()const{return vk::Queue{reinterpret_cast<VkQueue>(2)};}};
struct Semaphore {u64 next=1;explicit Semaphore(const Instance&){}void Refresh(){}bool IsFree(u64 n)const{return n<=completed;}void Wait(u64 n){assert(n<=submitted);++waits;completed=std::max(completed,n);}u64 NextTick(){return next++;}vk::Semaphore Handle()const{return vk::Semaphore{reinterpret_cast<VkSemaphore>(30)};}};
void Check(vk::Result r){assert(r==vk::Result::eSuccess);}template<class T>T Check(vk::ResultValue<T> r){assert(r.result==vk::Result::eSuccess);return std::move(r.value);}
}
HEADER
BODY
int main(){
 VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateCommandPool=Create;VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroyCommandPool=Destroy;
 VULKAN_HPP_DEFAULT_DISPATCHER.vkAllocateCommandBuffers=Allocate;VULKAN_HPP_DEFAULT_DISPATCHER.vkResetCommandBuffer=Reset;
 VULKAN_HPP_DEFAULT_DISPATCHER.vkBeginCommandBuffer=Begin;VULKAN_HPP_DEFAULT_DISPATCHER.vkEndCommandBuffer=End;
 VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdCopyBuffer=Copy;VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier2=Barrier;VULKAN_HPP_DEFAULT_DISPATCHER.vkQueueSubmit2=Submit;
 Vulkan::Instance instance;
 {Vulkan::ReadbackQueue q(instance);std::array<vk::BufferCopy,1> regions{{{.srcOffset=4096,.dstOffset=128,.size=32}}};
  for(unsigned i=0;i<64;++i)assert(q.Submit({}, {}, regions,{},7,{},3)==i+1);
  assert(q.Submit({}, {}, regions,{},7,{},3)==0&&copies==64&&waits==0);
  for(unsigned i=0;i<6;++i){completed=i+1;assert(q.Submit({}, {}, regions,{},7,{},3)==65+i);}
  assert(allocations==64&&resets==6&&copies==70&&barriers==70&&waits==0&&q.LastTick()==70);
 }
 assert(completed==70&&waits==1&&destroyed==1);
}
'''
  h=(VK/'vk_readback_queue.h').read_text();h='\n'.join(x for x in h.splitlines() if not x.startswith('#pragma') and 'vk_semaphore.h' not in x)
  s=(VK/'vk_readback_queue.cpp').read_text();s='\n'.join(x for x in s.splitlines() if not x.startswith('#include'))
  self.compile_run(code.replace('HEADER',h).replace('BODY',s))
 def test_graphics_war_wait_even_without_sparse_binds(self):
  code=r'''
#include <algorithm>
#include <cassert>
#include <chrono>
#include <memory>
#include <vector>
#include <vulkan/vulkan.hpp>
using u64=uint64_t;using u32=uint32_t;
#define ASSERT_MSG(condition,...) assert(condition)
template<class... T>void Log(T...){ }
#define LOG_WARNING(category,format,...) Log(__VA_ARGS__)
namespace Vulkan {
struct SubmitInfo {std::array<vk::Semaphore,4> semas{};std::array<u64,4> ticks{};unsigned count=0;void AddWait(vk::Semaphore s,u64 n){assert(count<4);semas[count]=s;ticks[count++]=n;}};
struct Semaphore {u64 next=1;u64 NextTick(){return next++;}vk::Semaphore Handle()const{return vk::Semaphore{reinterpret_cast<VkSemaphore>(20)};}};
struct Queue {Semaphore sem;u64 LastTick()const{return 101;}Semaphore& Timeline(){return sem;}};
}
namespace VideoCore {
struct Buffer {vk::Buffer Handle()const{return vk::Buffer{reinterpret_cast<VkBuffer>(1)};}};
struct BufferCache {
 struct Bind {Buffer* arena;std::vector<vk::SparseMemoryBind> binds;};
 std::vector<Bind> pending_binds;
 struct {unsigned calls=0;auto GetGraphicsQueue(){return *this;}vk::Result bindSparse(const vk::BindSparseInfo& s){assert(s.bufferBindCount==1);assert(s.signalSemaphoreCount==1);return vk::Result::eSuccess;}} instance;
 std::unique_ptr<Vulkan::Queue> readback_queue=std::make_unique<Vulkan::Queue>();
 Vulkan::Semaphore memory_semaphore;
 u64 submitted_residency_tick=0,residency_allocations=0;unsigned block_shift=12;
 struct Interval {u64 start,end;};std::vector<Interval> resident_ranges;
 void SubmitPendingArenaBinds(Vulkan::SubmitInfo&);
};
BODY
}
int main(){
 VideoCore::BufferCache c;Vulkan::SubmitInfo empty;c.SubmitPendingArenaBinds(empty);
 assert(empty.count==1&&empty.ticks[0]==101); // WAR wait cannot depend on new residency work
 VideoCore::Buffer arena;c.pending_binds.push_back({&arena,{{.resourceOffset=0,.size=4096}}});
 Vulkan::SubmitInfo with_acquire;with_acquire.AddWait({},1);c.SubmitPendingArenaBinds(with_acquire);
 assert(with_acquire.count==3&&with_acquire.ticks[1]==101&&with_acquire.ticks[2]==1);
 assert(c.pending_binds.empty()&&c.submitted_residency_tick==1);
 c.readback_queue.reset();Vulkan::SubmitInfo fallback;c.SubmitPendingArenaBinds(fallback);assert(fallback.count==0);
}
'''
  self.compile_run(code.replace('BODY',method(BC.read_text(),'void BufferCache::SubmitPendingArenaBinds(')))
