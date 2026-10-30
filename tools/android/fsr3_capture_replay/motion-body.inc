    if(argc<4)return 2;
    const std::filesystem::path capture=argv[1],output=argv[2];
    std::filesystem::create_directories(output);
    VkQueue queue{};vkGetDeviceQueue(device,choice.queueFamily,0,&queue);
    std::array<TestImage,3> images{};
    const VkImageUsageFlags usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if(!createImage(choice.device,device,VK_FORMAT_R32_SFLOAT,640,360,usage,&images[0])||
       !createImage(choice.device,device,VK_FORMAT_R16G16_SFLOAT,640,360,usage,&images[1])||
       !createImage(choice.device,device,VK_FORMAT_R32G32B32A32_SFLOAT,640,360,usage,&images[2]))return 3;
COMMON_TRANSFER
    std::array<VkImageView,3> views{};
    for(int i=0;i<3;++i){VkImageViewCreateInfo v{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        v.image=images[i].image;v.viewType=VK_IMAGE_VIEW_TYPE_2D;v.format=images[i].createInfo.format;
        v.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;v.subresourceRange.levelCount=1;v.subresourceRange.layerCount=1;
        check(vkCreateImageView(device,&v,nullptr,&views[i]));}
    const VkDescriptorSetLayoutBinding bindings[]{{0,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {1,VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr},
        {2,VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr}};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};dl.bindingCount=3;dl.pBindings=bindings;
    VkDescriptorSetLayout descriptorLayout{};check(vkCreateDescriptorSetLayout(device,&dl,nullptr,&descriptorLayout));
    struct Push{float reproj[12]{},proj[4]{},previous[4]{},size[2]{640,360},jitter[2]{},guest[2]{};uint32_t mode=3;};
    static_assert(sizeof(Push)==108);
    VkPushConstantRange range{VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(Push)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};pl.setLayoutCount=1;pl.pSetLayouts=&descriptorLayout;pl.pushConstantRangeCount=1;pl.pPushConstantRanges=&range;
    VkPipelineLayout layout{};check(vkCreatePipelineLayout(device,&pl,nullptr,&layout));
    auto spirvBytes=readFile(argv[3]);std::vector<uint32_t> words(spirvBytes.size()/4);std::memcpy(words.data(),spirvBytes.data(),spirvBytes.size());
    VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};sm.codeSize=spirvBytes.size();sm.pCode=words.data();
    VkShaderModule shader{};check(vkCreateShaderModule(device,&sm,nullptr,&shader));
    VkComputePipelineCreateInfo cp{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};cp.stage.sType=VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cp.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;cp.stage.module=shader;cp.stage.pName="main";cp.layout=layout;
    VkPipeline pipeline{};check(vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&cp,nullptr,&pipeline));
    const VkDescriptorPoolSize sizes[]{{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,2},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1}};
    VkDescriptorPoolCreateInfo pc2{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pc2.maxSets=1;pc2.poolSizeCount=2;pc2.pPoolSizes=sizes;
    VkDescriptorPool descriptors{};check(vkCreateDescriptorPool(device,&pc2,nullptr,&descriptors));
    VkDescriptorSetAllocateInfo sa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};sa.descriptorPool=descriptors;sa.descriptorSetCount=1;sa.pSetLayouts=&descriptorLayout;
    VkDescriptorSet set{};check(vkAllocateDescriptorSets(device,&sa,&set));
    std::array<VkDescriptorImageInfo,3> infos{};std::array<VkWriteDescriptorSet,3> writes{};
    for(int i=0;i<3;++i){infos[i].imageView=views[i];infos[i].imageLayout=i==1?VK_IMAGE_LAYOUT_GENERAL:VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=set;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=bindings[i].descriptorType;writes[i].pImageInfo=&infos[i];}
    vkUpdateDescriptorSets(device,3,writes.data(),0,nullptr);
    for(int frame=0;frame<2;++frame){
        const std::string prefix=frame?"f001":"f000";
        const std::array<std::filesystem::path,2> files{capture/(prefix+"_depth_640x360_f32.raw"),capture/(prefix+"_object_motion_640x360_rgba32f.raw")};
        for(int at=0;at<2;++at){const int i=at?2:0;auto bytes=readFile(files[at]);
            transfer(images[i].image,640,360,layouts[i],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,bytes,true);layouts[i]=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;}
        auto metadata=readInfo(capture/(prefix+"_aa_info.txt"));auto jitter=pairValue(metadata.at("jitter_render_pixels"));
        Push push;push.reproj[0]=push.reproj[5]=push.reproj[10]=push.proj[3]=1;push.jitter[0]=jitter[0];push.jitter[1]=jitter[1];
        begin();if(!frame){barrier(images[1].image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);layouts[1]=VK_IMAGE_LAYOUT_GENERAL;}
        vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
        vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),&push);vkCmdDispatch(cmd,80,45,1);submit();
        std::vector<uint8_t> bytes(640*360*4);transfer(images[1].image,640,360,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,bytes,false);
        writeFile(output/(prefix+"_motion_640x360_rg16f.raw"),bytes);
    }
    vkDeviceWaitIdle(device);vkDestroyPipeline(device,pipeline,nullptr);vkDestroyShaderModule(device,shader,nullptr);
    vkDestroyPipelineLayout(device,layout,nullptr);vkDestroyDescriptorPool(device,descriptors,nullptr);vkDestroyDescriptorSetLayout(device,descriptorLayout,nullptr);
    for(auto view:views)vkDestroyImageView(device,view,nullptr);for(auto& image:images)destroyImage(device,&image);
    vkUnmapMemory(device,staging.memory);destroyReadbackBuffer(device,&staging);vkDestroyCommandPool(device,pool,nullptr);
    vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);return 0;
}
