// The production methods are compiled with actual Vulkan-Hpp types and controlled driver calls.
#include <cassert>
#include <atomic>
#include <array>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/descriptor_template_data.h"
#include "video_core/renderer_vulkan/vk_descriptor_replay_memo.h"
VULKAN_HPP_DEFAULT_DISPATCH_LOADER_DYNAMIC_STORAGE
using u32=uint32_t;using u64=uint64_t;
namespace boost::container {template<class T,size_t N> using small_vector=std::vector<T>;}
namespace Shader {struct PushData {std::array<u32,32> words{};};}
namespace IssBionicPerf {
std::atomic<u64> descriptor_template_calls{},descriptor_template_bytes{},descriptor_legacy_calls{};
std::atomic<u64> descriptor_delta_partial{}, descriptor_delta_bindings{}, descriptor_replay_skipped{};
bool Enabled(){return true;}
}
#define LOG_WARNING(...) ((void)0)
template<class T> T H(uintptr_t x) {
    if constexpr(std::is_pointer_v<T>) return reinterpret_cast<T>(x);
    else return static_cast<T>(x);
}
static unsigned pushed,legacy,constants,created,destroyed,sets;
static bool fail_create;
static bool expect_compute;
static unsigned expected_writes;
static u64 observed;
static std::vector<VkDescriptorUpdateTemplateEntry> entries;
static VKAPI_ATTR VkResult VKAPI_CALL CreateLayout(VkDevice,const VkPipelineLayoutCreateInfo*,
    const VkAllocationCallbacks*,VkPipelineLayout* out){*out=H<VkPipelineLayout>(1);return VK_SUCCESS;}
static VKAPI_ATTR void VKAPI_CALL DestroyLayout(VkDevice,VkPipelineLayout,const VkAllocationCallbacks*){}
static VKAPI_ATTR VkResult VKAPI_CALL CreateSet(VkDevice,const VkDescriptorSetLayoutCreateInfo*,
    const VkAllocationCallbacks*,VkDescriptorSetLayout* out){*out=H<VkDescriptorSetLayout>(2);return VK_SUCCESS;}
static VKAPI_ATTR void VKAPI_CALL DestroySet(VkDevice,VkDescriptorSetLayout,const VkAllocationCallbacks*){}
static VKAPI_ATTR VkResult VKAPI_CALL CreateTemplate(VkDevice,const VkDescriptorUpdateTemplateCreateInfo* ci,
    const VkAllocationCallbacks*,VkDescriptorUpdateTemplate* out) {
    ++created;
    assert(ci->templateType==VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_PUSH_DESCRIPTORS_KHR);
    assert(ci->pipelineLayout==H<VkPipelineLayout>(1) && ci->set==0);
    assert(ci->pipelineBindPoint==(expect_compute?VK_PIPELINE_BIND_POINT_COMPUTE:VK_PIPELINE_BIND_POINT_GRAPHICS));
    entries.assign(ci->pDescriptorUpdateEntries,ci->pDescriptorUpdateEntries+ci->descriptorUpdateEntryCount);
    if(fail_create) return VK_ERROR_OUT_OF_HOST_MEMORY;
    *out=H<VkDescriptorUpdateTemplate>(3);return VK_SUCCESS;
}
static VKAPI_ATTR void VKAPI_CALL DestroyTemplate(VkDevice,VkDescriptorUpdateTemplate,const VkAllocationCallbacks*){++destroyed;}
static VKAPI_ATTR void VKAPI_CALL PushTemplate(VkCommandBuffer,VkDescriptorUpdateTemplate update,
    VkPipelineLayout layout,uint32_t set,const void* payload) {
    ++pushed;assert(update==H<VkDescriptorUpdateTemplate>(3) && layout==H<VkPipelineLayout>(1) && set==0);
    assert(reinterpret_cast<uintptr_t>(payload)%8==0);
    for(const auto& e:entries) {
        const auto* data=static_cast<const std::byte*>(payload)+e.offset;
        if(e.descriptorType==VK_DESCRIPTOR_TYPE_STORAGE_BUFFER) {
            VkDescriptorBufferInfo info{};std::memcpy(&info,data,sizeof(info));
            observed=info.offset;assert(info.buffer==H<VkBuffer>(101) && info.range==512);
        } else {
            for(unsigned i=0;i<e.descriptorCount;++i) {
                VkDescriptorImageInfo info{};std::memcpy(&info,data+i*e.stride,sizeof(info));
                assert(info.imageView==H<VkImageView>(201+i));
            }
        }
    }
}
static VKAPI_ATTR void VKAPI_CALL PushConstants(VkCommandBuffer,VkPipelineLayout,VkShaderStageFlags,
    uint32_t,uint32_t size,const void* data){++constants;assert(size==128 && *static_cast<const u32*>(data)==37);}
