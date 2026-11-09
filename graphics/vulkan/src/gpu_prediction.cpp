#include "gpu_prediction.hpp"
#include <chrono>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void check(VkResult result, const char *operation) {
    if (result != VK_SUCCESS)
        throw std::runtime_error(std::string(operation) + " failed: " + std::to_string(result));
}
} // namespace
class GpuPrediction::Impl {
  public:
    ComputeContext context;
    prediction::Course course;
    struct Buffer {
        VkBuffer handle = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void *mapped = nullptr;
    };
    Buffer courseBuffer, output, readback;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkCommandBuffer commands = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkQueryPool queries = VK_NULL_HANDLE;
    VkShaderModule shader = VK_NULL_HANDLE;
    double timestampPeriod = 0;
    uint32_t timestampBits = 0;
    bool pending = false;
    prediction::Request request;
    std::chrono::steady_clock::time_point started;

    Impl(ComputeContext c, const prediction::Course &data) : context(c), course(data) {
        try {
            initialize();
        } catch (...) {
            destroy();
            throw;
        }
    }
    ~Impl() {
        destroy();
    }
    void destroyBuffer(Buffer &b) {
        if (b.mapped)
            vkUnmapMemory(context.device, b.memory);
        if (b.handle)
            vkDestroyBuffer(context.device, b.handle, nullptr);
        if (b.memory)
            vkFreeMemory(context.device, b.memory, nullptr);
        b = {};
    }
    void destroy() {
        if (pending)
            vkWaitForFences(context.device, 1, &fence, VK_TRUE, UINT64_MAX);
        if (pipeline)
            vkDestroyPipeline(context.device, pipeline, nullptr);
        if (shader)
            vkDestroyShaderModule(context.device, shader, nullptr);
        if (pipelineLayout)
            vkDestroyPipelineLayout(context.device, pipelineLayout, nullptr);
        if (descriptorPool)
            vkDestroyDescriptorPool(context.device, descriptorPool, nullptr);
        if (setLayout)
            vkDestroyDescriptorSetLayout(context.device, setLayout, nullptr);
        if (queries)
            vkDestroyQueryPool(context.device, queries, nullptr);
        if (fence)
            vkDestroyFence(context.device, fence, nullptr);
        if (pool)
            vkDestroyCommandPool(context.device, pool, nullptr);
        destroyBuffer(courseBuffer);
        destroyBuffer(output);
        destroyBuffer(readback);
    }
    void allocate(Buffer &b, VkDeviceSize size, VkBufferUsageFlags usage,
                  VkMemoryPropertyFlags flags) {
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = usage;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        check(vkCreateBuffer(context.device, &info, nullptr, &b.handle), "prediction buffer");
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(context.device, b.handle, &requirements);
        VkPhysicalDeviceMemoryProperties memory;
        vkGetPhysicalDeviceMemoryProperties(context.physical, &memory);
        uint32_t type = memory.memoryTypeCount;
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i)) &&
                (memory.memoryTypes[i].propertyFlags & flags) == flags) {
                type = i;
                break;
            }
        }
        if (type == memory.memoryTypeCount)
            throw std::runtime_error("prediction memory type unavailable");
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize = requirements.size;
        allocation.memoryTypeIndex = type;
        check(vkAllocateMemory(context.device, &allocation, nullptr, &b.memory),
              "prediction memory");
        check(vkBindBufferMemory(context.device, b.handle, b.memory, 0), "prediction bind");
        if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
            check(vkMapMemory(context.device, b.memory, 0, size, 0, &b.mapped), "prediction map");
    }
    void initialize() {
        prediction::validate(course, prediction::Request{});
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(context.physical, &properties);
        if (properties.limits.maxComputeWorkGroupInvocations < 64 ||
            properties.limits.maxComputeWorkGroupSize[0] < 64 ||
            properties.limits.maxComputeWorkGroupCount[1] < prediction::CHOICES ||
            properties.limits.maxPushConstantsSize < sizeof(request)) {
            throw std::runtime_error("GPU lacks prediction limits");
        }
        timestampPeriod = properties.limits.timestampPeriod;
        uint32_t queueCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(context.physical, &queueCount, nullptr);
        std::vector<VkQueueFamilyProperties> families(queueCount);
        vkGetPhysicalDeviceQueueFamilyProperties(context.physical, &queueCount, families.data());
        timestampBits = families.at(context.family).timestampValidBits;
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = context.family;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        check(vkCreateCommandPool(context.device, &poolInfo, nullptr, &pool),
              "prediction command pool");
        VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandInfo.commandPool = pool;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(context.device, &commandInfo, &commands),
              "prediction commands");
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        check(vkCreateFence(context.device, &fenceInfo, nullptr, &fence), "prediction fence");
        allocate(courseBuffer, sizeof(course),
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        allocate(output, prediction::CHOICES * sizeof(prediction::Summary),
                 VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                 VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        allocate(readback, prediction::CHOICES * sizeof(prediction::Summary),
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        Buffer staging;
        try {
            allocate(staging, sizeof(course), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            std::memcpy(staging.mapped, &course, sizeof(course));
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            check(vkBeginCommandBuffer(commands, &begin), "course upload begin");
            VkBufferCopy copy{0, 0, sizeof(course)};
            vkCmdCopyBuffer(commands, staging.handle, courseBuffer.handle, 1, &copy);
            VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, nullptr,
                                 0, nullptr);
            check(vkEndCommandBuffer(commands), "course upload end");
            VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
            submit.commandBufferCount = 1;
            submit.pCommandBuffers = &commands;
            check(vkQueueSubmit(context.queue, 1, &submit, fence), "course upload submit");
            check(vkWaitForFences(context.device, 1, &fence, VK_TRUE, UINT64_MAX),
                  "course upload wait");
        } catch (...) {
            destroyBuffer(staging);
            throw;
        }
        destroyBuffer(staging);
        VkDescriptorSetLayoutBinding bindings[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount = 2;
        layout.pBindings = bindings;
        check(vkCreateDescriptorSetLayout(context.device, &layout, nullptr, &setLayout),
              "prediction descriptor layout");
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
        VkDescriptorPoolCreateInfo descriptorInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        descriptorInfo.maxSets = 1;
        descriptorInfo.poolSizeCount = 1;
        descriptorInfo.pPoolSizes = &poolSize;
        check(vkCreateDescriptorPool(context.device, &descriptorInfo, nullptr, &descriptorPool),
              "prediction descriptor pool");
        VkDescriptorSetAllocateInfo setInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        setInfo.descriptorPool = descriptorPool;
        setInfo.descriptorSetCount = 1;
        setInfo.pSetLayouts = &setLayout;
        check(vkAllocateDescriptorSets(context.device, &setInfo, &set),
              "prediction descriptor set");
        VkDescriptorBufferInfo buffers[] = {
            {courseBuffer.handle, 0, sizeof(course)},
            {output.handle, 0, prediction::CHOICES * sizeof(prediction::Summary)}};
        VkWriteDescriptorSet writes[2]{};
        for (uint32_t i = 0; i < 2; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].dstSet = set;
            writes[i].dstBinding = i;
            writes[i].descriptorCount = 1;
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[i].pBufferInfo = &buffers[i];
        }
        vkUpdateDescriptorSets(context.device, 2, writes, 0, nullptr);
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(request)};
        VkPipelineLayoutCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineInfo.setLayoutCount = 1;
        pipelineInfo.pSetLayouts = &setLayout;
        pipelineInfo.pushConstantRangeCount = 1;
        pipelineInfo.pPushConstantRanges = &push;
        check(vkCreatePipelineLayout(context.device, &pipelineInfo, nullptr, &pipelineLayout),
              "prediction pipeline layout");
        std::ifstream file("build/predict.spv", std::ios::binary | std::ios::ate);
        if (!file)
            throw std::runtime_error("missing build/predict.spv; run make game");
        const auto length = file.tellg();
        if (length <= 0 || size_t(length) % 4)
            throw std::runtime_error("invalid prediction SPIR-V");
        std::vector<uint32_t> code(size_t(length) / 4);
        file.seekg(0);
        file.read(reinterpret_cast<char *>(code.data()), length);
        VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        module.codeSize = size_t(length);
        module.pCode = code.data();
        check(vkCreateShaderModule(context.device, &module, nullptr, &shader), "prediction shader");
        VkComputePipelineCreateInfo compute{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        compute.layout = pipelineLayout;
        compute.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        compute.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        compute.stage.module = shader;
        compute.stage.pName = "main";
        check(vkCreateComputePipelines(context.device, VK_NULL_HANDLE, 1, &compute, nullptr,
                                       &pipeline),
              "prediction pipeline");
        if (timestampBits) {
            VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
            query.queryType = VK_QUERY_TYPE_TIMESTAMP;
            query.queryCount = 2;
            check(vkCreateQueryPool(context.device, &query, nullptr, &queries),
                  "prediction timestamps");
        }
    }
    bool submit(const prediction::Request &input) {
        if (pending)
            return false;
        prediction::validate(course, input);
        request = input;
        started = std::chrono::steady_clock::now();
        check(vkResetFences(context.device, 1, &fence), "prediction fence reset");
        check(vkResetCommandBuffer(commands, 0), "prediction command reset");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(commands, &begin), "prediction begin");
        vkCmdFillBuffer(commands, output.handle, 0,
                        prediction::CHOICES * sizeof(prediction::Summary), 0);
        VkMemoryBarrier clear{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        clear.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        clear.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &clear, 0, nullptr, 0,
                             nullptr);
        if (queries) {
            vkCmdResetQueryPool(commands, queries, 0, 2);
            vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, 0);
        }
        vkCmdBindPipeline(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(commands, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1,
                                &set, 0, nullptr);
        vkCmdPushConstants(commands, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(request), &request);
        vkCmdDispatch(commands, (request.rollouts + 63) / 64, request.routeCount, 1);
        if (queries)
            vkCmdWriteTimestamp(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, queries, 1);
        VkMemoryBarrier transfer{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        transfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        transfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &transfer, 0, nullptr, 0,
                             nullptr);
        VkBufferCopy copy{0, 0, prediction::CHOICES * sizeof(prediction::Summary)};
        vkCmdCopyBuffer(commands, output.handle, readback.handle, 1, &copy);
        VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 1, &host, 0, nullptr, 0, nullptr);
        check(vkEndCommandBuffer(commands), "prediction end");
        VkSubmitInfo submitInfo{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commands;
        check(vkQueueSubmit(context.queue, 1, &submitInfo, fence), "prediction submit");
        pending = true;
        return true;
    }
    bool poll(prediction::Result &result) {
        if (!pending)
            return false;
        const auto status = vkGetFenceStatus(context.device, fence);
        if (status == VK_NOT_READY)
            return false;
        check(status, "prediction completion");
        result = {};
        result.request = request;
        std::memcpy(result.routes.data(), readback.mapped,
                    prediction::CHOICES * sizeof(prediction::Summary));
        result.latencyMs =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
                .count();
        if (queries) {
            uint64_t stamps[2]{};
            check(vkGetQueryPoolResults(context.device, queries, 0, 2, sizeof(stamps), stamps,
                                        sizeof(uint64_t), VK_QUERY_RESULT_64_BIT),
                  "prediction query read");
            const uint64_t mask =
                timestampBits == 64 ? UINT64_MAX : (uint64_t(1) << timestampBits) - 1;
            result.kernelMs = double((stamps[1] - stamps[0]) & mask) * timestampPeriod / 1000000.0;
        }
        pending = false;
        return true;
    }
};
GpuPrediction::GpuPrediction(ComputeContext context, const prediction::Course &course)
    : impl(std::make_unique<Impl>(context, course)) {}
GpuPrediction::~GpuPrediction() = default;
bool GpuPrediction::busy() const {
    return impl->pending;
}
bool GpuPrediction::submit(const prediction::Request &r) {
    return impl->submit(r);
}
bool GpuPrediction::poll(prediction::Result &r) {
    return impl->poll(r);
}
