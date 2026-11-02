"""Run production Android transaction callbacks with reordered completions."""
from pathlib import Path
import shutil,subprocess,tempfile,unittest
from test_bionic_resource_reuse import method
ROOT=Path(__file__).resolve().parents[3]
CC=shutil.which('cc')
@unittest.skipUnless(CC,'C compiler required')
class AndroidCapture(unittest.TestCase):
 def test_owned_frame_ids_and_callback_lifetime(self):
  source=(ROOT/'tools/android/bionic/present/isspresent.c').read_text()
  structures=source[source.index('typedef struct {\n    uint32_t session, generation, index;'):source.index('static void wake_receiver')]
  bodies='\n'.join(method(source,signature) for signature in ['static int64_t trace_clock_ns(', 'static void trace_event(', 'static void on_complete(', 'static void on_commit(', 'static void show_frame(', 'static void pump_queue(', 'static void present_frame('])
  pre=r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <stdatomic.h>
#include <pthread.h>
#include "iss_present_proto.h"
#include "geometry.h"
typedef int ASurfaceControl;
typedef int AHardwareBuffer;
typedef int ASurfaceTransaction;
typedef int ANativeWindow;
typedef struct {int64_t latch;} ASurfaceTransactionStats;
typedef struct {int left,top,right,bottom;} ARect;
#define ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE 1
#define ASURFACE_TRANSACTION_VISIBILITY_SHOW 1
#define LOGE(...) ((void)0)
static int ANativeWindow_getWidth(ANativeWindow* w){(void)w;return 2688;}
static int ANativeWindow_getHeight(ANativeWindow* w){(void)w;return 1216;}
typedef struct {uint32_t generation,index,width,height;uint64_t frame;int fence;} Queued;
static struct {
 pthread_mutex_t lock;char path[108];ANativeWindow* window;ASurfaceControl* sc;
 int fullscreen,last_set,fifo,conn_fd;uint32_t session,generation,count,queue_head,queue_len;
 uint64_t txn_seq,inflight;Queued queue[ISS_PRESENT_MAX_BUFFERS];
 AHardwareBuffer* bufs[ISS_PRESENT_MAX_BUFFERS];
 int trace_indices[ISS_PRESENT_MAX_BUFFERS];uint64_t trace_frames[ISS_PRESENT_MAX_BUFFERS];
 atomic_ullong frames;
} S={.lock=PTHREAD_MUTEX_INITIALIZER,.fullscreen=1,.last_set=-1,.conn_fd=1,.session=1,.generation=1,.count=3};
static void* callbacks[8];static void (*done[8])(void*,ASurfaceTransactionStats*);
static unsigned applied=0,releases=0;static uint32_t last_release=0;
static int tx;
static ASurfaceTransaction* make_tx(void){return &tx;}
static void delete_tx(ASurfaceTransaction* t){(void)t;}
static void apply_tx(ASurfaceTransaction* t){(void)t;++applied;}
static void buffer_tx(ASurfaceTransaction*t,ASurfaceControl*s,AHardwareBuffer*b,int f){(void)t;(void)s;(void)b;(void)f;}
static void geometry_tx(ASurfaceTransaction*t,ASurfaceControl*s,const ARect*a,const ARect*b,int tr){(void)t;(void)s;assert(a->right==1280&&a->bottom==720);assert(b->right==2688&&b->bottom==1216&&tr==0);}
static void setting_tx(ASurfaceTransaction*t,ASurfaceControl*s,int8_t v){(void)t;(void)s;(void)v;}
static void callback_tx(ASurfaceTransaction*t,void*c,void(*fn)(void*,ASurfaceTransactionStats*)){(void)t;callbacks[applied]=c;done[applied]=fn;}
static void controls(ASurfaceTransactionStats* s,ASurfaceControl*** p,size_t* n){(void)s;*n=1;*p=malloc(sizeof(**p));(*p)[0]=S.sc;}
static int release_fd(ASurfaceTransactionStats*s,ASurfaceControl*c){(void)s;(void)c;return -1;}
static int64_t latch_time(ASurfaceTransactionStats*s){return s->latch;}
static void release_controls(ASurfaceControl**p){free(p);}
static struct {
 int ok;ASurfaceTransaction*(*txn_create)(void);void(*txn_delete)(ASurfaceTransaction*);void(*txn_apply)(ASurfaceTransaction*);
 void(*set_buffer)(ASurfaceTransaction*,ASurfaceControl*,AHardwareBuffer*,int);
 void(*set_geometry)(ASurfaceTransaction*,ASurfaceControl*,const ARect*,const ARect*,int);
 void(*set_transparency)(ASurfaceTransaction*,ASurfaceControl*,int8_t);void(*set_visibility)(ASurfaceTransaction*,ASurfaceControl*,int8_t);
 void(*set_on_complete)(ASurfaceTransaction*,void*,void(*)(void*,ASurfaceTransactionStats*));
 void(*set_on_commit)(ASurfaceTransaction*,void*,void(*)(void*,ASurfaceTransactionStats*));
 void(*get_controls)(ASurfaceTransactionStats*,ASurfaceControl***,size_t*);
 int(*prev_release_fence)(ASurfaceTransactionStats*,ASurfaceControl*);
 void(*release_controls)(ASurfaceControl**);int64_t(*get_latch_time)(ASurfaceTransactionStats*);
} api={.ok=1,.txn_create=make_tx,.txn_delete=delete_tx,.txn_apply=apply_tx,.set_buffer=buffer_tx,
 .set_geometry=geometry_tx,.set_transparency=setting_tx,.set_visibility=setting_tx,.set_on_complete=callback_tx,
 .get_controls=controls,.prev_release_fence=release_fd,.release_controls=release_controls,.get_latch_time=latch_time};
