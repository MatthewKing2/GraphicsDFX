#include "gpu.hpp"
#include "random_source.hpp"
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
constexpr std::uint32_t imageSize=768, gridSize=512, queryCount=8;
void check(VkResult result,const char* action) {
    if(result!=VK_SUCCESS) throw std::runtime_error(std::string(action)+" failed (VkResult "+std::to_string(result)+")");
}
struct Buffer {VkBuffer handle=VK_NULL_HANDLE;VkDeviceMemory memory=VK_NULL_HANDLE;VkDeviceSize size=0;};
struct Push {
    std::uint32_t count,blocks,epoch,seed,workload,mode,visualSamples;
    float stepSize;
};
static_assert(sizeof(Push)==32);
}

struct Gpu::Impl {
    Options o;
    GLFWwindow* window=nullptr;
    bool glfwReady=false;
    VkInstance instance=VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger=VK_NULL_HANDLE;
    VkPhysicalDevice physical=VK_NULL_HANDLE;
    VkPhysicalDeviceProperties properties{};
    VkDevice device=VK_NULL_HANDLE;
    VkQueue queue=VK_NULL_HANDLE;
    std::uint32_t family=0,validBits=0;
    std::string name;
    std::atomic<unsigned> errors{0};
    VkSurfaceKHR surface=VK_NULL_HANDLE;
    VkSwapchainKHR swapchain=VK_NULL_HANDLE;
    VkFormat swapFormat=VK_FORMAT_UNDEFINED;
    VkExtent2D swapExtent{};
    std::vector<VkImage> swapImages;
    std::vector<VkSemaphore> presentReady;
    VkSemaphore acquired=VK_NULL_HANDLE;
    VkCommandPool commandPool=VK_NULL_HANDLE;
    VkCommandBuffer command=VK_NULL_HANDLE;
    VkFence fence=VK_NULL_HANDLE;
    VkQueryPool queries=VK_NULL_HANDLE;
    Buffer randoms,states,pixels,timestampScratch;
    Buffer inputStaging;
    void* inputMapped=nullptr;
    std::vector<std::uint8_t> inputStorage;
    void* inputAligned=nullptr;
    std::unique_ptr<rnglab::RandomStream> inputStream;
    VkDescriptorSetLayout descriptorLayout=VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool=VK_NULL_HANDLE;
    VkDescriptorSet descriptors=VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout=VK_NULL_HANDLE;
    VkPipeline rngPipeline=VK_NULL_HANDLE,simulationPipeline=VK_NULL_HANDLE,visualPipeline=VK_NULL_HANDLE,drawPipeline=VK_NULL_HANDLE;
    VkImage image=VK_NULL_HANDLE;
    VkDeviceMemory imageMemory=VK_NULL_HANDLE;
    VkImageView imageView=VK_NULL_HANDLE;
    VkRenderPass renderPass=VK_NULL_HANDLE;
    VkFramebuffer framebuffer=VK_NULL_HANDLE;

