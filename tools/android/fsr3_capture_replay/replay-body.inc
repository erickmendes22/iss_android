    if(argc<3){std::fprintf(stderr,"replay capture_dir output_dir [motion_dir]\n");return 2;}
    const std::filesystem::path capture=argv[1],output=argv[2];
    const std::filesystem::path motionRoot=argc>3?argv[3]:argv[1];
    std::filesystem::create_directories(output);
    VkQueue queue{};vkGetDeviceQueue(device,choice.queueFamily,0,&queue);
    FfxVkPortableDeviceInfo owner{};owner.structSize=sizeof(owner);
    owner.instance=instance;owner.physicalDevice=choice.device;owner.device=device;
    owner.getDeviceProcAddr=vkGetDeviceProcAddr;owner.getInstanceProcAddr=vkGetInstanceProcAddr;
    owner.queue=queue;owner.queueFamilyIndex=choice.queueFamily;
    owner.shaderFloat16Enabled=enabledShaderFloat16;
    owner.shaderStorageBufferArrayNonUniformIndexingEnabled=enabledStorageBufferNonUniformIndexing;
    owner.shaderStorageImageWriteWithoutFormatEnabled=enabledStorageImageWriteWithoutFormat;
    FfxVkPortableUpscaleCreateInfo desc{};desc.structSize=sizeof(desc);
    desc.flags=FFX_VK_PORTABLE_CONTEXT_DEPTH_INVERTED|FFX_VK_PORTABLE_CONTEXT_AUTO_EXPOSURE;
    desc.maxRenderSize={640,360};desc.maxOutputSize={1280,720};
    FfxVkPortableUpscaleContext* context{};
    const auto result=ffxVkPortableUpscaleContextCreate(&owner,&desc,&context);
    if(result!=FFX_VK_PORTABLE_OK){std::fprintf(stderr,"context=%d\n",int(result));return 3;}
    std::fprintf(stderr,"Replay GPU: %s, float16=%u\n",properties.deviceName,enabledShaderFloat16);
    std::array<TestImage,4> images{};
    const VkImageUsageFlags usage=VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_STORAGE_BIT|
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if(!createImage(choice.device,device,VK_FORMAT_R8G8B8A8_UNORM,640,360,usage,&images[0])||
       !createImage(choice.device,device,VK_FORMAT_R32_SFLOAT,640,360,usage,&images[1])||
       !createImage(choice.device,device,VK_FORMAT_R16G16_SFLOAT,640,360,usage,&images[2])||
       !createImage(choice.device,device,VK_FORMAT_R8G8B8A8_UNORM,1280,720,usage,&images[3]))return 4;
    ReadbackBuffer staging{};
    if(!createReadbackBuffer(choice.device,device,32ull*1024*1024,&staging))return 5;
    void* mapped{};if(vkMapMemory(device,staging.memory,0,VK_WHOLE_SIZE,0,&mapped)!=VK_SUCCESS)return 6;
    VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pc.queueFamilyIndex=choice.queueFamily;pc.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool{};if(vkCreateCommandPool(device,&pc,nullptr,&pool)!=VK_SUCCESS)return 7;
    VkCommandBufferAllocateInfo ac{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ac.commandPool=pool;ac.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;ac.commandBufferCount=1;
    VkCommandBuffer cmd{};if(vkAllocateCommandBuffers(device,&ac,&cmd)!=VK_SUCCESS)return 8;
    auto begin=[&]{vkResetCommandBuffer(cmd,0);VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        b.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;check(vkBeginCommandBuffer(cmd,&b));};
    auto submit=[&]{check(vkEndCommandBuffer(cmd));VkSubmitInfo s{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        s.commandBufferCount=1;s.pCommandBuffers=&cmd;check(vkQueueSubmit(queue,1,&s,VK_NULL_HANDLE));check(vkQueueWaitIdle(queue));};
    auto barrier=[&](VkImage image,VkImageLayout from,VkImageLayout to){
        auto b=imageBarrier(image,from,to,from==VK_IMAGE_LAYOUT_UNDEFINED?0:VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT,
            VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT);
        vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,0,nullptr,0,nullptr,1,&b);};
    auto transfer=[&](VkImage image,uint32_t w,uint32_t h,VkImageLayout old,VkImageLayout restore,
                      std::vector<uint8_t>& bytes,bool upload){
        if(upload){std::memcpy(mapped,bytes.data(),bytes.size());
            if(!(staging.memoryProperties&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){
                VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};r.memory=staging.memory;r.size=VK_WHOLE_SIZE;check(vkFlushMappedMemoryRanges(device,1,&r));}}
        begin();auto layout=upload?VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier(image,old,layout);VkBufferImageCopy c{};c.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;
        c.imageSubresource.layerCount=1;c.imageExtent={w,h,1};
        if(upload)vkCmdCopyBufferToImage(cmd,staging.buffer,image,layout,1,&c);
        else vkCmdCopyImageToBuffer(cmd,image,layout,staging.buffer,1,&c);
        barrier(image,layout,restore);submit();
        if(!upload){if(!(staging.memoryProperties&VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)){
                VkMappedMemoryRange r{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};r.memory=staging.memory;r.size=VK_WHOLE_SIZE;check(vkInvalidateMappedMemoryRanges(device,1,&r));}
            std::memcpy(bytes.data(),mapped,bytes.size());}
    };
    std::array<VkImageLayout,4> layouts{};
    for(int frame=0;frame<2;++frame){
        const std::string prefix=frame?"f001":"f000";
        const auto info=readInfo(capture/(prefix+"_fsr_info.txt"));
        const std::array<std::filesystem::path,3> files{
            capture/(prefix+"_input_640x360_rgba.raw"),capture/(prefix+"_depth_640x360_f32.raw"),
            motionRoot/(prefix+"_motion_640x360_rg16f.raw")};
        for(int i=0;i<3;++i){auto bytes=readFile(files[i]);if(bytes.size()!=640*360*4)std::abort();
            transfer(images[i].image,640,360,layouts[i],VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,bytes,true);
            layouts[i]=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;}
        begin();if(!frame){barrier(images[3].image,VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL);layouts[3]=VK_IMAGE_LAYOUT_GENERAL;}
        FfxVkPortableUpscaleDispatchInfo d{};d.structSize=sizeof(d);d.commandBuffer=cmd;
        d.color=asPortableImage(images[0],FFX_VK_PORTABLE_RESOURCE_STATE_COMPUTE_READ);
        d.depth=asPortableImage(images[1],FFX_VK_PORTABLE_RESOURCE_STATE_COMPUTE_READ);
        d.motionVectors=asPortableImage(images[2],FFX_VK_PORTABLE_RESOURCE_STATE_COMPUTE_READ);
        d.output=asPortableImage(images[3],FFX_VK_PORTABLE_RESOURCE_STATE_UNORDERED_ACCESS);
        d.exposure.structSize=sizeof(d.exposure);d.reactiveMask.structSize=sizeof(d.reactiveMask);
        d.transparencyAndCompositionMask.structSize=sizeof(d.transparencyAndCompositionMask);
        auto jitter=pairValue(info.at("sdk_jitter"));d.jitterOffset={jitter[0],jitter[1]};
        d.motionVectorScale={1,1};d.renderSize={640,360};d.outputSize={1280,720};
        d.frameTimeMilliseconds=std::stof(info.at("frame_time_ms"));d.preExposure=1;
        d.cameraNear=10;d.cameraFar=10000000;d.cameraVerticalFovRadians=1;d.viewSpaceToMeters=1;
        d.sharpness=.8f;d.enableSharpening=VK_TRUE;d.reset=frame?VK_FALSE:VK_TRUE;d.frameId=frame;
        const auto status=ffxVkPortableUpscaleContextRecordDispatch(context,&d);
        if(status!=FFX_VK_PORTABLE_OK){std::fprintf(stderr,"dispatch=%d\n",int(status));return 9;}
        submit();
        FfxVkPortableUpscaleDebugInfo debug{};debug.structSize=sizeof(debug);
        if(ffxVkPortableUpscaleContextGetDebugInfo(context,&debug)!=FFX_VK_PORTABLE_OK)return 10;
        if(!frame){
            const std::array<std::string,3> names{"fsr_before_rcas_1280x720_rgba16f.raw",
                "fsr_accumulation_640x360_r8.raw","fsr_reactivity_640x360_rgba.raw"};
            for(int i=1;i<4;++i){auto bytes=readFile(capture/(prefix+"_"+names[i-1]));auto image=debug.images[i];
                transfer(image.image,image.width,image.height,image.layout,image.layout,bytes,true);}
            replaySetFrameIndex(context,std::stof(info.at("sdk_history_frame_index")));
        }else{
            std::vector<uint8_t> bytes(1280*720*4);transfer(images[3].image,1280,720,VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_GENERAL,bytes,false);
            writeFile(output/"f001_output_1280x720_rgba.raw",bytes);
            bytes.resize(1280*720*8);auto image=debug.images[1];transfer(image.image,1280,720,image.layout,image.layout,bytes,false);
            writeFile(output/"f001_before_rcas_1280x720_rgba16f.raw",bytes);
            std::printf("history_index=%.0f phase=%.0f jitter=%g,%g\n",debug.frameIndex,debug.jitterPhaseCount,debug.jitter[0],debug.jitter[1]);
        }
    }
    vkDeviceWaitIdle(device);ffxVkPortableUpscaleContextDestroy(context);
    for(auto& image:images)destroyImage(device,&image);
    vkUnmapMemory(device,staging.memory);destroyReadbackBuffer(device,&staging);
    vkDestroyCommandPool(device,pool,nullptr);vkDestroyDevice(device,nullptr);vkDestroyInstance(instance,nullptr);
    return 0;
}