static VKAPI_ATTR void VKAPI_CALL PushLegacy(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,
    uint32_t,uint32_t count,const VkWriteDescriptorSet* writes){++legacy;assert(count==expected_writes && writes[0].pBufferInfo);observed=writes[0].pBufferInfo->offset;}
static VKAPI_ATTR void VKAPI_CALL UpdateSets(VkDevice,uint32_t count,const VkWriteDescriptorSet*,uint32_t,const VkCopyDescriptorSet*){assert(count==expected_writes);}
static VKAPI_ATTR void VKAPI_CALL BindSets(VkCommandBuffer,VkPipelineBindPoint,VkPipelineLayout,uint32_t,
    uint32_t count,const VkDescriptorSet*,uint32_t,const uint32_t*){assert(count==1);++sets;}

namespace Vulkan {
constexpr auto AllGraphicsStageBits=vk::ShaderStageFlagBits::eVertex|vk::ShaderStageFlagBits::eFragment;
struct Instance {vk::Device GetDevice()const{return vk::Device{H<VkDevice>(1)};}};
struct Scheduler {
    bool deferred=true;
    bool switch_direct=false;
    std::vector<std::unique_ptr<std::byte[]>> storage;
    std::vector<std::function<void(vk::CommandBuffer)>> commands;
    bool IsRecordingDeferred()const{return deferred;}
    u64 CurrentTick()const noexcept{return 7;}
    u64 DescriptorStreamEpoch()const noexcept{return 11;}
    void ReserveRecordData(size_t){}
    template<class T> std::span<const T> RecordData(std::span<const T> data) {
        if(switch_direct)deferred=false; // ReserveRecordData may switch an oversized chunk to direct
        if(!deferred) return data;
        auto memory=std::make_unique<std::byte[]>(data.size_bytes());
        std::memcpy(memory.get(),data.data(),data.size_bytes());
        auto* ptr=reinterpret_cast<const T*>(memory.get());storage.push_back(std::move(memory));
        return {ptr,data.size()};
    }
    template<class F> void Record(F f) {
        if(deferred) commands.emplace_back(std::move(f));else f(vk::CommandBuffer{H<VkCommandBuffer>(1)});
    }
    void Drain(){for(auto& f:commands)f(vk::CommandBuffer{H<VkCommandBuffer>(1)});commands.clear();storage.clear();}
};
struct Heap {vk::DescriptorSet Commit(vk::DescriptorSetLayout)const{return vk::DescriptorSet{H<VkDescriptorSet>(7)};}};
struct Pipeline {
    Instance instance;Scheduler& scheduler;Heap desc_heap;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniqueDescriptorSetLayout desc_layout;
    DescriptorTemplateData descriptor_template_data;
    vk::UniqueDescriptorUpdateTemplate descriptor_update_template;
    PFN_vkCmdPushDescriptorSetWithTemplateKHR push_descriptor_template{};
    bool uses_push_descriptors=true,compute=false;
    using DescriptorWrites=std::vector<vk::WriteDescriptorSet>;
    explicit Pipeline(Scheduler& s):scheduler(s) {
        pipeline_layout=std::move(instance.GetDevice().createPipelineLayoutUnique({}).value);
        desc_layout=std::move(instance.GetDevice().createDescriptorSetLayoutUnique({}).value);
    }
    bool IsCompute()const{return compute;}
    void BuildDescriptorTemplate(std::span<const vk::DescriptorSetLayoutBinding>);
    void BindResources(DescriptorWrites&,u32,const Shader::PushData&)const;
};
// PRODUCTION_MEMO_HELPERS
// PRODUCTION_METHODS
}