    explicit Impl(const Options& options):o(options) {
        try {initialize();} catch(...) {cleanup();throw;}
    }
    ~Impl() {cleanup();}
    static VKAPI_ATTR VkBool32 VKAPI_CALL debug(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT,const VkDebugUtilsMessengerCallbackDataEXT* data,void* user) {
        auto* self=static_cast<Impl*>(user);
        if(severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++self->errors;
        std::cerr << "Vulkan validation: " << data->pMessage << '\n';
        return VK_FALSE;
    }
    void initialize() {
        std::vector<const char*> extensions;
        if(!o.headless) {
            glfwSetErrorCallback([](int,const char* msg){std::cerr << "GLFW: " << msg << '\n';});
#if GLFW_VERSION_MAJOR > 3 || (GLFW_VERSION_MAJOR == 3 && GLFW_VERSION_MINOR >= 4)
            glfwInitVulkanLoader(vkGetInstanceProcAddr);
#endif
            if(!glfwInit()) throw std::runtime_error("GLFW initialization failed; use --headless for profiling without a display");
            glfwReady=true;
            glfwWindowHint(GLFW_CLIENT_API,GLFW_NO_API);
            window=glfwCreateWindow(imageSize,imageSize,o.workload?"Random-walk diffusion":"Monte Carlo pi",nullptr,nullptr);
            if(!window) throw std::runtime_error("GLFW window creation failed");
            std::uint32_t n=0;auto required=glfwGetRequiredInstanceExtensions(&n);
            if(!required) throw std::runtime_error("GLFW Vulkan surface extensions unavailable");
            extensions.assign(required,required+n);
        }
        std::uint32_t extensionCount=0;
        check(vkEnumerateInstanceExtensionProperties(nullptr,&extensionCount,nullptr),"enumerate instance extensions");
        std::vector<VkExtensionProperties> available(extensionCount);
        check(vkEnumerateInstanceExtensionProperties(nullptr,&extensionCount,available.data()),"enumerate instance extensions");
        auto hasInstanceExtension=[&](const char* s){return std::any_of(available.begin(),available.end(),[&](auto& p){return std::strcmp(p.extensionName,s)==0;});};
        VkInstanceCreateFlags flags=0;
#ifdef VK_KHR_portability_enumeration
        if(hasInstanceExtension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            flags|=VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }
#else
        (void)hasInstanceExtension;
#endif
        extensions.push_back(VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
        const char* layer="VK_LAYER_KHRONOS_validation";
        if(o.validation) {
            std::uint32_t n=0;check(vkEnumerateInstanceLayerProperties(&n,nullptr),"enumerate layers");
            std::vector<VkLayerProperties> layers(n);check(vkEnumerateInstanceLayerProperties(&n,layers.data()),"enumerate layers");
            if(!std::any_of(layers.begin(),layers.end(),[&](auto& p){return std::strcmp(p.layerName,layer)==0;}))
                throw std::runtime_error("Requested VK_LAYER_KHRONOS_validation is unavailable");
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
        }
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName="GraphicsDFX RNG laboratory";app.applicationVersion=VK_MAKE_VERSION(1,0,0);app.apiVersion=VK_API_VERSION_1_1;
        VkDebugUtilsMessengerCreateInfoEXT debugInfo{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        debugInfo.messageSeverity=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        debugInfo.messageType=VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT|VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        debugInfo.pfnUserCallback=debug;debugInfo.pUserData=this;
        VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ci.flags=flags;ci.pApplicationInfo=&app;ci.enabledExtensionCount=static_cast<std::uint32_t>(extensions.size());ci.ppEnabledExtensionNames=extensions.data();
        const VkValidationFeatureEnableEXT syncValidation=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
        VkValidationFeaturesEXT validationFeatures{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
        validationFeatures.enabledValidationFeatureCount=1;validationFeatures.pEnabledValidationFeatures=&syncValidation;
        validationFeatures.pNext=&debugInfo;
        if(o.validation) {ci.enabledLayerCount=1;ci.ppEnabledLayerNames=&layer;ci.pNext=&validationFeatures;}
        check(vkCreateInstance(&ci,nullptr,&instance),"create Vulkan instance");
        if(o.validation) {
            auto create=reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkCreateDebugUtilsMessengerEXT"));
            if(!create) throw std::runtime_error("Debug utils extension unavailable");
            check(create(instance,&debugInfo,nullptr,&messenger),"create validation messenger");
        }
        if(window) check(glfwCreateWindowSurface(instance,window,nullptr,&surface),"create window surface");
        selectDevice();
        VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pool.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;pool.queueFamilyIndex=family;
        check(vkCreateCommandPool(device,&pool,nullptr,&commandPool),"create command pool");
        VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        alloc.commandPool=commandPool;alloc.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;alloc.commandBufferCount=1;
        check(vkAllocateCommandBuffers(device,&alloc,&command),"allocate command buffer");
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};check(vkCreateFence(device,&fci,nullptr,&fence),"create fence");
        VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};qci.queryType=VK_QUERY_TYPE_TIMESTAMP;qci.queryCount=queryCount;
        check(vkCreateQueryPool(device,&qci,nullptr,&queries),"create timestamp queries");
        const auto randomBytes=std::uint64_t(o.count)*o.blocks*16;
        if(randomBytes>properties.limits.maxStorageBufferRange && !o.mode)
            throw std::runtime_error("Random buffer exceeds device maxStorageBufferRange; reduce --count or --blocks");
        if(std::uint64_t(o.count)*16>properties.limits.maxStorageBufferRange)
            throw std::runtime_error("State buffer exceeds maxStorageBufferRange");
        if((std::uint64_t(o.count)+255)/256>properties.limits.maxComputeWorkGroupCount[0] || properties.limits.maxComputeWorkGroupInvocations<256 || properties.limits.maxComputeWorkGroupSize[0]<256)
            throw std::runtime_error("Workload exceeds compute workgroup limits");
        // Fused mode has a dummy binding only; its shader never accesses RNG storage.
        randoms=createBuffer(o.mode?16:randomBytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if(!o.rngStream.empty()) {
            // Read into ordinary aligned RAM: a PCIe driver need not support
            // pinning VkDeviceMemory mappings. Measure the extra host copy.
            inputStorage.resize(static_cast<std::size_t>(randomBytes)+4095);
            inputAligned=reinterpret_cast<void*>((reinterpret_cast<std::uintptr_t>(inputStorage.data())+4095)&~std::uintptr_t(4095));
            inputStaging=createBuffer(randomBytes,VK_BUFFER_USAGE_TRANSFER_SRC_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            check(vkMapMemory(device,inputStaging.memory,0,inputStaging.size,0,&inputMapped),"map external RNG staging");
            inputStream=std::make_unique<rnglab::RandomStream>(o.rngStream);
        }
        states=createBuffer(std::uint64_t(o.count)*16,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        pixels=createBuffer(gridSize*gridSize*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
#ifdef __APPLE__
        timestampScratch=createBuffer(16,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
#endif
        createDescriptors();
        rngPipeline=createCompute("rng.comp");
        simulationPipeline=createCompute("simulate.comp",true);
        visualPipeline=createCompute("visual.comp");
        if(o.visualize) createRendering();
        if(surface) createSwapchain();
    }
    void selectDevice() {
        std::uint32_t count=0;check(vkEnumeratePhysicalDevices(instance,&count,nullptr),"enumerate devices");
        std::vector<VkPhysicalDevice> devices(count);check(vkEnumeratePhysicalDevices(instance,&count,devices.data()),"enumerate devices");
        if(o.deviceIndex>=count) throw std::runtime_error("Vulkan --device index unavailable");
        physical=devices[o.deviceIndex];vkGetPhysicalDeviceProperties(physical,&properties);name=properties.deviceName;
        std::uint32_t n=0;vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,nullptr);
        std::vector<VkQueueFamilyProperties> families(n);vkGetPhysicalDeviceQueueFamilyProperties(physical,&n,families.data());
        bool found=false;
        for(std::uint32_t i=0;i<n;++i) {
            VkBool32 present=VK_TRUE;
            if(surface) check(vkGetPhysicalDeviceSurfaceSupportKHR(physical,i,surface,&present),"check presentation support");
            VkQueueFlags flags=VK_QUEUE_COMPUTE_BIT|VK_QUEUE_GRAPHICS_BIT;
            if((families[i].queueFlags&flags)==flags && present && families[i].timestampValidBits) {
                family=i;validBits=families[i].timestampValidBits;found=true;break;
            }
        }
        if(!found || properties.limits.timestampPeriod<=0)
            throw std::runtime_error("Device requires a graphics+compute queue with supported GPU timestamps");
        float priority=1;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex=family;qci.queueCount=1;qci.pQueuePriorities=&priority;
        std::vector<const char*> extensions;
        if(surface) extensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
        check(vkEnumerateDeviceExtensionProperties(physical,nullptr,&n,nullptr),"enumerate device extensions");
        std::vector<VkExtensionProperties> available(n);check(vkEnumerateDeviceExtensionProperties(physical,nullptr,&n,available.data()),"enumerate device extensions");
        for(auto& p:available) if(std::strcmp(p.extensionName,"VK_KHR_portability_subset")==0) extensions.push_back("VK_KHR_portability_subset");
        VkDeviceCreateInfo ci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        ci.queueCreateInfoCount=1;ci.pQueueCreateInfos=&qci;ci.enabledExtensionCount=static_cast<std::uint32_t>(extensions.size());ci.ppEnabledExtensionNames=extensions.data();
        check(vkCreateDevice(physical,&ci,nullptr,&device),"create device");vkGetDeviceQueue(device,family,0,&queue);
    }
    std::uint32_t memoryType(std::uint32_t bits,VkMemoryPropertyFlags flags) {
        VkPhysicalDeviceMemoryProperties memory{};vkGetPhysicalDeviceMemoryProperties(physical,&memory);
        for(std::uint32_t i=0;i<memory.memoryTypeCount;++i)
            if((bits&(1u<<i)) && (memory.memoryTypes[i].propertyFlags&flags)==flags) return i;
        throw std::runtime_error("Required Vulkan memory type unavailable");
    }
    Buffer createBuffer(VkDeviceSize size,VkBufferUsageFlags usage,VkMemoryPropertyFlags flags) {
        Buffer b;b.size=size;
        try {
            VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};ci.size=size;ci.usage=usage;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
            check(vkCreateBuffer(device,&ci,nullptr,&b.handle),"create buffer");
            VkMemoryRequirements requirements{};vkGetBufferMemoryRequirements(device,b.handle,&requirements);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=requirements.size;ai.memoryTypeIndex=memoryType(requirements.memoryTypeBits,flags);
            check(vkAllocateMemory(device,&ai,nullptr,&b.memory),"allocate buffer memory");
            check(vkBindBufferMemory(device,b.handle,b.memory,0),"bind buffer");return b;
        } catch(...) {destroyBuffer(b);throw;}
    }
    void destroyBuffer(Buffer& b) {
        if(b.handle) vkDestroyBuffer(device,b.handle,nullptr);
        if(b.memory) vkFreeMemory(device,b.memory,nullptr);b={};
    }
    void createDescriptors() {
        std::array<VkDescriptorSetLayoutBinding,3> bindings{};
        for(unsigned i=0;i<3;++i) {bindings[i].binding=i;bindings[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;bindings[i].descriptorCount=1;bindings[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_FRAGMENT_BIT;}
        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};ci.bindingCount=3;ci.pBindings=bindings.data();
        check(vkCreateDescriptorSetLayout(device,&ci,nullptr,&descriptorLayout),"create descriptor layout");
        VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3};
        VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};pool.maxSets=1;pool.poolSizeCount=1;pool.pPoolSizes=&size;
        check(vkCreateDescriptorPool(device,&pool,nullptr,&descriptorPool),"create descriptor pool");
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};ai.descriptorPool=descriptorPool;ai.descriptorSetCount=1;ai.pSetLayouts=&descriptorLayout;
        check(vkAllocateDescriptorSets(device,&ai,&descriptors),"allocate descriptors");
        Buffer buffers[]={randoms,states,pixels};
        std::array<VkDescriptorBufferInfo,3> infos{};std::array<VkWriteDescriptorSet,3> writes{};
        for(unsigned i=0;i<3;++i) {
            infos[i]={buffers[i].handle,0,buffers[i].size};writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet=descriptors;writes[i].dstBinding=i;writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;writes[i].pBufferInfo=&infos[i];
        }
        vkUpdateDescriptorSets(device,3,writes.data(),0,nullptr);
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(Push)};
        VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layout.setLayoutCount=1;layout.pSetLayouts=&descriptorLayout;layout.pushConstantRangeCount=1;layout.pPushConstantRanges=&push;
        check(vkCreatePipelineLayout(device,&layout,nullptr,&pipelineLayout),"create pipeline layout");
    }
    VkShaderModule shader(const std::string& file) {
        std::ifstream input(std::string(RNG_SHADER_DIR)+"/"+file+".spv",std::ios::binary|std::ios::ate);
        if(!input) throw std::runtime_error("Shader not found: "+file);
        auto bytes=input.tellg();if(bytes<=0 || bytes%4!=0) throw std::runtime_error("Invalid SPIR-V size");
        std::vector<std::uint32_t> code(static_cast<std::size_t>(bytes)/4);input.seekg(0);input.read(reinterpret_cast<char*>(code.data()),bytes);
        if(!input) throw std::runtime_error("Cannot read shader");
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};ci.codeSize=static_cast<std::size_t>(bytes);ci.pCode=code.data();
        VkShaderModule module;check(vkCreateShaderModule(device,&ci,nullptr,&module),"create shader module");return module;
    }
    VkPipeline createCompute(const std::string& file,bool specialize=false) {
        auto module=shader(file);
        const std::array<std::uint32_t,2> values{o.mode,o.workload};
        const std::array<VkSpecializationMapEntry,2> entries{{{0,0,sizeof(std::uint32_t)},{1,sizeof(std::uint32_t),sizeof(std::uint32_t)}}};
        VkSpecializationInfo info{2,entries.data(),sizeof(values),values.data()};
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.layout=pipelineLayout;ci.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};ci.stage.stage=VK_SHADER_STAGE_COMPUTE_BIT;ci.stage.module=module;ci.stage.pName="main";
        if(specialize) ci.stage.pSpecializationInfo=&info;
        VkPipeline pipeline=VK_NULL_HANDLE;auto result=vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&ci,nullptr,&pipeline);
        vkDestroyShaderModule(device,module,nullptr);check(result,"create compute pipeline");return pipeline;
    }
    void createRendering() {
        VkFormatProperties format{};vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_R8G8B8A8_UNORM,&format);
        if(!(format.optimalTilingFeatures&VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) throw std::runtime_error("Offscreen RGBA render target unsupported");
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};ci.imageType=VK_IMAGE_TYPE_2D;ci.format=VK_FORMAT_R8G8B8A8_UNORM;ci.extent={imageSize,imageSize,1};ci.mipLevels=1;ci.arrayLayers=1;ci.samples=VK_SAMPLE_COUNT_1_BIT;ci.tiling=VK_IMAGE_TILING_OPTIMAL;ci.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;ci.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateImage(device,&ci,nullptr,&image),"create offscreen image");
        VkMemoryRequirements req{};vkGetImageMemoryRequirements(device,image,&req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};ai.allocationSize=req.size;ai.memoryTypeIndex=memoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        check(vkAllocateMemory(device,&ai,nullptr,&imageMemory),"allocate image memory");check(vkBindImageMemory(device,image,imageMemory,0),"bind image");
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};vi.image=image;vi.viewType=VK_IMAGE_VIEW_TYPE_2D;vi.format=ci.format;vi.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        check(vkCreateImageView(device,&vi,nullptr,&imageView),"create offscreen view");
        VkAttachmentDescription attachment{};attachment.format=ci.format;attachment.samples=VK_SAMPLE_COUNT_1_BIT;attachment.loadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE;attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;attachment.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};VkSubpassDescription subpass{};subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS;subpass.colorAttachmentCount=1;subpass.pColorAttachments=&color;
        std::array<VkSubpassDependency,2> deps{};
        deps[0].srcSubpass=VK_SUBPASS_EXTERNAL;deps[0].dstSubpass=0;deps[0].srcStageMask=VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;deps[0].dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;deps[0].srcAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;deps[0].dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].srcSubpass=0;deps[1].dstSubpass=VK_SUBPASS_EXTERNAL;deps[1].srcStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;deps[1].dstStageMask=VK_PIPELINE_STAGE_TRANSFER_BIT;deps[1].srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;deps[1].dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
        VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};rp.attachmentCount=1;rp.pAttachments=&attachment;rp.subpassCount=1;rp.pSubpasses=&subpass;rp.dependencyCount=2;rp.pDependencies=deps.data();
        check(vkCreateRenderPass(device,&rp,nullptr,&renderPass),"create render pass");
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};fb.renderPass=renderPass;fb.attachmentCount=1;fb.pAttachments=&imageView;fb.width=imageSize;fb.height=imageSize;fb.layers=1;
        check(vkCreateFramebuffer(device,&fb,nullptr,&framebuffer),"create framebuffer");
        auto vert=shader("display.vert");VkShaderModule frag=VK_NULL_HANDLE;
        try {
            frag=shader("display.frag");
            std::array<VkPipelineShaderStageCreateInfo,2> stages{};
            for(auto& s:stages) {s={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};s.pName="main";}
            stages[0].stage=VK_SHADER_STAGE_VERTEX_BIT;stages[0].module=vert;stages[1].stage=VK_SHADER_STAGE_FRAGMENT_BIT;stages[1].module=frag;
            VkPipelineVertexInputStateCreateInfo vertex{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkViewport viewport{0,0,float(imageSize),float(imageSize),0,1};VkRect2D scissor{{0,0},{imageSize,imageSize}};
            VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};vp.viewportCount=1;vp.pViewports=&viewport;vp.scissorCount=1;vp.pScissors=&scissor;
            VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};raster.polygonMode=VK_POLYGON_MODE_FILL;raster.cullMode=VK_CULL_MODE_NONE;raster.frontFace=VK_FRONT_FACE_COUNTER_CLOCKWISE;raster.lineWidth=1;
            VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};ms.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
            VkPipelineColorBlendAttachmentState blend{};blend.colorWriteMask=VK_COLOR_COMPONENT_R_BIT|VK_COLOR_COMPONENT_G_BIT|VK_COLOR_COMPONENT_B_BIT|VK_COLOR_COMPONENT_A_BIT;
            VkPipelineColorBlendStateCreateInfo bs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};bs.attachmentCount=1;bs.pAttachments=&blend;
            VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};pci.stageCount=2;pci.pStages=stages.data();pci.pVertexInputState=&vertex;pci.pInputAssemblyState=&assembly;pci.pViewportState=&vp;pci.pRasterizationState=&raster;pci.pMultisampleState=&ms;pci.pColorBlendState=&bs;pci.layout=pipelineLayout;pci.renderPass=renderPass;
            check(vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&pci,nullptr,&drawPipeline),"create display pipeline");
        } catch(...) {vkDestroyShaderModule(device,vert,nullptr);if(frag)vkDestroyShaderModule(device,frag,nullptr);throw;}
        vkDestroyShaderModule(device,vert,nullptr);vkDestroyShaderModule(device,frag,nullptr);
    }
    void destroySwapchain() {
        for(auto s:presentReady) vkDestroySemaphore(device,s,nullptr);presentReady.clear();
        if(swapchain) vkDestroySwapchainKHR(device,swapchain,nullptr);swapchain=VK_NULL_HANDLE;swapImages.clear();
    }
    void createSwapchain() {
        check(vkDeviceWaitIdle(device),"wait before swapchain recreation");destroySwapchain();
        VkSurfaceCapabilitiesKHR caps{};check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical,surface,&caps),"get surface capabilities");
        if(!(caps.supportedUsageFlags&VK_IMAGE_USAGE_TRANSFER_DST_BIT)) throw std::runtime_error("Window surface does not support transfer-destination images; use --headless");
        std::uint32_t count=0;check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&count,nullptr),"get surface formats");
        std::vector<VkSurfaceFormatKHR> formats(count);check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical,surface,&count,formats.data()),"get surface formats");
        if(formats.empty()) throw std::runtime_error("No surface formats");
        auto selected=formats[0];
        for(auto f:formats) if(f.format==VK_FORMAT_B8G8R8A8_UNORM || f.format==VK_FORMAT_R8G8B8A8_UNORM) {selected=f;break;}
        swapFormat=selected.format;
        VkFormatProperties source{},target{};vkGetPhysicalDeviceFormatProperties(physical,VK_FORMAT_R8G8B8A8_UNORM,&source);vkGetPhysicalDeviceFormatProperties(physical,swapFormat,&target);
        if(!(source.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_SRC_BIT)||!(target.optimalTilingFeatures&VK_FORMAT_FEATURE_BLIT_DST_BIT))
            throw std::runtime_error("Surface format cannot blit offscreen visualization; use --headless");
        if(caps.currentExtent.width!=UINT32_MAX) swapExtent=caps.currentExtent;
        else {
            int w,h;glfwGetFramebufferSize(window,&w,&h);
            swapExtent={std::clamp(static_cast<std::uint32_t>(std::max(w,1)),caps.minImageExtent.width,caps.maxImageExtent.width),std::clamp(static_cast<std::uint32_t>(std::max(h,1)),caps.minImageExtent.height,caps.maxImageExtent.height)};
        }
        count=caps.minImageCount+1;if(caps.maxImageCount)count=std::min(count,caps.maxImageCount);
        VkCompositeAlphaFlagBitsKHR alpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        for(auto candidate:{VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR}) if(caps.supportedCompositeAlpha&candidate) {alpha=candidate;break;}
        VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};ci.surface=surface;ci.minImageCount=count;ci.imageFormat=swapFormat;ci.imageColorSpace=selected.colorSpace;ci.imageExtent=swapExtent;ci.imageArrayLayers=1;ci.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;ci.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE;ci.preTransform=caps.currentTransform;ci.compositeAlpha=alpha;ci.presentMode=VK_PRESENT_MODE_FIFO_KHR;ci.clipped=VK_TRUE;
        check(vkCreateSwapchainKHR(device,&ci,nullptr,&swapchain),"create swapchain");
        check(vkGetSwapchainImagesKHR(device,swapchain,&count,nullptr),"get swapchain images");swapImages.resize(count);check(vkGetSwapchainImagesKHR(device,swapchain,&count,swapImages.data()),"get swapchain images");
        VkSemaphoreCreateInfo sci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        presentReady.resize(count,VK_NULL_HANDLE);for(auto& s:presentReady)check(vkCreateSemaphore(device,&sci,nullptr,&s),"create present semaphore");
        if(!acquired)check(vkCreateSemaphore(device,&sci,nullptr,&acquired),"create acquire semaphore");
    }
    void begin() {
        check(vkResetCommandBuffer(command,0),"reset command buffer");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};bi.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(command,&bi),"begin command buffer");
    }
    void barrier() {
        // Deliberately serialize pass scopes for attributable timestamp intervals.
        VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};b.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT;b.dstAccessMask=VK_ACCESS_MEMORY_READ_BIT|VK_ACCESS_MEMORY_WRITE_BIT;
        vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,0,1,&b,0,nullptr,0,nullptr);
    }
    void dispatch(VkPipeline pipeline,std::uint32_t count) {
        vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);vkCmdDispatch(command,(count+255)/256,1,1);
    }
    void stamp(std::uint32_t index,bool end=false) {
        vkCmdWriteTimestamp(command,end?VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT:VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,queries,index);
    }
    void timestampBoundary(std::uint32_t slot) {
#ifdef __APPLE__
        // MoltenVK 1.4.2 defers Apple tile-GPU timestamps to encoder ends.
        // A real compute->blit switch flushes pending end/start samples BEFORE
        // the next pass. Each scope includes its tiny sentinel/sampling cost.
        // See MVKCommandBuffer.mm: encodeTimestampStageCounterSamples().
        // vkCmdFillBuffer uses a compute kernel in MoltenVK; UpdateBuffer uses
        // a blit encoder, which is the boundary needed here.
        constexpr std::uint32_t zero=0;
        vkCmdUpdateBuffer(command,timestampScratch.handle,slot*4,sizeof(zero),&zero);
#else
        (void)slot;
#endif
    }
    void submit(bool present=false,std::uint32_t imageIndex=0) {
        check(vkEndCommandBuffer(command),"end command buffer");check(vkResetFences(device,1,&fence),"reset fence");
        VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};si.commandBufferCount=1;si.pCommandBuffers=&command;
        if(present) {si.waitSemaphoreCount=1;si.pWaitSemaphores=&acquired;si.pWaitDstStageMask=&waitStage;si.signalSemaphoreCount=1;si.pSignalSemaphores=&presentReady[imageIndex];}
        check(vkQueueSubmit(queue,1,&si,fence),"submit GPU work");
        check(vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX),"wait for GPU frame");
        if(present) {
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};pi.waitSemaphoreCount=1;pi.pWaitSemaphores=&presentReady[imageIndex];pi.swapchainCount=1;pi.pSwapchains=&swapchain;pi.pImageIndices=&imageIndex;
            auto result=vkQueuePresentKHR(queue,&pi);
            if(result==VK_ERROR_OUT_OF_DATE_KHR || result==VK_SUBOPTIMAL_KHR) createSwapchain();else check(result,"present");
        }
    }
    Timing frame(std::uint32_t epoch) {
        const auto start=std::chrono::steady_clock::now();
        const Push push{o.count,o.blocks,epoch,o.seed,o.workload,o.mode,o.visualSamples,o.stepSize};
        double inputMs=0,stagingMs=0;
        if(inputStream) {
            inputStream->readFrame(inputAligned,static_cast<std::size_t>(inputStaging.size),o.count,o.blocks,epoch,o.seed,o.workload);
            const auto readDone=std::chrono::steady_clock::now();
            std::memcpy(inputMapped,inputAligned,static_cast<std::size_t>(inputStaging.size));
            const auto staged=std::chrono::steady_clock::now();
            inputMs=std::chrono::duration<double,std::milli>(readDone-start).count();
            stagingMs=std::chrono::duration<double,std::milli>(staged-readDone).count();
        }
        if(o.isolate==2) {
            // Generate this epoch's exact inputs before the measured command.
            // A separate submission and fence keep RNG work outside every
            // timestamp scope, while retaining the normal consumer's streams.
            begin();
            vkCmdPushConstants(command,pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),&push);
            vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&descriptors,0,nullptr);
            barrier();dispatch(rngPipeline,o.count);barrier();submit();
        }
        std::uint32_t imageIndex=0;
        bool present=window && o.visualize;
        if(present) {
            for(;;) {
                auto result=vkAcquireNextImageKHR(device,swapchain,UINT64_MAX,acquired,VK_NULL_HANDLE,&imageIndex);
                if(result==VK_ERROR_OUT_OF_DATE_KHR) {createSwapchain();continue;}
                if(result!=VK_SUBOPTIMAL_KHR) check(result,"acquire swapchain image");break;
            }
        }
        begin();vkCmdResetQueryPool(command,queries,0,queryCount);
        vkCmdPushConstants(command,pipelineLayout,VK_SHADER_STAGE_COMPUTE_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(push),&push);
        vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelineLayout,0,1,&descriptors,0,nullptr);
        barrier();stamp(0);
        if(inputStream) {
            VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
            host.srcAccessMask=VK_ACCESS_HOST_WRITE_BIT;host.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
            host.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;host.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            host.buffer=inputStaging.handle;host.size=inputStaging.size;
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,1,&host,0,nullptr);
            const VkBufferCopy copy{0,0,inputStaging.size};
            vkCmdCopyBuffer(command,inputStaging.handle,randoms.handle,1,&copy);
        } else if(!o.mode && o.isolate!=2) dispatch(rngPipeline,o.count);
        stamp(1,true);barrier();stamp(2);timestampBoundary(0);
        if(o.isolate!=1) dispatch(simulationPipeline,o.count);
        stamp(3,true);barrier();stamp(4);timestampBoundary(1);
        if(o.visualize) {
            vkCmdFillBuffer(command,pixels.handle,0,pixels.size,0);barrier();
            dispatch(visualPipeline,std::min(o.count,o.visualSamples));
        }
        stamp(5,true);barrier();stamp(6);timestampBoundary(2);
        if(o.visualize) {
            VkRenderPassBeginInfo ri{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};ri.renderPass=renderPass;ri.framebuffer=framebuffer;ri.renderArea={{0,0},{imageSize,imageSize}};
            vkCmdBeginRenderPass(command,&ri,VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_GRAPHICS,drawPipeline);
            vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,1,&descriptors,0,nullptr);
            vkCmdDraw(command,3,1,0,0);vkCmdEndRenderPass(command);
        }
        stamp(7,true);timestampBoundary(3);
        // Presentation copy, WSI waits and readbacks are excluded from GPU scopes.
        if(present) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;b.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;b.image=swapImages[imageIndex];b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
            VkImageBlit blit{};blit.srcSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};blit.srcOffsets[1]={int(imageSize),int(imageSize),1};blit.dstSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};blit.dstOffsets[1]={int(swapExtent.width),int(swapExtent.height),1};
            vkCmdBlitImage(command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,swapImages[imageIndex],VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&blit,VK_FILTER_NEAREST);
            b.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;b.dstAccessMask=0;b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;b.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&b);
        }
        submit(present,imageIndex);
        std::array<std::uint64_t,queryCount> ticks{};
        check(vkGetQueryPoolResults(device,queries,0,queryCount,sizeof(ticks),ticks.data(),sizeof(std::uint64_t),VK_QUERY_RESULT_64_BIT|VK_QUERY_RESULT_WAIT_BIT),"read GPU timestamps");
        auto ms=[&](unsigned a,unsigned b) {
            auto mask=validBits==64?UINT64_MAX:((std::uint64_t(1)<<validBits)-1);
            return double((ticks[b]-ticks[a])&mask)*properties.limits.timestampPeriod/1e6;
        };
        Timing t;t.rng=(o.mode || o.isolate==2 || inputStream)?0:ms(0,1);t.compute=ms(0,3);
        t.simulation=o.isolate==1?0:(o.mode?t.compute:ms(2,3));
        t.rngInput=inputMs;t.rngStaging=stagingMs;t.upload=inputStream?ms(0,1):0;
        t.visualPrepare=o.visualize?ms(4,5):0;t.render=o.visualize?ms(6,7):0;t.gpu=ms(0,7);
        t.cpu=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
        if(window && epoch%15==0) {
            std::string title=std::string(o.workload?"Diffusion":"Monte Carlo pi")+" | "+(o.mode?"fused":"split")+" | RNG "+std::to_string(t.rng)+" ms | compute "+std::to_string(t.compute)+" ms | visual "+std::to_string(t.visualPrepare+t.render)+" ms | Esc exits";
            glfwSetWindowTitle(window,title.c_str());
        }
        return t;
    }
    std::vector<State> read(const Buffer& source) {
        auto staging=createBuffer(source.size,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        try {
            begin();barrier();VkBufferCopy copy{0,0,source.size};vkCmdCopyBuffer(command,source.handle,staging.handle,1,&copy);submit();
            std::vector<State> data(static_cast<std::size_t>(source.size)/sizeof(State));void* mapped=nullptr;
            check(vkMapMemory(device,staging.memory,0,staging.size,0,&mapped),"map readback");std::memcpy(data.data(),mapped,static_cast<std::size_t>(staging.size));vkUnmapMemory(device,staging.memory);
            destroyBuffer(staging);return data;
        } catch(...) {destroyBuffer(staging);throw;}
    }
    void saveImage(const std::string& path) {
        auto staging=createBuffer(imageSize*imageSize*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        try {
            begin();barrier();VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={imageSize,imageSize,1};
            vkCmdCopyImageToBuffer(command,image,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,staging.handle,1,&copy);submit();
            std::ofstream out(path,std::ios::binary);if(!out)throw std::runtime_error("Cannot write image: "+path);
            out << "P6\n" << imageSize << ' ' << imageSize << "\n255\n";
            void* mapped=nullptr;check(vkMapMemory(device,staging.memory,0,staging.size,0,&mapped),"map image");
            auto* rgba=static_cast<const char*>(mapped);for(std::size_t i=0;i<imageSize*imageSize;++i)out.write(rgba+i*4,3);
            vkUnmapMemory(device,staging.memory);if(!out)throw std::runtime_error("Image write failed");destroyBuffer(staging);
        } catch(...) {destroyBuffer(staging);throw;}
    }
    void cleanup() {
        if(device) {
            vkDeviceWaitIdle(device);destroySwapchain();
            if(acquired)vkDestroySemaphore(device,acquired,nullptr);
            if(drawPipeline)vkDestroyPipeline(device,drawPipeline,nullptr);
            if(visualPipeline)vkDestroyPipeline(device,visualPipeline,nullptr);
            if(simulationPipeline)vkDestroyPipeline(device,simulationPipeline,nullptr);
            if(rngPipeline)vkDestroyPipeline(device,rngPipeline,nullptr);
            if(framebuffer)vkDestroyFramebuffer(device,framebuffer,nullptr);
            if(renderPass)vkDestroyRenderPass(device,renderPass,nullptr);
            if(imageView)vkDestroyImageView(device,imageView,nullptr);
            if(image)vkDestroyImage(device,image,nullptr);
            if(imageMemory)vkFreeMemory(device,imageMemory,nullptr);
            if(inputMapped)vkUnmapMemory(device,inputStaging.memory);
            inputMapped=nullptr;destroyBuffer(inputStaging);
            if(pipelineLayout)vkDestroyPipelineLayout(device,pipelineLayout,nullptr);
            if(descriptorPool)vkDestroyDescriptorPool(device,descriptorPool,nullptr);
            if(descriptorLayout)vkDestroyDescriptorSetLayout(device,descriptorLayout,nullptr);
            destroyBuffer(timestampScratch);destroyBuffer(pixels);destroyBuffer(states);destroyBuffer(randoms);
            if(queries)vkDestroyQueryPool(device,queries,nullptr);
            if(fence)vkDestroyFence(device,fence,nullptr);
            if(commandPool)vkDestroyCommandPool(device,commandPool,nullptr);
            vkDestroyDevice(device,nullptr);
        }
        if(surface)vkDestroySurfaceKHR(instance,surface,nullptr);
        if(messenger) {
            auto destroy=reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance,"vkDestroyDebugUtilsMessengerEXT"));if(destroy)destroy(instance,messenger,nullptr);
        }
        if(instance)vkDestroyInstance(instance,nullptr);
        if(window)glfwDestroyWindow(window);
        if(glfwReady)glfwTerminate();
    }
};

