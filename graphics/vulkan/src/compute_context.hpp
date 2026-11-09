#pragma once
#include <vulkan/vulkan.h>

struct ComputeContext {
    VkPhysicalDevice physical;
    VkDevice device;
    VkQueue queue;
    uint32_t family;
};