int main(int argc,char** argv) {
    std::string mode=argc>1?argv[1]:"deferred";
    unsetenv("TLG_DESCRIPTOR_TEMPLATES");if(mode=="off"||mode=="offdirect")setenv("TLG_DESCRIPTOR_TEMPLATES","0",1);
    auto& d=VULKAN_HPP_DEFAULT_DISPATCHER;
    d.vkCreatePipelineLayout=CreateLayout;d.vkDestroyPipelineLayout=DestroyLayout;
    d.vkCreateDescriptorSetLayout=CreateSet;d.vkDestroyDescriptorSetLayout=DestroySet;
    d.vkCreateDescriptorUpdateTemplate=CreateTemplate;d.vkDestroyDescriptorUpdateTemplate=DestroyTemplate;
    d.vkCmdPushDescriptorSetWithTemplateKHR=mode=="missing"?nullptr:PushTemplate;
    d.vkCmdPushConstants=PushConstants;d.vkCmdPushDescriptorSetKHR=PushLegacy;
    d.vkUpdateDescriptorSets=UpdateSets;d.vkCmdBindDescriptorSets=BindSets;
    fail_create=mode=="failure";
    expect_compute=mode=="compute";
    Vulkan::Scheduler scheduler;scheduler.deferred=mode!="direct"&&mode!="offdirect";
    scheduler.switch_direct=mode=="switchdirect";
    {
        Vulkan::Pipeline pipeline{scheduler};pipeline.uses_push_descriptors=mode!="ordinary";
        pipeline.compute=expect_compute;
        std::array<vk::DescriptorSetLayoutBinding,2> bindings{{
            {.binding=0,.descriptorType=vk::DescriptorType::eStorageBuffer,.descriptorCount=1},
            {.binding=3,.descriptorType=vk::DescriptorType::eSampledImage,.descriptorCount=2}}};
        if(mode=="unsupported")bindings[1].descriptorType=vk::DescriptorType::eUniformBufferDynamic;
        pipeline.BuildDescriptorTemplate(mode=="empty"?std::span<const vk::DescriptorSetLayoutBinding>{}:std::span<const vk::DescriptorSetLayoutBinding>{bindings});
        vk::DescriptorBufferInfo buffer{.buffer=vk::Buffer{H<VkBuffer>(101)},.offset=123,.range=512};
        std::array<vk::DescriptorImageInfo,2> images{{
            {.imageView=vk::ImageView{H<VkImageView>(201)},.imageLayout=vk::ImageLayout::eGeneral},
            {.imageView=vk::ImageView{H<VkImageView>(202)},.imageLayout=vk::ImageLayout::eGeneral}}};
        Vulkan::Pipeline::DescriptorWrites writes{
            {.dstBinding=0,.descriptorCount=1,.descriptorType=vk::DescriptorType::eStorageBuffer,.pBufferInfo=&buffer},
            {.dstBinding=3,.descriptorCount=2,.descriptorType=vk::DescriptorType::eSampledImage,.pImageInfo=images.data()}};
        if(mode=="mismatch")writes.pop_back();
        if(mode=="empty")writes.clear();
        Shader::PushData push;push.words[0]=37;
        expected_writes=static_cast<unsigned>(writes.size());
        const u32 active_count=static_cast<u32>(writes.size());
        // A smaller subsequent draw leaves old high-water entries in the pool. They must not
        // enter the template, legacy push, or ordinary descriptor update path.
        writes.resize(writes.size()+3);
        for(size_t i=active_count;i<writes.size();++i) {
            writes[i].descriptorCount=123;
            writes[i].pBufferInfo=reinterpret_cast<const vk::DescriptorBufferInfo*>(uintptr_t{1});
        }
        const auto high_water=writes.size();
        pipeline.BindResources(writes,active_count,push);
        assert(writes.size()==high_water);
        bool templated=mode=="deferred"||mode=="direct"||mode=="compute"||mode=="switchdirect";
        if(scheduler.deferred) {
            assert(pushed==0 && constants==0);
            assert(scheduler.commands.size()==(templated||mode=="empty"?1:2));
        }
        buffer.offset=999;images[0].imageView=vk::ImageView{H<VkImageView>(999)};push.words[0]=0;
        scheduler.Drain();
        assert(constants==1);
        assert(pushed==unsigned(templated));
        assert(legacy==unsigned(!templated && mode!="ordinary" && mode!="empty"));
        assert(sets==unsigned(mode=="ordinary"));
        if(mode!="ordinary" && mode!="empty")assert(observed==123);
        assert(IssBionicPerf::descriptor_template_calls==unsigned(templated));
        assert(IssBionicPerf::descriptor_legacy_calls==unsigned(!templated && mode!="empty"));
    }
    assert(destroyed==unsigned(created && !fail_create));
}