Gpu::Gpu(const Options& o):impl(std::make_unique<Impl>(o)){}
Gpu::~Gpu()=default;
bool Gpu::poll() {
    if(!impl->window)return true;
    glfwPollEvents();
    if(glfwGetKey(impl->window,GLFW_KEY_ESCAPE)==GLFW_PRESS)glfwSetWindowShouldClose(impl->window,GLFW_TRUE);
    int w,h;glfwGetFramebufferSize(impl->window,&w,&h);
    while((w==0||h==0)&&!glfwWindowShouldClose(impl->window)) {glfwWaitEvents();glfwGetFramebufferSize(impl->window,&w,&h);}
    return !glfwWindowShouldClose(impl->window);
}
Timing Gpu::frame(std::uint32_t epoch){return impl->frame(epoch);}
std::vector<State> Gpu::readStates(){return impl->read(impl->states);}
std::vector<State> Gpu::readRandoms(){return impl->read(impl->randoms);}
void Gpu::saveImage(const std::string& p){impl->saveImage(p);}
const std::string& Gpu::deviceName()const{return impl->name;}
std::uint32_t Gpu::driverVersion()const{return impl->properties.driverVersion;}
float Gpu::timestampPeriod()const{return impl->properties.limits.timestampPeriod;}
std::uint32_t Gpu::timestampBits()const{return impl->validBits;}
const char* Gpu::timestampMethod()const{
#ifdef __APPLE__
    return "encoder_boundaries";
#else
    return "native";
#endif
}
unsigned Gpu::validationErrors()const{return impl->errors.load();}