static void send_release(uint32_t g,uint32_t i,uint64_t f,int fence){(void)g;(void)f;assert(fence==-1);++releases;last_release=i;}
static void wake_receiver(void){}
static void pump_queue(void);
'''
  main=r'''
static void check_file(const char* dir,int i,const char* wanted,const char* unwanted){
 char path[512],text[4096];snprintf(path,sizeof(path),"%s/fsr-capture/f%03d_android_info.txt",dir,i);
 FILE*f=fopen(path,"rb");assert(f);size_t n=fread(text,1,sizeof(text)-1,f);text[n]=0;fclose(f);
 assert(strstr(text,wanted));if(unwanted)assert(!strstr(text,unwanted));
}
int main(int argc,char**argv){
 assert(argc==2);snprintf(S.path,sizeof(S.path),"%s/present.sock",argv[1]);
 int control=1,window=1,buffer=1;S.sc=&control;S.window=&window;for(int i=0;i<3;i++)S.bufs[i]=&buffer;
 iss_present_msg m=iss_present_make(ISS_PRESENT_FRAME);m.generation=1;m.width=1280;m.height=720;
 assert(sizeof(m)==40&&iss_present_trace_index(&m)==-1);
 m.format=ISS_PRESENT_TRACE_MAGIC;m.count=1;m.frame=42;m.index=0;
 assert(iss_present_trace_index(&m)==0);present_frame(&m,-1);
 m.count=0;m.frame=43;m.index=1;present_frame(&m,-1); // ordinary frame does not inherit trace zero
 m.count=120;m.frame=44;m.index=2;assert(iss_present_trace_index(&m)==119);present_frame(&m,-1);
 assert(applied==3);ASurfaceTransactionStats stats={.latch=123456};
 done[2](callbacks[2],&stats);assert(releases==1&&last_release==1);
 done[0](callbacks[0],&stats); // callback order must not relabel frame 42 as frame 44
 check_file(argv[1],0,"event=complete frame=42","frame=44");
 check_file(argv[1],119,"event=complete frame=44","frame=42");
 check_file(argv[1],0,"destination=0,0,2688,1216",NULL);
 ++S.session;done[1](callbacks[1],&stats);assert(releases==1); // stale session cannot release a current buffer
 m.count=121;assert(iss_present_trace_index(&m)==-1);m.count=1;m.type=ISS_PRESENT_MODE;assert(iss_present_trace_index(&m)==-1);
 m.type=ISS_PRESENT_FRAME;m.count=10;m.index=7;m.frame=45;present_frame(&m,-1);
 check_file(argv[1],9,"reason=invalid_buffer",NULL);
}
'''
  with tempfile.TemporaryDirectory() as tmp:
   p=Path(tmp);(p/'fsr-capture').mkdir();(p/'test.c').write_text(pre+structures+bodies+main)
   subprocess.run([CC,'-std=c11','-D_POSIX_C_SOURCE=200809L','-Wall','-Wextra','-pthread','-I',str(ROOT/'tools/android/present'),'-I',str(ROOT/'tools/android/bionic/present'),str(p/'test.c'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test'),tmp],check=True)
   self.assertEqual({q.name for q in (p/'fsr-capture').iterdir()},{'f000_android_info.txt','f119_android_info.txt','f009_android_info.txt'})

class AhbReadback(unittest.TestCase):
 def test_production_copy_owns_image_and_restores_layout(self):
  compiler=shutil.which('c++')
  candidates=[ROOT/'port/gpu/third_party/vulkan-headers/include',Path('/usr/include'),ROOT.parent/'replay-deps/root/usr/include']
  include=next((p for p in candidates if (p/'vulkan/vulkan.hpp').is_file()),None)
  if not compiler or include is None:self.skipTest('Vulkan-Hpp and C++ compiler required')
  source=(ROOT/'port/gpu/shadps4/video_core/renderer_vulkan/vk_presenter.cpp').read_text()
  body=method(source,'static void CaptureAhb(')
  code=r'''
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#define VULKAN_HPP_NO_CONSTRUCTORS
#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
#include <array>
#include <cassert>
#include <cstdint>
#include <string>
#include <vector>
#include "iss_temporal_capture.h"
using u32=uint32_t;using u64=uint64_t;using s32=int32_t;
namespace Vulkan {
struct Instance{};struct Scheduler{};
struct Frame {int capture_index=-1;u64 capture_serial=0,captured_ahb_serial=0;u32 width=1280,height=720;};
struct Swapchain {
 bool supported=true;int token=-1;u64 serial=0;std::string status;std::array<u32,6> dimensions{};
 vk::Extent2D GetExtent(){return {1280,720};}
 bool CaptureSupported(){return supported;}
 vk::Image Image(){return vk::Image{reinterpret_cast<VkImage>(uintptr_t(123))};}
 void SetCaptureToken(int i,u64 s,const char* state,u32 x,u32 y,u32 w,u32 h,u32 fw,u32 fh){token=i;serial=s;status=state;dimensions={x,y,w,h,fw,fh};}
};
std::vector<std::string> events;std::vector<VkImageMemoryBarrier> barriers;bool allocate=true;
static bool DumpRaw(const Instance&,Scheduler&,vk::CommandBuffer,vk::Image image,vk::ImageLayout layout,
                    u32 w,u32 h,const char* name,int index,u32 x,u32 y,bool trace){
 assert(image==vk::Image{reinterpret_cast<VkImage>(uintptr_t(123))});
 assert(layout==vk::ImageLayout::eTransferSrcOptimal&&w==640&&h==360&&x==320&&y==180&&index==0&&trace);
 assert(std::string(name)=="trace_ahb");events.push_back("copy");return allocate;
}
'''+body+r'''
}
static VKAPI_ATTR void VKAPI_CALL barrier(VkCommandBuffer,VkPipelineStageFlags src,VkPipelineStageFlags dst,
 VkDependencyFlags,uint32_t,const VkMemoryBarrier*,uint32_t,const VkBufferMemoryBarrier*,uint32_t n,const VkImageMemoryBarrier* b){
 assert(src==VK_PIPELINE_STAGE_TRANSFER_BIT&&dst==VK_PIPELINE_STAGE_TRANSFER_BIT&&n==1);
 assert(b->srcQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED&&b->dstQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED);
 Vulkan::barriers.push_back(*b);Vulkan::events.push_back("barrier");
}
int main(){
 VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier=barrier;
 vk::CommandBuffer cmd{reinterpret_cast<VkCommandBuffer>(uintptr_t(1))};
 Vulkan::Instance instance;Vulkan::Scheduler scheduler;Vulkan::Swapchain swapchain;Vulkan::Frame frame;
 Vulkan::CaptureAhb(instance,scheduler,cmd,swapchain,frame);assert(Vulkan::events.empty());
 frame.capture_index=0;frame.capture_serial=55;
 Vulkan::CaptureAhb(instance,scheduler,cmd,swapchain,frame);
 assert((Vulkan::events==std::vector<std::string>{"barrier","copy","barrier"}));
 assert(Vulkan::barriers[0].oldLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL&&Vulkan::barriers[0].newLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
 assert(Vulkan::barriers[1].oldLayout==VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL&&Vulkan::barriers[1].newLayout==VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
 assert(swapchain.token==0&&swapchain.serial==55&&swapchain.status=="copied");
 Vulkan::CaptureAhb(instance,scheduler,cmd,swapchain,frame);assert(Vulkan::events.size()==3);
 frame.capture_serial=56;swapchain.supported=false;
 Vulkan::CaptureAhb(instance,scheduler,cmd,swapchain,frame);assert(Vulkan::events.size()==3&&swapchain.status=="unsupported");
 frame.capture_serial=57;swapchain.supported=true;Vulkan::allocate=false;
 Vulkan::CaptureAhb(instance,scheduler,cmd,swapchain,frame);assert(Vulkan::events.size()==6&&swapchain.status=="allocation_failed");
}
'''
  with tempfile.TemporaryDirectory() as tmp:
   p=Path(tmp);(p/'test.cpp').write_text(code)
   subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror','-I',str(include),'-I',str(ROOT/'port/gpu/shadps4/video_core/renderer_vulkan'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)

 def test_actual_sender_preserves_protocol_and_frame_counter(self):
  compiler=shutil.which('c++')
  if not compiler:self.skipTest('C++ compiler required')
  source=(ROOT/'port/gpu/shim/iss_present_client.cpp').read_text()
  body=method(source,'bool Client::SendFrame(')
  code=r'''
#include <cstdint>
#include <cstring>
#include <mutex>
#include <cassert>
#define _Static_assert static_assert
#include "iss_present_proto.h"
using u32=uint32_t;using u64=uint64_t;
namespace IssPresent {
struct Client {
 int fd=1;std::mutex send_mutex;iss_present_msg packet{};int fence=-1;
 bool SendRaw(const void* p,int f){std::memcpy(&packet,p,sizeof(packet));fence=f;return true;}
 bool SendFrame(u32,u32,u64,u32,u32,int,int trace=-1);
};
'''+body+r'''
}
int main(){
 IssPresent::Client client;
 assert(client.SendFrame(3,2,1001,1280,720,17));
 auto original=client.packet;assert(sizeof(original)==40&&original.version==3&&original.frame==1001&&original.generation==3&&original.index==2);
 assert(original.format==0&&original.count==0&&client.fence==17&&iss_present_trace_index(&original)==-1);
 assert(client.SendFrame(3,1,1002,1280,720,18,0));assert(client.packet.frame==1002&&client.packet.index==1&&client.fence==18&&iss_present_trace_index(&client.packet)==0);
 assert(client.SendFrame(3,0,1003,1280,720,-1,119));assert(iss_present_trace_index(&client.packet)==119&&client.packet.frame==1003);
 assert(client.SendFrame(3,0,1004,1280,720,-1,120));assert(iss_present_trace_index(&client.packet)==-1);
 client.fd=-1;assert(!client.SendFrame(3,0,1005,1280,720,-1,0));assert(client.packet.frame==1004);
}
'''
  with tempfile.TemporaryDirectory() as tmp:
   p=Path(tmp);(p/'test.cpp').write_text(code)
   subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror','-I',str(ROOT/'tools/android/present'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)

 def test_device_writes_are_made_visible_to_host_before_deferred_reads(self):
  compiler=shutil.which('c++');candidates=[ROOT/'port/gpu/third_party/vulkan-headers/include',Path('/usr/include'),ROOT.parent/'replay-deps/root/usr/include']
  include=next((p for p in candidates if (p/'vulkan/vulkan.hpp').is_file()),None)
  if not compiler or include is None:self.skipTest('Vulkan-Hpp and C++ compiler required')
  directory=ROOT/'port/gpu/shadps4/video_core/renderer_vulkan'
  for file,signature in [('vk_presenter.cpp','static bool DumpRaw('),('vk_temporal_upscaler.cpp','void DumpImages(')]:
   code=method((directory/file).read_text(),signature)
   self.assertLess(code.index('copyImageToBuffer'),code.index('CaptureHostReadBarrier('))
   self.assertLess(code.index('CaptureHostReadBarrier('),code.index('DeferPriorityOperation('))
  code=r'''
#define VULKAN_HPP_NO_CONSTRUCTORS
#define VULKAN_HPP_DISPATCH_LOADER_DYNAMIC 1
#include <vulkan/vulkan.hpp>
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
#include <cassert>
#include <cstdint>
#include "vk_capture_host_read.h"
static unsigned called=0;
static VKAPI_ATTR void VKAPI_CALL barrier(VkCommandBuffer,const VkDependencyInfo* info){
 assert(info->bufferMemoryBarrierCount==1&&info->memoryBarrierCount==0&&info->imageMemoryBarrierCount==0);
 const auto& b=info->pBufferMemoryBarriers[0];
 assert(b.srcStageMask==VK_PIPELINE_STAGE_2_TRANSFER_BIT&&b.srcAccessMask==VK_ACCESS_2_TRANSFER_WRITE_BIT);
 assert(b.dstStageMask==VK_PIPELINE_STAGE_2_HOST_BIT&&b.dstAccessMask==VK_ACCESS_2_HOST_READ_BIT);
 assert(b.srcQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED&&b.dstQueueFamilyIndex==VK_QUEUE_FAMILY_IGNORED);
 assert(b.buffer==reinterpret_cast<VkBuffer>(uintptr_t(77))&&b.offset==0&&b.size==1234);++called;
}
int main(){
 VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPipelineBarrier2=barrier;
 Vulkan::CaptureHostReadBarrier(vk::CommandBuffer{reinterpret_cast<VkCommandBuffer>(uintptr_t(1))},
  vk::Buffer{reinterpret_cast<VkBuffer>(uintptr_t(77))},1234);
 assert(called==1);
}
'''
  with tempfile.TemporaryDirectory() as tmp:
   p=Path(tmp);(p/'test.cpp').write_text(code)
   subprocess.run([compiler,'-std=c++20','-Wall','-Wextra','-Werror','-I',str(include),'-I',str(directory),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
   subprocess.run([str(p/'test')],check=True)
