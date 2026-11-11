#include "renderer.hpp"
#include "profiling.hpp"
#include "nbody.hpp"
#include "compute_context.hpp"

#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <iostream>
#include <fstream>
#include <stdexcept>
#include <algorithm>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <array>
#include <optional>
#include <set>
#include <cmath>
const int MAX_FRAMES_IN_FLIGHT = 2;
const uint32_t COMPUTE_WORKGROUP_SIZE = 128; // Must match nbody.comp.

const std::vector<const char*> validationLayers = {
    "VK_LAYER_KHRONOS_validation"
};

const std::vector<const char*> deviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME
};

#ifdef NDEBUG
const bool enableValidationLayers = false;
#else
const bool enableValidationLayers = true;
#endif

VkResult CreateDebugUtilsMessengerEXT(VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo, const VkAllocationCallbacks* pAllocator, VkDebugUtilsMessengerEXT* pDebugMessenger) {
    auto func = (PFN_vkCreateDebugUtilsMessengerEXT) vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
    if (func != nullptr) {
        return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
    } else {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
}

void DestroyDebugUtilsMessengerEXT(VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger, const VkAllocationCallbacks* pAllocator) {
    auto func = (PFN_vkDestroyDebugUtilsMessengerEXT) vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
    if (func != nullptr) {
        func(instance, debugMessenger, pAllocator);
    }
}

struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;

    bool isComplete() {
        return graphicsFamily.has_value() && presentFamily.has_value();
    }
};

struct SwapChainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

static VkVertexInputBindingDescription vertexBindingDescription() {
    VkVertexInputBindingDescription description{};
    description.binding = 0;
    description.stride = sizeof(Vertex);
    description.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    return description;
}

static std::array<VkVertexInputAttributeDescription, 3> vertexAttributeDescriptions() {
    std::array<VkVertexInputAttributeDescription, 3> descriptions{};

    descriptions[0].binding = 0;
    descriptions[0].location = 0;
    descriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    descriptions[0].offset = offsetof(Vertex, position);

    descriptions[1].binding = 0;
    descriptions[1].location = 1;
    descriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    descriptions[1].offset = offsetof(Vertex, color);

    descriptions[2].binding = 0;
    descriptions[2].location = 2;
    descriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
    descriptions[2].offset = offsetof(Vertex, textureCoordinate);

    return descriptions;
}

static_assert(sizeof(glm::mat4) == 64, "shader expects a 64-byte model matrix");
static_assert(sizeof(FrameTransforms) == 144, "two packed mat4s plus scene atmosphere");
static_assert(offsetof(FrameTransforms, atmosphere) == 128, "shader atmosphere offset");
static_assert(offsetof(FrameTransforms, projection) == 64, "shader matrix offsets must match");
static_assert(sizeof(gpu_particle) == 32, "shader expects a 32-byte particle");
static_assert(offsetof(gpu_particle, velocity) == 16, "shader particle offsets must match");

// Matches the push-constant block in nbody.comp, including field order.
struct SimulationPushConstants {
    float STEP_SIZE;
    float GRAV_SOFT;
    float GRAV_CONS;
    uint32_t NUM_STARS;
};
static_assert(sizeof(SimulationPushConstants) == 16, "shader expects 16 bytes of parameters");
static_assert(offsetof(SimulationPushConstants, NUM_STARS) == 12, "shader parameter offsets must match");
static_assert(sizeof(BhNode) == 48, "Barnes-Hut shader expects three vec4s");
struct BhPushConstants {
    float dt, softening, gravity;
    uint32_t count;
    float theta;
    uint32_t leafCount, phase = 0, offset = 0;
    uint32_t workCount = 0, sortWidth = 0, sortStride = 0, unused = 0;
};
static_assert(sizeof(BhPushConstants) == 48, "Barnes-Hut shader parameter layout");
using FrameClock = std::chrono::steady_clock;
static double frameElapsed(FrameClock::time_point start) {
    return std::chrono::duration<double, std::milli>(FrameClock::now() - start).count();
}

class Renderer::Impl {
public:
    Impl(
        uint32_t initialWidth,
        uint32_t initialHeight,
        const char* windowTitle,
        const std::vector<Mesh>& meshes,
        const std::vector<gpu_particle>& initialParticles,
        const std::string& initialTexturePath,
        const RendererOptions& rendererOptions,
        uint32_t sceneCapacity = 0)
        : width(initialWidth),
        height(initialHeight),
        title(windowTitle),
        texturePath(initialTexturePath),
        particleCount(0), options(rendererOptions), sceneMode(sceneCapacity != 0) {

        if (meshes.size() != 1 || (!sceneMode && initialParticles.empty()) ||
            initialParticles.size() > std::numeric_limits<uint32_t>::max()) {
            throw std::invalid_argument(
                "provide one shared mesh and a nonempty particle array");
        }
        particleCount = sceneMode ? sceneCapacity : static_cast<uint32_t>(initialParticles.size());
        if (particleCount > (1u << 23)) {
            throw std::invalid_argument("particle count exceeds this demo's tree/index limit");
        }
        if (isCpu() && !sceneMode) cpuSimulation = std::make_unique<CpuNBody>(initialParticles, isBarnesHut());
        for (const auto& particle : initialParticles) {
            for (int component = 0; component < 4; ++component) {
                if (!std::isfinite(particle.positionMass[component]) ||
                    !std::isfinite(particle.velocity[component])) {
                    throw std::invalid_argument("initial particles must be finite");
                }
            }
            if (particle.positionMass.w < 0.0f) {
                throw std::invalid_argument("particle mass must be nonnegative");
            }
        }

        const auto& mesh = meshes[0];
        if (mesh.vertices.empty() || mesh.indices.empty()) {
            throw std::invalid_argument("mesh must contain vertices and indices");
        }
        if (mesh.indices.size() > std::numeric_limits<uint32_t>::max()) {
            throw std::invalid_argument("too many mesh indices");
        }
        for (auto index : mesh.indices) {
            if (index >= mesh.vertices.size()) {
                throw std::invalid_argument("mesh index outside vertex array");
            }
        }

        initWindow();
        initVulkan(meshes, initialParticles);
    }

    ~Impl() {
        vkDeviceWaitIdle(device);
        if (timingsEnabled()) for (uint32_t i = 0; i < frames.size(); ++i) collectGpuTiming(i);
        if constexpr (profiling::ENABLED) profiling::profiler().finish();
        cleanup();
    }

    bool shouldClose() const {
        return glfwWindowShouldClose(window);
    }

    void pollEvents() const {
        glfwPollEvents();
    }

    float aspectRatio() const {
        return swapChainExtent.width / static_cast<float>(swapChainExtent.height);
    }

    ComputeContext computeContext() {
        return {physicalDevice, device, graphicsQueue, findQueueFamilies(physicalDevice).graphicsFamily.value()};
    }

    void drawScene(const FrameTransforms& transforms, const std::vector<SceneInstance>& objects) {
        if (!sceneMode || objects.size() > particleCount) throw std::runtime_error("scene capacity exceeded");
        sceneObjects = &objects;
        drawFrameInternal(transforms, SimulationParameters{});
        sceneObjects = nullptr;
    }
    FrameTimings completedSceneTiming{};
    bool completedSceneReady = false;
    bool readCompletedSceneTiming(FrameTimings& result) {
        if (!completedSceneReady) return false;
        result = completedSceneTiming;
        completedSceneReady = false;
        return true;
    }

    void captureNextFrame(const std::string& path) {
        if (!sceneMode || !captureSupported) throw std::runtime_error("scene screenshot unsupported");
        capturePath = path;
    }

    void drawFrame(
        const FrameTransforms& transforms,
        const SimulationParameters& simulation) {

        if (sceneMode) throw std::logic_error("use drawScene for the game renderer");

        if (!std::isfinite(simulation.stepSize) || simulation.stepSize < 0.0f ||
            !std::isfinite(simulation.softening) || simulation.softening <= 0.0f ||
            !std::isfinite(simulation.gravityConstant) || simulation.gravityConstant < 0.0f ||
            !std::isfinite(simulation.theta) || simulation.theta < 0.0f) {
            throw std::invalid_argument("require finite timestep/gravity >= 0 and softening > 0");
        }
        drawFrameInternal(transforms, simulation);
    }

    std::vector<gpu_particle> readParticles() {
        if (sceneMode) throw std::logic_error("a scene renderer has no particles");
        if (isCpu()) return cpuSimulation->particles();
        // Deliberately blocking: useful for correctness tests, not the frame loop.
        if (vkDeviceWaitIdle(device) != VK_SUCCESS) {
            throw std::runtime_error("failed to wait for particle readback");
        }
        const VkDeviceSize bytes = VkDeviceSize(particleCount) * sizeof(gpu_particle);
        VkBuffer staging;
        VkDeviceMemory memory;
        createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            staging, memory);

        auto commandBuffer = beginSingleTimeCommands();
        VkMemoryBarrier beforeCopy{};
        beforeCopy.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        beforeCopy.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        beforeCopy.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(commandBuffer,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &beforeCopy, 0, nullptr, 0, nullptr);
        VkBufferCopy copy{};
        copy.size = bytes;
        vkCmdCopyBuffer(commandBuffer, particleBuffers[currentParticleBuffer].buffer,
            staging, 1, &copy);
        VkMemoryBarrier afterCopy{};
        afterCopy.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        afterCopy.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        afterCopy.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &afterCopy, 0, nullptr, 0, nullptr);
        endSingleTimeCommands(commandBuffer);

        std::vector<gpu_particle> result(particleCount);
        void* mapped = nullptr;
        if (vkMapMemory(device, memory, 0, bytes, 0, &mapped) != VK_SUCCESS) {
            vkDestroyBuffer(device, staging, nullptr);
            vkFreeMemory(device, memory, nullptr);
            throw std::runtime_error("failed to map particle readback");
        }
        memcpy(result.data(), mapped, static_cast<size_t>(bytes));
        vkUnmapMemory(device, memory);
        vkDestroyBuffer(device, staging, nullptr);
        vkFreeMemory(device, memory, nullptr);
        return result;
    }

    FrameTimings finishFrame() {
        if (lastSubmittedFrame == std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error("no frame has been submitted");
        }
        if (vkWaitForFences(device, 1, &inFlightFences[lastSubmittedFrame], VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
            throw std::runtime_error("failed to finish benchmark frame");
        }
        collectGpuTiming(lastSubmittedFrame);
        return frames[lastSubmittedFrame].timings;
    }

    void* nativeWindowHandle() const {
        return window;
    }

    // Startup upload through CPU-visible staging into device-local memory.
    void uploadBuffer(
        const void* source,
        VkDeviceSize bytes,
        VkBufferUsageFlags usage,
        VkBuffer& destination,
        VkDeviceMemory& destinationMemory) {
        VkBuffer stagingBuffer;
        VkDeviceMemory stagingMemory;

        createBuffer(
            bytes,
            VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            stagingBuffer,
            stagingMemory);

        void* mapped = nullptr;
        if (vkMapMemory(device, stagingMemory, 0, bytes, 0, &mapped)
                != VK_SUCCESS) {
            vkDestroyBuffer(device, stagingBuffer, nullptr);
            vkFreeMemory(device, stagingMemory, nullptr);
            throw std::runtime_error("failed to map mesh staging memory");
        }

        memcpy(mapped, source, static_cast<size_t>(bytes));
        vkUnmapMemory(device, stagingMemory);

        createBuffer(
            bytes,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | usage,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            destination,
            destinationMemory);

        copyBuffer(stagingBuffer, destination, bytes);

        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingMemory, nullptr);
    }

    void recordMeshDraw(
        VkCommandBuffer commandBuffer,
        VkBuffer vertices,
        VkBuffer indices,
        uint32_t instanceCount,
        uint32_t indexCount) {

        const VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertices, &offset);
        vkCmdBindIndexBuffer(commandBuffer, indices, 0, VK_INDEX_TYPE_UINT32);

        vkCmdDrawIndexed(
            commandBuffer, indexCount, instanceCount, 0, 0, 0);
    }


private:
    uint32_t width;
    uint32_t height;
    std::string title;
    std::string texturePath;
    uint32_t particleCount;
    RendererOptions options;
    std::unique_ptr<CpuNBody> cpuSimulation;
    bool sceneMode = false;
    const std::vector<SceneInstance>* sceneObjects = nullptr;
    uint32_t lastSubmittedFrame = std::numeric_limits<uint32_t>::max();
    bool isCpu() const {
        return sceneMode || options.mode == SimulationMode::CpuAllPairs || options.mode == SimulationMode::CpuBarnesHut;
    }
    bool isBarnesHut() const {
        return options.mode == SimulationMode::CpuBarnesHut || options.mode == SimulationMode::GpuBarnesHut;
    }
    bool timingsEnabled() const { return options.measureTimings || profiling::ENABLED; }

    GLFWwindow* window;

    VkInstance instance;
    VkDebugUtilsMessengerEXT debugMessenger;
    VkSurfaceKHR surface;

    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device;

    VkQueue graphicsQueue;
    VkQueue presentQueue;

    VkSwapchainKHR swapChain;
    std::vector<VkImage> swapChainImages;
    VkFormat swapChainImageFormat;
    VkExtent2D swapChainExtent;
    std::vector<VkImageView> swapChainImageViews;
    std::vector<VkFramebuffer> swapChainFramebuffers;

    VkRenderPass renderPass;
    VkDescriptorSetLayout descriptorSetLayout;
    VkPipelineLayout pipelineLayout;
    VkPipeline graphicsPipeline;

    VkDescriptorSetLayout computeDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool computeDescriptorPool = VK_NULL_HANDLE;
    VkPipelineLayout computePipelineLayout = VK_NULL_HANDLE;
    VkPipeline computePipeline = VK_NULL_HANDLE;
    VkPipeline treeBuildPipeline = VK_NULL_HANDLE;

    // Simulation snapshots, NOT frame slots: step t reads one and writes the other.
    struct ParticleBuffer {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };
    std::array<ParticleBuffer, 2> particleBuffers{};
    ParticleBuffer sortKeys, treeNodes, sortedRanks;
    ParticleBuffer screenshot;
    void* screenshotMapped = nullptr;
    VkDeviceSize screenshotBytes = 0;
    bool captureSupported = false;
    std::string capturePath;
    uint32_t leafCount = 1;
    uint32_t currentParticleBuffer = 0;

    VkCommandPool commandPool;

    VkImage depthImage;
    VkDeviceMemory depthImageMemory;
    VkImageView depthImageView;

    VkImage textureImage;
    VkDeviceMemory textureImageMemory;
    VkImageView textureImageView;
    VkSampler textureSampler;

    // CPU bookkeeping for GPU-resident geometry. No duplicate CPU vertex arrays.
    struct GpuMesh {
        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
        VkBuffer indexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory indexMemory = VK_NULL_HANDLE;
        uint32_t indexCount = 0;
    };
    std::vector<GpuMesh> gpuMeshes;

    // One shared camera UBO per in-flight frame, independent of mesh count.
    struct FrameResources {
        VkBuffer uniformBuffer = VK_NULL_HANDLE;
        VkDeviceMemory uniformMemory = VK_NULL_HANDLE;
        void* mapped = nullptr;

        VkBuffer modelsBuffer = VK_NULL_HANDLE;
        VkDeviceMemory modelsMemory = VK_NULL_HANDLE;
        void* modelsMapped = nullptr; // Only CPU simulation modes map model buffers.
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        // [0]: A -> B, [1]: B -> A. Both write this frame slot's models buffer.
        std::array<VkDescriptorSet, 2> computeDescriptorSets{};
        bool timestampsPending = false;
        uint64_t profilingFrameId = 0;
        FrameTimings timings;
    };
    std::array<FrameResources, MAX_FRAMES_IN_FLIGHT> frames{};
    VkQueryPool timestampPool = VK_NULL_HANDLE;
    uint32_t timestampValidBits = 0;
    double timestampPeriodNs = 0.0;
    VkDescriptorPool descriptorPool;

    std::vector<VkCommandBuffer> commandBuffers;

    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> inFlightFences;
    uint32_t currentFrame = 0;

    bool framebufferResized = false;

    void initWindow() {
        glfwInit();

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

        window = glfwCreateWindow(width, height, title.c_str(), nullptr, nullptr);
        glfwSetWindowUserPointer(window, this);
        glfwSetFramebufferSizeCallback(window, framebufferResizeCallback);
    }

    static void framebufferResizeCallback(GLFWwindow* window, int width, int height) {
        auto app = reinterpret_cast<Impl*>(glfwGetWindowUserPointer(window));
        app->framebufferResized = true;
    }

    void initVulkan(const std::vector<Mesh>& meshes,
                    const std::vector<gpu_particle>& initialParticles) {
        createInstance();
        setupDebugMessenger();
        createSurface();
        pickPhysicalDevice();
        createLogicalDevice();
        createSwapChain();
        createImageViews();
        createRenderPass();
        createDescriptorSetLayout();
        createGraphicsPipeline();
        createCommandPool();
        createDepthResources();
        createFramebuffers();
        createTextureImage();
        createTextureImageView();
        createTextureSampler();
        createMeshBuffers(meshes);
        createUniformBuffers();
        if (!isCpu()) createParticleBuffers(initialParticles);
        createDescriptorPool();
        createDescriptorSets();
        if (!isCpu()) {
            createComputePipeline();
            createComputeDescriptorSets();
        }
        createCommandBuffers();
        createSyncObjects();
        if (timingsEnabled()) createProfilingQueries();
    }

    void cleanupSwapChain() {
        for (auto semaphore : renderFinishedSemaphores) {
            vkDestroySemaphore(device, semaphore, nullptr);
        }
        renderFinishedSemaphores.clear();

        vkDestroyImageView(device, depthImageView, nullptr);
        vkDestroyImage(device, depthImage, nullptr);
        vkFreeMemory(device, depthImageMemory, nullptr);

        for (auto framebuffer : swapChainFramebuffers) {
            vkDestroyFramebuffer(device, framebuffer, nullptr);
        }

        for (auto imageView : swapChainImageViews) {
            vkDestroyImageView(device, imageView, nullptr);
        }

        vkDestroySwapchainKHR(device, swapChain, nullptr);
    }

    void cleanup() {
        cleanupSwapChain();
        if (screenshotMapped) vkUnmapMemory(device, screenshot.memory);
        if (screenshot.buffer) vkDestroyBuffer(device, screenshot.buffer, nullptr);
        if (screenshot.memory) vkFreeMemory(device, screenshot.memory, nullptr);
        if (timestampPool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(device, timestampPool, nullptr);
        }

        vkDestroyPipeline(device, graphicsPipeline, nullptr);
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
        vkDestroyPipeline(device, computePipeline, nullptr);
        vkDestroyPipeline(device, treeBuildPipeline, nullptr);
        vkDestroyPipelineLayout(device, computePipelineLayout, nullptr);
        vkDestroyDescriptorPool(device, computeDescriptorPool, nullptr);
        vkDestroyDescriptorSetLayout(device, computeDescriptorSetLayout, nullptr);
        vkDestroyRenderPass(device, renderPass, nullptr);

        for (const auto& particles : particleBuffers) {
            if (particles.buffer != VK_NULL_HANDLE) {
                vkDestroyBuffer(device, particles.buffer, nullptr);
                vkFreeMemory(device, particles.memory, nullptr);
            }
        }
        for (const auto& scratch : {sortKeys, treeNodes, sortedRanks}) {
            if (scratch.buffer != VK_NULL_HANDLE) {
                vkDestroyBuffer(device, scratch.buffer, nullptr);
                vkFreeMemory(device, scratch.memory, nullptr);
            }
        }

        for (auto& frame : frames) {
            vkUnmapMemory(device, frame.uniformMemory);
            vkDestroyBuffer(device, frame.uniformBuffer, nullptr);
            vkFreeMemory(device, frame.uniformMemory, nullptr);
            if (frame.modelsMapped) vkUnmapMemory(device, frame.modelsMemory);
            vkDestroyBuffer(device, frame.modelsBuffer, nullptr);
            vkFreeMemory(device, frame.modelsMemory, nullptr);
        }

        vkDestroyDescriptorPool(device, descriptorPool, nullptr);

        vkDestroySampler(device, textureSampler, nullptr);
        vkDestroyImageView(device, textureImageView, nullptr);

        vkDestroyImage(device, textureImage, nullptr);
        vkFreeMemory(device, textureImageMemory, nullptr);

        vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);

        for (const auto& mesh : gpuMeshes) {
            vkDestroyBuffer(device, mesh.indexBuffer, nullptr);
            vkFreeMemory(device, mesh.indexMemory, nullptr);
            vkDestroyBuffer(device, mesh.vertexBuffer, nullptr);
            vkFreeMemory(device, mesh.vertexMemory, nullptr);
        }

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            vkDestroySemaphore(device, imageAvailableSemaphores[i], nullptr);
            vkDestroyFence(device, inFlightFences[i], nullptr);
        }

        vkDestroyCommandPool(device, commandPool, nullptr);

        vkDestroyDevice(device, nullptr);

        if (enableValidationLayers) {
            DestroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
        }

        vkDestroySurfaceKHR(instance, surface, nullptr);
        vkDestroyInstance(instance, nullptr);

        glfwDestroyWindow(window);

        glfwTerminate();
    }

    void recreateSwapChain() {
        int width = 0, height = 0;
        glfwGetFramebufferSize(window, &width, &height);
        while (width == 0 || height == 0) {
            glfwGetFramebufferSize(window, &width, &height);
            glfwWaitEvents();
        }

        vkDeviceWaitIdle(device);

        cleanupSwapChain();

        createSwapChain();
        createImageViews();
        createDepthResources();
        createFramebuffers();
        createRenderFinishedSemaphores();
    }

    void createInstance() {
        if (enableValidationLayers && !checkValidationLayerSupport()) {
            throw std::runtime_error("validation layers requested, but not available!");
        }

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "Hello Triangle";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "No Engine";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_0;

        VkInstanceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;

        auto extensions = getRequiredExtensions();
        createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
        createInfo.ppEnabledExtensionNames = extensions.data();

        VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
        if (enableValidationLayers) {
            createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
            createInfo.ppEnabledLayerNames = validationLayers.data();

            populateDebugMessengerCreateInfo(debugCreateInfo);
            createInfo.pNext = (VkDebugUtilsMessengerCreateInfoEXT*) &debugCreateInfo;
        } else {
            createInfo.enabledLayerCount = 0;

            createInfo.pNext = nullptr;
        }

        if (vkCreateInstance(&createInfo, nullptr, &instance) != VK_SUCCESS) {
            throw std::runtime_error("failed to create instance!");
        }
    }

    void populateDebugMessengerCreateInfo(VkDebugUtilsMessengerCreateInfoEXT& createInfo) {
        createInfo = {};
        createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        createInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        createInfo.pfnUserCallback = debugCallback;
    }

    void setupDebugMessenger() {
        if (!enableValidationLayers) return;

        VkDebugUtilsMessengerCreateInfoEXT createInfo;
        populateDebugMessengerCreateInfo(createInfo);

        if (CreateDebugUtilsMessengerEXT(instance, &createInfo, nullptr, &debugMessenger) != VK_SUCCESS) {
            throw std::runtime_error("failed to set up debug messenger!");
        }
    }

    void createSurface() {
        if (glfwCreateWindowSurface(instance, window, nullptr, &surface) != VK_SUCCESS) {
            throw std::runtime_error("failed to create window surface!");
        }
    }

    void pickPhysicalDevice() {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);

        if (deviceCount == 0) {
            throw std::runtime_error("failed to find GPUs with Vulkan support!");
        }

        std::vector<VkPhysicalDevice> devices(deviceCount);
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());

        for (const auto& device : devices) {
            if (isDeviceSuitable(device)) {
                physicalDevice = device;
                break;
            }
        }

        if (physicalDevice == VK_NULL_HANDLE) {
            throw std::runtime_error("failed to find a suitable GPU!");
        }
    }

    void createLogicalDevice() {
        QueueFamilyIndices indices = findQueueFamilies(physicalDevice);

        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        std::set<uint32_t> uniqueQueueFamilies = {indices.graphicsFamily.value(), indices.presentFamily.value()};

        float queuePriority = 1.0f;
        for (uint32_t queueFamily : uniqueQueueFamilies) {
            VkDeviceQueueCreateInfo queueCreateInfo{};
            queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
            queueCreateInfo.queueFamilyIndex = queueFamily;
            queueCreateInfo.queueCount = 1;
            queueCreateInfo.pQueuePriorities = &queuePriority;
            queueCreateInfos.push_back(queueCreateInfo);
        }

        VkPhysicalDeviceFeatures deviceFeatures{};
        deviceFeatures.samplerAnisotropy = VK_TRUE;

        VkDeviceCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;

        createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
        createInfo.pQueueCreateInfos = queueCreateInfos.data();

        createInfo.pEnabledFeatures = &deviceFeatures;

        createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
        createInfo.ppEnabledExtensionNames = deviceExtensions.data();

        if (enableValidationLayers) {
            createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
            createInfo.ppEnabledLayerNames = validationLayers.data();
        } else {
            createInfo.enabledLayerCount = 0;
        }

        if (vkCreateDevice(physicalDevice, &createInfo, nullptr, &device) != VK_SUCCESS) {
            throw std::runtime_error("failed to create logical device!");
        }

        vkGetDeviceQueue(device, indices.graphicsFamily.value(), 0, &graphicsQueue);
        vkGetDeviceQueue(device, indices.presentFamily.value(), 0, &presentQueue);
    }

    void createSwapChain() {
        SwapChainSupportDetails swapChainSupport = querySwapChainSupport(physicalDevice);

        VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(swapChainSupport.formats);
        VkPresentModeKHR presentMode = chooseSwapPresentMode(swapChainSupport.presentModes);
        VkExtent2D extent = chooseSwapExtent(swapChainSupport.capabilities);
        if constexpr (profiling::ENABLED) {
            std::cout << "[profile] resolution=" << extent.width << 'x' << extent.height
                      << " presentation=" << (presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? "MAILBOX" : "FIFO")
                      << " validation=" << (enableValidationLayers ? "ON" : "OFF") << std::endl;
        }

        uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;
        if (swapChainSupport.capabilities.maxImageCount > 0 && imageCount > swapChainSupport.capabilities.maxImageCount) {
            imageCount = swapChainSupport.capabilities.maxImageCount;
        }

        VkSwapchainCreateInfoKHR createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        createInfo.surface = surface;

        createInfo.minImageCount = imageCount;
        createInfo.imageFormat = surfaceFormat.format;
        createInfo.imageColorSpace = surfaceFormat.colorSpace;
        createInfo.imageExtent = extent;
        createInfo.imageArrayLayers = 1;
        createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        captureSupported = sceneMode && (swapChainSupport.capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        if (captureSupported) createInfo.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

        QueueFamilyIndices indices = findQueueFamilies(physicalDevice);
        uint32_t queueFamilyIndices[] = {indices.graphicsFamily.value(), indices.presentFamily.value()};

        if (indices.graphicsFamily != indices.presentFamily) {
            createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
            createInfo.queueFamilyIndexCount = 2;
            createInfo.pQueueFamilyIndices = queueFamilyIndices;
        } else {
            createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        }

        createInfo.preTransform = swapChainSupport.capabilities.currentTransform;
        createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        createInfo.presentMode = presentMode;
        createInfo.clipped = VK_TRUE;

        if (vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapChain) != VK_SUCCESS) {
            throw std::runtime_error("failed to create swap chain!");
        }

        vkGetSwapchainImagesKHR(device, swapChain, &imageCount, nullptr);
        swapChainImages.resize(imageCount);
        vkGetSwapchainImagesKHR(device, swapChain, &imageCount, swapChainImages.data());

        swapChainImageFormat = surfaceFormat.format;
        swapChainExtent = extent;
    }

    void createImageViews() {
        swapChainImageViews.resize(swapChainImages.size());

        for (uint32_t i = 0; i < swapChainImages.size(); i++) {
            swapChainImageViews[i] = createImageView(swapChainImages[i], swapChainImageFormat, VK_IMAGE_ASPECT_COLOR_BIT);
        }
    }

    void createRenderPass() {
        VkAttachmentDescription colorAttachment{};
        colorAttachment.format = swapChainImageFormat;
        colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentDescription depthAttachment{};
        depthAttachment.format = findDepthFormat();
        depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
        depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkAttachmentReference colorAttachmentRef{};
        colorAttachmentRef.attachment = 0;
        colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depthAttachmentRef{};
        depthAttachmentRef.attachment = 1;
        depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorAttachmentRef;
        subpass.pDepthStencilAttachment = &depthAttachmentRef;

        VkSubpassDependency dependency{};
        dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
        dependency.dstSubpass = 0;
        dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
        dependency.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        std::array<VkAttachmentDescription, 2> attachments = {colorAttachment, depthAttachment};
        VkRenderPassCreateInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        renderPassInfo.pAttachments = attachments.data();
        renderPassInfo.subpassCount = 1;
        renderPassInfo.pSubpasses = &subpass;
        renderPassInfo.dependencyCount = 1;
        renderPassInfo.pDependencies = &dependency;

        if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) {
            throw std::runtime_error("failed to create render pass!");
        }
    }

    void createDescriptorSetLayout() {
        VkDescriptorSetLayoutBinding uboLayoutBinding{};
        uboLayoutBinding.binding = 0;
        uboLayoutBinding.descriptorCount = 1;
        uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        uboLayoutBinding.pImmutableSamplers = nullptr;
        uboLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutBinding samplerLayoutBinding{};
        samplerLayoutBinding.binding = 1;
        samplerLayoutBinding.descriptorCount = 1;
        samplerLayoutBinding.descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutBinding modelsLayoutBinding{};
        modelsLayoutBinding.binding = 2;
        modelsLayoutBinding.descriptorCount = 1;
        modelsLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        modelsLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        std::array<VkDescriptorSetLayoutBinding, 3> bindings = {
            uboLayoutBinding,
            samplerLayoutBinding,
            modelsLayoutBinding
        };

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();

        if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descriptorSetLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create descriptor set layout!");
        }
    }

    void createGraphicsPipeline() {
        auto vertShaderCode = readFile(sceneMode ? options.sceneShaders.vertex : "build/vert.spv");
        auto fragShaderCode = readFile(sceneMode ? options.sceneShaders.fragment : "build/frag.spv");

        VkShaderModule vertShaderModule = createShaderModule(vertShaderCode);
        VkShaderModule fragShaderModule = createShaderModule(fragShaderCode);

        VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
        vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
        vertShaderStageInfo.module = vertShaderModule;
        vertShaderStageInfo.pName = "main";

        VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
        fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        fragShaderStageInfo.module = fragShaderModule;
        fragShaderStageInfo.pName = "main";

        VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};

        VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
        vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        auto bindingDescription = vertexBindingDescription();
        auto attributeDescriptions = vertexAttributeDescriptions();

        vertexInputInfo.vertexBindingDescriptionCount = 1;
        vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
        vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
        vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssembly.primitiveRestartEnable = VK_FALSE;

        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.depthClampEnable = VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.depthBiasEnable = VK_FALSE;

        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.sampleShadingEnable = VK_FALSE;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = VK_TRUE;
        depthStencil.depthWriteEnable = VK_TRUE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
        depthStencil.depthBoundsTestEnable = VK_FALSE;
        depthStencil.stencilTestEnable = VK_FALSE;

        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachment.blendEnable = VK_FALSE;

        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.logicOpEnable = VK_FALSE;
        colorBlending.logicOp = VK_LOGIC_OP_COPY;
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &colorBlendAttachment;
        colorBlending.blendConstants[0] = 0.0f;
        colorBlending.blendConstants[1] = 0.0f;
        colorBlending.blendConstants[2] = 0.0f;
        colorBlending.blendConstants[3] = 0.0f;

        std::vector<VkDynamicState> dynamicStates = {
            VK_DYNAMIC_STATE_VIEWPORT,
            VK_DYNAMIC_STATE_SCISSOR
        };
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamicState.pDynamicStates = dynamicStates.data();

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout;

        if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create pipeline layout!");
        }

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = 2;
        pipelineInfo.pStages = shaderStages;
        pipelineInfo.pVertexInputState = &vertexInputInfo;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = pipelineLayout;
        pipelineInfo.renderPass = renderPass;
        pipelineInfo.subpass = 0;
        pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

        if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline) != VK_SUCCESS) {
            throw std::runtime_error("failed to create graphics pipeline!");
        }

        vkDestroyShaderModule(device, fragShaderModule, nullptr);
        vkDestroyShaderModule(device, vertShaderModule, nullptr);
    }

    void createFramebuffers() {
        swapChainFramebuffers.resize(swapChainImageViews.size());

        for (size_t i = 0; i < swapChainImageViews.size(); i++) {
            std::array<VkImageView, 2> attachments = {
                swapChainImageViews[i],
                depthImageView
            };

            VkFramebufferCreateInfo framebufferInfo{};
            framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            framebufferInfo.renderPass = renderPass;
            framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
            framebufferInfo.pAttachments = attachments.data();
            framebufferInfo.width = swapChainExtent.width;
            framebufferInfo.height = swapChainExtent.height;
            framebufferInfo.layers = 1;

            if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &swapChainFramebuffers[i]) != VK_SUCCESS) {
                throw std::runtime_error("failed to create framebuffer!");
            }
        }
    }

    void createCommandPool() {
        QueueFamilyIndices queueFamilyIndices = findQueueFamilies(physicalDevice);

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();

        if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) {
            throw std::runtime_error("failed to create graphics command pool!");
        }
    }

    void createDepthResources() {
        VkFormat depthFormat = findDepthFormat();

        createImage(swapChainExtent.width, swapChainExtent.height, depthFormat, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, depthImage, depthImageMemory);
        depthImageView = createImageView(depthImage, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    VkFormat findSupportedFormat(const std::vector<VkFormat>& candidates, VkImageTiling tiling, VkFormatFeatureFlags features) {
        for (VkFormat format : candidates) {
            VkFormatProperties props;
            vkGetPhysicalDeviceFormatProperties(physicalDevice, format, &props);

            if (tiling == VK_IMAGE_TILING_LINEAR && (props.linearTilingFeatures & features) == features) {
                return format;
            } else if (tiling == VK_IMAGE_TILING_OPTIMAL && (props.optimalTilingFeatures & features) == features) {
                return format;
            }
        }

        throw std::runtime_error("failed to find supported format!");
    }

    VkFormat findDepthFormat() {
        return findSupportedFormat(
            {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT},
            VK_IMAGE_TILING_OPTIMAL,
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
        );
    }

    bool hasStencilComponent(VkFormat format) {
        return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT;
    }

    void createTextureImage() {
        int texWidth = 1, texHeight = 1, texChannels = 4;
        stbi_uc white[] = {255, 255, 255, 255};
        stbi_uc* pixels = sceneMode ? white : stbi_load(texturePath.c_str(), &texWidth, &texHeight, &texChannels, STBI_rgb_alpha);

        if (!pixels) {
            throw std::runtime_error("failed to load texture image!");
        }
        VkDeviceSize imageSize = VkDeviceSize(texWidth) * texHeight * 4;

        VkBuffer stagingBuffer;
        VkDeviceMemory stagingBufferMemory;
        createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer, stagingBufferMemory);

        void* data;
        vkMapMemory(device, stagingBufferMemory, 0, imageSize, 0, &data);
            memcpy(data, pixels, static_cast<size_t>(imageSize));
        vkUnmapMemory(device, stagingBufferMemory);

        if (!sceneMode) stbi_image_free(pixels);

        createImage(texWidth, texHeight, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, textureImage, textureImageMemory);

        transitionImageLayout(textureImage, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            copyBufferToImage(stagingBuffer, textureImage, static_cast<uint32_t>(texWidth), static_cast<uint32_t>(texHeight));
        transitionImageLayout(textureImage, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkFreeMemory(device, stagingBufferMemory, nullptr);
    }

    void createTextureImageView() {
        textureImageView = createImageView(textureImage, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_ASPECT_COLOR_BIT);
    }

    void createTextureSampler() {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physicalDevice, &properties);

        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.anisotropyEnable = VK_TRUE;
        samplerInfo.maxAnisotropy = properties.limits.maxSamplerAnisotropy;
        samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;
        samplerInfo.compareEnable = VK_FALSE;
        samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

        if (vkCreateSampler(device, &samplerInfo, nullptr, &textureSampler) != VK_SUCCESS) {
            throw std::runtime_error("failed to create texture sampler!");
        }
    }

    VkImageView createImageView(VkImage image, VkFormat format, VkImageAspectFlags aspectFlags) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = format;
        viewInfo.subresourceRange.aspectMask = aspectFlags;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = 0;
        viewInfo.subresourceRange.layerCount = 1;

        VkImageView imageView;
        if (vkCreateImageView(device, &viewInfo, nullptr, &imageView) != VK_SUCCESS) {
            throw std::runtime_error("failed to create image view!");
        }

        return imageView;
    }

    void createImage(uint32_t width, uint32_t height, VkFormat format, VkImageTiling tiling, VkImageUsageFlags usage, VkMemoryPropertyFlags properties, VkImage& image, VkDeviceMemory& imageMemory) {
        VkImageCreateInfo imageInfo{};
        imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageInfo.imageType = VK_IMAGE_TYPE_2D;
        imageInfo.extent.width = width;
        imageInfo.extent.height = height;
        imageInfo.extent.depth = 1;
        imageInfo.mipLevels = 1;
        imageInfo.arrayLayers = 1;
        imageInfo.format = format;
        imageInfo.tiling = tiling;
        imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageInfo.usage = usage;
        imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateImage(device, &imageInfo, nullptr, &image) != VK_SUCCESS) {
            throw std::runtime_error("failed to create image!");
        }

        VkMemoryRequirements memRequirements;
        vkGetImageMemoryRequirements(device, image, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);

        if (vkAllocateMemory(device, &allocInfo, nullptr, &imageMemory) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate image memory!");
        }

        vkBindImageMemory(device, image, imageMemory, 0);
    }

    void transitionImageLayout(VkImage image, VkFormat format, VkImageLayout oldLayout, VkImageLayout newLayout) {
        VkCommandBuffer commandBuffer = beginSingleTimeCommands();

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = oldLayout;
        barrier.newLayout = newLayout;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;

        VkPipelineStageFlags sourceStage;
        VkPipelineStageFlags destinationStage;

        if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
            barrier.srcAccessMask = 0;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

            sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
            destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
            barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

            sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
            destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        } else {
            throw std::invalid_argument("unsupported layout transition!");
        }

        vkCmdPipelineBarrier(
            commandBuffer,
            sourceStage, destinationStage,
            0,
            0, nullptr,
            0, nullptr,
            1, &barrier
        );

        endSingleTimeCommands(commandBuffer);
    }

    void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height) {
        VkCommandBuffer commandBuffer = beginSingleTimeCommands();

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {
            width,
            height,
            1
        };

        vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        endSingleTimeCommands(commandBuffer);
    }

    // Startup only: each mesh owns one vertex buffer and one index buffer.
    void createMeshBuffers(const std::vector<Mesh>& meshes) {
        gpuMeshes.resize(meshes.size());
        for (size_t i = 0; i < meshes.size(); ++i) {
            const auto& source = meshes[i];
            auto& destination = gpuMeshes[i];
            destination.indexCount = static_cast<uint32_t>(source.indices.size());
            uploadBuffer(
                source.vertices.data(),
                sizeof(Vertex) * source.vertices.size(),
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                destination.vertexBuffer, destination.vertexMemory);
            uploadBuffer(
                source.indices.data(),
                sizeof(uint32_t) * source.indices.size(),
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                destination.indexBuffer, destination.indexMemory);
        }
    }

    void createUniformBuffers() {
        const VkDeviceSize modelBytes =
            VkDeviceSize(particleCount) * (sceneMode ? sizeof(SceneInstance) : sizeof(glm::mat4));

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physicalDevice, &properties);
        if (modelBytes > properties.limits.maxStorageBufferRange) {
            throw std::runtime_error(
                "instance buffer exceeds device storage-buffer limit");
        }

        for (auto& frame : frames) {
            createBuffer(
                sizeof(FrameTransforms),
                VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                frame.uniformBuffer,
                frame.uniformMemory);

            if (vkMapMemory(
                    device, frame.uniformMemory, 0, sizeof(FrameTransforms),
                    0, &frame.mapped) != VK_SUCCESS) {
                throw std::runtime_error("failed to map camera uniform memory");
            }

            createBuffer(
                modelBytes,
                VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                isCpu() ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
                        : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                frame.modelsBuffer,
                frame.modelsMemory);
            if (isCpu() && vkMapMemory(device, frame.modelsMemory, 0, modelBytes, 0,
                    &frame.modelsMapped) != VK_SUCCESS) {
                throw std::runtime_error("failed to map CPU model buffer");
            }
        }
    }

    void createDescriptorPool() {
        const uint32_t setCount = MAX_FRAMES_IN_FLIGHT;

        std::array<VkDescriptorPoolSize, 3> sizes{};
        sizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        sizes[0].descriptorCount = setCount;
        sizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sizes[1].descriptorCount = setCount;
        sizes[2].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        sizes[2].descriptorCount = setCount;

        VkDescriptorPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        info.poolSizeCount = static_cast<uint32_t>(sizes.size());
        info.pPoolSizes = sizes.data();
        info.maxSets = setCount;

        if (vkCreateDescriptorPool(device, &info, nullptr, &descriptorPool)
                != VK_SUCCESS) {
            throw std::runtime_error("failed to create descriptor pool");
        }
    }

    void createParticleBuffers(const std::vector<gpu_particle>& initialParticles) {
        const VkDeviceSize bytes = VkDeviceSize(particleCount) * sizeof(gpu_particle);
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physicalDevice, &properties);
        const uint32_t groups = (particleCount - 1) / COMPUTE_WORKGROUP_SIZE + 1;
        if (bytes > properties.limits.maxStorageBufferRange ||
            groups > properties.limits.maxComputeWorkGroupCount[0] ||
            COMPUTE_WORKGROUP_SIZE > properties.limits.maxComputeWorkGroupSize[0] ||
            COMPUTE_WORKGROUP_SIZE > properties.limits.maxComputeWorkGroupInvocations) {
            throw std::runtime_error("particle count or workgroup size exceeds GPU limits");
        }

        // Only A needs initial data. Every element of B is written by the first dispatch.
        uploadBuffer(initialParticles.data(), bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            particleBuffers[0].buffer, particleBuffers[0].memory);
        createBuffer(bytes,
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            particleBuffers[1].buffer, particleBuffers[1].memory);
        if (isBarnesHut()) {
            while (leafCount < particleCount) leafCount *= 2;
            const VkDeviceSize treeBytes = VkDeviceSize(leafCount) * 2 * sizeof(BhNode);
            if (treeBytes > properties.limits.maxStorageBufferRange ||
                properties.limits.maxPerStageDescriptorStorageBuffers < 6) {
                throw std::runtime_error("Barnes-Hut tree exceeds GPU storage-buffer limits");
            }
            createBuffer(VkDeviceSize(leafCount) * 8, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, sortKeys.buffer, sortKeys.memory);
            createBuffer(treeBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, treeNodes.buffer, treeNodes.memory);
            createBuffer(VkDeviceSize(particleCount) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, sortedRanks.buffer, sortedRanks.memory);
        }
    }

    void createComputePipeline() {
        std::vector<VkDescriptorSetLayoutBinding> bindings(isBarnesHut() ? 6 : 3);
        for (uint32_t binding = 0; binding < bindings.size(); ++binding) {
            bindings[binding].binding = binding;
            bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bindings[binding].descriptorCount = 1;
            bindings[binding].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }
        VkDescriptorSetLayoutCreateInfo descriptorInfo{};
        descriptorInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        descriptorInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        descriptorInfo.pBindings = bindings.data();
        if (vkCreateDescriptorSetLayout(device, &descriptorInfo, nullptr,
                &computeDescriptorSetLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create compute descriptor layout");
        }

        VkPushConstantRange parameters{};
        parameters.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        parameters.size = isBarnesHut() ? sizeof(BhPushConstants) : sizeof(SimulationPushConstants);
        VkPipelineLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layoutInfo.setLayoutCount = 1;
        layoutInfo.pSetLayouts = &computeDescriptorSetLayout;
        layoutInfo.pushConstantRangeCount = 1;
        layoutInfo.pPushConstantRanges = &parameters;
        if (vkCreatePipelineLayout(device, &layoutInfo, nullptr,
                &computePipelineLayout) != VK_SUCCESS) {
            throw std::runtime_error("failed to create compute pipeline layout");
        }

        const auto code = readFile(isBarnesHut() ? "build/bh_force.spv" : "build/nbody.spv");
        const auto shader = createShaderModule(code);
        VkComputePipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipelineInfo.layout = computePipelineLayout;
        pipelineInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipelineInfo.stage.module = shader;
        pipelineInfo.stage.pName = "main";
        const auto result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
            &pipelineInfo, nullptr, &computePipeline);
        vkDestroyShaderModule(device, shader, nullptr);
        if (result != VK_SUCCESS) {
            throw std::runtime_error("failed to create compute pipeline");
        }
        if (isBarnesHut()) {
            const auto buildCode = readFile("build/bh_build.spv");
            const auto buildShader = createShaderModule(buildCode);
            pipelineInfo.stage.module = buildShader;
            const auto buildResult = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1,
                &pipelineInfo, nullptr, &treeBuildPipeline);
            vkDestroyShaderModule(device, buildShader, nullptr);
            if (buildResult != VK_SUCCESS) throw std::runtime_error("failed to create tree-build pipeline");
        }
    }

    void createComputeDescriptorSets() {
        // Immutable sets for both simulation directions and both frame slots.
        const uint32_t setCount = MAX_FRAMES_IN_FLIGHT * 2;
        VkDescriptorPoolSize size{};
        size.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        const uint32_t bindingCount = isBarnesHut() ? 6 : 3;
        size.descriptorCount = setCount * bindingCount;
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = setCount;
        poolInfo.poolSizeCount = 1;
        poolInfo.pPoolSizes = &size;
        if (vkCreateDescriptorPool(device, &poolInfo, nullptr,
                &computeDescriptorPool) != VK_SUCCESS) {
            throw std::runtime_error("failed to create compute descriptor pool");
        }

        std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT * 2> layouts{};
        layouts.fill(computeDescriptorSetLayout);
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT * 2> sets{};
        VkDescriptorSetAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocation.descriptorPool = computeDescriptorPool;
        allocation.descriptorSetCount = setCount;
        allocation.pSetLayouts = layouts.data();
        if (vkAllocateDescriptorSets(device, &allocation, sets.data()) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate compute descriptor sets");
        }

        for (uint32_t frame = 0; frame < frames.size(); ++frame) {
            for (uint32_t input = 0; input < 2; ++input) {
                const auto set = sets[frame * 2 + input];
                frames[frame].computeDescriptorSets[input] = set;
                std::vector<VkDescriptorBufferInfo> buffers(bindingCount);
                buffers[0] = {particleBuffers[input].buffer, 0,
                    VkDeviceSize(particleCount) * sizeof(gpu_particle)};
                buffers[1] = {particleBuffers[1 - input].buffer, 0,
                    VkDeviceSize(particleCount) * sizeof(gpu_particle)};
                buffers[2] = {frames[frame].modelsBuffer, 0,
                    VkDeviceSize(particleCount) * sizeof(glm::mat4)};
                if (isBarnesHut()) {
                    buffers[3] = {sortKeys.buffer, 0, VkDeviceSize(leafCount) * 8};
                    buffers[4] = {treeNodes.buffer, 0, VkDeviceSize(leafCount) * 2 * sizeof(BhNode)};
                    buffers[5] = {sortedRanks.buffer, 0, VkDeviceSize(particleCount) * 4};
                }
                std::vector<VkWriteDescriptorSet> writes(bindingCount);
                for (uint32_t binding = 0; binding < writes.size(); ++binding) {
                    writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writes[binding].dstSet = set;
                    writes[binding].dstBinding = binding;
                    writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                    writes[binding].descriptorCount = 1;
                    writes[binding].pBufferInfo = &buffers[binding];
                }
                vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                    writes.data(), 0, nullptr);
            }
        }
    }

    void recordSimulation(VkCommandBuffer commandBuffer,
                          const SimulationParameters& simulation) {
        // Also covers the startup upload. Serializes particle snapshots between
        // dispatches: previous writes become visible, and reads finish before reuse.
        VkMemoryBarrier stateDependency{};
        stateDependency.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        stateDependency.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        stateDependency.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        vkCmdPipelineBarrier(commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &stateDependency,
            0, nullptr, 0, nullptr);

        const auto set = frames[currentFrame].computeDescriptorSets[currentParticleBuffer];
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
            computePipelineLayout, 0, 1, &set, 0, nullptr);
        if (isBarnesHut()) {
            BhPushConstants parameters{simulation.stepSize, simulation.softening,
                simulation.gravityConstant, particleCount, simulation.theta, leafCount};
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, treeBuildPipeline);
            // Each phase is separated by a device memory/execution dependency.
            auto buildPhase = [&](uint32_t phase, uint32_t count, uint32_t offset = 0,
                                  uint32_t width = 0, uint32_t stride = 0) {
                parameters.phase = phase; parameters.workCount = count; parameters.offset = offset;
                parameters.sortWidth = width; parameters.sortStride = stride;
                vkCmdPushConstants(commandBuffer, computePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                    0, sizeof(parameters), &parameters);
                vkCmdDispatch(commandBuffer, (count - 1) / COMPUTE_WORKGROUP_SIZE + 1, 1, 1);
                VkMemoryBarrier dependency{};
                dependency.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
                dependency.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                dependency.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
                vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &dependency, 0, nullptr, 0, nullptr);
            };
            auto reduceLevels = [&]() {
                for (uint32_t offset = leafCount / 2; offset > 0; offset /= 2) buildPhase(1, offset, offset);
            };
            buildPhase(0, leafCount); // Global bounds from unsorted particles.
            reduceLevels();
            buildPhase(2, leafCount); // Generate Morton keys.
            for (uint32_t width = 2; width <= leafCount; width *= 2) {
                for (uint32_t stride = width / 2; stride > 0; stride /= 2) {
                    buildPhase(3, leafCount, 0, width, stride);
                }
            }
            buildPhase(4, leafCount); // Sorted leaves and inverse rank lookup.
            reduceLevels(); // Centers of mass, masses and bounds.
            if (timingsEnabled() && timestampPool != VK_NULL_HANDLE) {
                vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    timestampPool, currentFrame * 4 + 1);
            }
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);
            vkCmdPushConstants(commandBuffer, computePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                0, sizeof(parameters), &parameters);
        } else {
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, computePipeline);
            const SimulationPushConstants parameters = {
                simulation.stepSize, simulation.softening, simulation.gravityConstant, particleCount};
            vkCmdPushConstants(commandBuffer, computePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                0, sizeof(parameters), &parameters);
            if (timingsEnabled() && timestampPool != VK_NULL_HANDLE) {
                vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    timestampPool, currentFrame * 4 + 1);
            }
        }
        vkCmdDispatch(commandBuffer, (particleCount - 1) / COMPUTE_WORKGROUP_SIZE + 1, 1, 1);

        if (timingsEnabled()) {
            if (timestampPool != VK_NULL_HANDLE) {
                vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                    timestampPool, currentFrame * 4 + 2);
            }
        }

        // Same model buffer, two consumers: compute writes it, vertex shader reads it.
        VkBufferMemoryBarrier modelsDependency{};
        modelsDependency.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        modelsDependency.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        modelsDependency.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        modelsDependency.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        modelsDependency.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        modelsDependency.buffer = frames[currentFrame].modelsBuffer;
        modelsDependency.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 0, nullptr,
            1, &modelsDependency, 0, nullptr);
    }

    void createDescriptorSets() {
        std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts;
        layouts.fill(descriptorSetLayout);

        VkDescriptorSetAllocateInfo allocation{};
        allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocation.descriptorPool = descriptorPool;
        allocation.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
        allocation.pSetLayouts = layouts.data();
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> sets{};
        if (vkAllocateDescriptorSets(device, &allocation, sets.data()) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate descriptor sets");
        }

        for (size_t i = 0; i < frames.size(); ++i) {
            auto& frame = frames[i];
            frame.descriptorSet = sets[i];

            VkDescriptorBufferInfo models{};
            models.buffer = frame.modelsBuffer;
            models.offset = 0;
            models.range = VkDeviceSize(particleCount) * (sceneMode ? sizeof(SceneInstance) : sizeof(glm::mat4));
            std::array<VkWriteDescriptorSet, 3> writes{};

            VkDescriptorImageInfo image{};
            image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            image.imageView = textureImageView;
            image.sampler = textureSampler;

            VkDescriptorBufferInfo buffer{};
            buffer.buffer = frame.uniformBuffer;
            buffer.offset = 0;
            buffer.range = sizeof(FrameTransforms);

            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = frame.descriptorSet;
            writes[0].dstBinding = 0;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].descriptorCount = 1;
            writes[0].pBufferInfo = &buffer;

            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = frame.descriptorSet;
            writes[1].dstBinding = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[1].descriptorCount = 1;
            writes[1].pImageInfo = &image;

            writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[2].dstSet = frame.descriptorSet;
            writes[2].dstBinding = 2;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[2].descriptorCount = 1;
            writes[2].pBufferInfo = &models;

            vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);
        }
    }

    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags properties, VkBuffer& buffer, VkDeviceMemory& bufferMemory) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = size;
        bufferInfo.usage = usage;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(device, &bufferInfo, nullptr, &buffer) != VK_SUCCESS) {
            throw std::runtime_error("failed to create buffer!");
        }

        VkMemoryRequirements memRequirements;
        vkGetBufferMemoryRequirements(device, buffer, &memRequirements);

        VkMemoryAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        allocInfo.allocationSize = memRequirements.size;
        allocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, properties);

        if (vkAllocateMemory(device, &allocInfo, nullptr, &bufferMemory) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate buffer memory!");
        }

        vkBindBufferMemory(device, buffer, bufferMemory, 0);
    }

    VkCommandBuffer beginSingleTimeCommands() {
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandPool = commandPool;
        allocInfo.commandBufferCount = 1;

        VkCommandBuffer commandBuffer;
        if (vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate upload/readback command buffer");
        }

        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

        if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
            throw std::runtime_error("failed to begin upload/readback command buffer");
        }

        return commandBuffer;
    }

    void endSingleTimeCommands(VkCommandBuffer commandBuffer) {
        if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
            throw std::runtime_error("failed to end upload/readback command buffer");
        }

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffer;

        if (vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS ||
            vkQueueWaitIdle(graphicsQueue) != VK_SUCCESS) {
            throw std::runtime_error("failed to submit upload/readback commands");
        }

        vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
    }

    void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size) {
        VkCommandBuffer commandBuffer = beginSingleTimeCommands();

        VkBufferCopy copyRegion{};
        copyRegion.size = size;
        vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

        endSingleTimeCommands(commandBuffer);
    }

    uint32_t findMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
        VkPhysicalDeviceMemoryProperties memProperties;
        vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

        for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
            if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }

        throw std::runtime_error("failed to find suitable memory type!");
    }

    void createCommandBuffers() {
        commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);

        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = commandPool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        allocInfo.commandBufferCount = (uint32_t) commandBuffers.size();

        if (vkAllocateCommandBuffers(device, &allocInfo, commandBuffers.data()) != VK_SUCCESS) {
            throw std::runtime_error("failed to allocate command buffers!");
        }
    }

    void recordCommandBuffer(
        VkCommandBuffer commandBuffer,
        uint32_t imageIndex,
        const SimulationParameters& simulation) {
        VkCommandBufferBeginInfo beginInfo{};
        beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

        if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) {
            throw std::runtime_error("failed to begin recording command buffer!");
        }

        if (timingsEnabled()) {
            if (timestampPool != VK_NULL_HANDLE) {
                const uint32_t firstQuery = currentFrame * 4;
                vkCmdResetQueryPool(commandBuffer, timestampPool, firstQuery, 4);
                vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                    timestampPool, firstQuery);
            }
        }

        if (!isCpu()) recordSimulation(commandBuffer, simulation);
        else if (timingsEnabled() && timestampPool != VK_NULL_HANDLE) {
            vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestampPool, currentFrame * 4 + 1);
            vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timestampPool, currentFrame * 4 + 2);
        }

        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = renderPass;
        renderPassInfo.framebuffer = swapChainFramebuffers[imageIndex];
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = swapChainExtent;

        std::array<VkClearValue, 2> clearValues{};
        clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}};
        clearValues[1].depthStencil = {1.0f, 0};

        renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
        renderPassInfo.pClearValues = clearValues.data();

        vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);

        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(swapChainExtent.width);
        viewport.height = static_cast<float>(swapChainExtent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = swapChainExtent;
        vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

        // All draws use the same view/projection and (currently) texture.
        vkCmdBindDescriptorSets(
            commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout,
            0, 1, &frames[currentFrame].descriptorSet, 0, nullptr);

        {
            const auto& mesh = gpuMeshes[0];
            recordMeshDraw(
                commandBuffer,
                mesh.vertexBuffer,
                mesh.indexBuffer,
                sceneMode ? static_cast<uint32_t>(sceneObjects->size()) : particleCount,
                mesh.indexCount);
        }

        vkCmdEndRenderPass(commandBuffer);

        if (timingsEnabled()) {
            if (timestampPool != VK_NULL_HANDLE) {
                vkCmdWriteTimestamp(commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                    timestampPool, currentFrame * 4 + 3);
            }
        }

        if (!capturePath.empty()) {
            VkImageMemoryBarrier image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            image.srcAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT; image.dstAccessMask=VK_ACCESS_TRANSFER_READ_BIT;
            image.oldLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR; image.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            image.srcQueueFamilyIndex=image.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            image.image=swapChainImages[imageIndex];
            image.subresourceRange.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT; image.subresourceRange.levelCount=1; image.subresourceRange.layerCount=1;
            vkCmdPipelineBarrier(commandBuffer,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&image);
            VkBufferImageCopy copy{};copy.imageSubresource.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT;copy.imageSubresource.layerCount=1;
            copy.imageExtent={swapChainExtent.width,swapChainExtent.height,1};
            vkCmdCopyImageToBuffer(commandBuffer,swapChainImages[imageIndex],VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,screenshot.buffer,1,&copy);
            image.srcAccessMask=VK_ACCESS_TRANSFER_READ_BIT;image.dstAccessMask=0;
            image.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;image.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            vkCmdPipelineBarrier(commandBuffer,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&image);
            VkMemoryBarrier host{VK_STRUCTURE_TYPE_MEMORY_BARRIER};host.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;host.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
            vkCmdPipelineBarrier(commandBuffer,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&host,0,nullptr,0,nullptr);
        }
        if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
            throw std::runtime_error("failed to record command buffer!");
        }
    }

    void createProfilingQueries() {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physicalDevice, &properties);
        uint32_t queueCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueCount, nullptr);
        std::vector<VkQueueFamilyProperties> queues(queueCount);
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueCount, queues.data());
        const auto graphicsFamily = findQueueFamilies(physicalDevice).graphicsFamily.value();
        timestampValidBits = queues[graphicsFamily].timestampValidBits;
        timestampPeriodNs = properties.limits.timestampPeriod;

        if constexpr (profiling::ENABLED) std::cout << "[profile] GPU=" << properties.deviceName
                  << " timestamp_bits=" << timestampValidBits
                  << " timestamp_period_ns=" << timestampPeriodNs
                  << " seed=" << profiling::RANDOM_SEED
                  << " warmup_frames=" << profiling::WARMUP_FRAMES
                  << " report_frames=" << profiling::REPORT_FRAMES << std::endl;
        if (timestampValidBits == 0) {
            std::cout << "[profile] GPU timestamps unsupported; CPU timers remain enabled.\n";
            return;
        }

        VkQueryPoolCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        info.queryType = VK_QUERY_TYPE_TIMESTAMP;
        info.queryCount = MAX_FRAMES_IN_FLIGHT * 4;
        if (vkCreateQueryPool(device, &info, nullptr, &timestampPool) != VK_SUCCESS) {
            throw std::runtime_error("failed to create profiling timestamp pool");
        }
    }

    // Called only after this frame slot's fence completes (or device idle at
    // shutdown). No WAIT_BIT, extra fence, or per-frame device-idle is introduced.
    void collectGpuTiming(uint32_t frameIndex) {
        auto& frame = frames[frameIndex];
        if (timestampPool == VK_NULL_HANDLE || !frame.timestampsPending) return;

        std::array<uint64_t, 4> ticks{};
        const VkResult result = vkGetQueryPoolResults(
            device, timestampPool, frameIndex * 4, 4, sizeof(ticks),
            ticks.data(), sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
        frame.timestampsPending = false;
        double renderMs = 0.0;
        double computeMs = 0.0;
        if (result == VK_SUCCESS) {
            const uint64_t mask = timestampValidBits == 64
                ? std::numeric_limits<uint64_t>::max()
                : (uint64_t(1) << timestampValidBits) - 1;
            auto duration = [&](uint32_t first, uint32_t last) {
                return double((ticks[last] - ticks[first]) & mask) * timestampPeriodNs / 1.0e6;
            };
            frame.timings.gpuBuildMs = !isCpu() && isBarnesHut() ? duration(0, 1) : 0.0;
            frame.timings.gpuForceMs = isCpu() ? 0.0 : duration(1, 2);
            frame.timings.gpuRenderMs = duration(2, 3);
            if (sceneMode) { completedSceneTiming = frame.timings; completedSceneReady = true; }
            computeMs = frame.timings.gpuBuildMs + frame.timings.gpuForceMs;
            renderMs = frame.timings.gpuRenderMs;
        } else if (result != VK_NOT_READY) {
            std::cerr << "[profile] GPU timestamp read failed: " << result << '\n';
        }
        if constexpr (profiling::ENABLED) {
            profiling::profiler().gpuResult(frame.profilingFrameId, result == VK_SUCCESS, computeMs, renderMs);
        }
    }

    // Presentation may outlive a frame fence, so use one semaphore per swapchain image.
    void createRenderFinishedSemaphores() {
        renderFinishedSemaphores.resize(swapChainImages.size());

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        for (auto& semaphore : renderFinishedSemaphores) {
            if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &semaphore) != VK_SUCCESS) {
                throw std::runtime_error("failed to create presentation semaphore!");
            }
        }
    }

    void createSyncObjects() {
        imageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
        inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);
        createRenderFinishedSemaphores();

        VkSemaphoreCreateInfo semaphoreInfo{};
        semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailableSemaphores[i]) != VK_SUCCESS ||
                vkCreateFence(device, &fenceInfo, nullptr, &inFlightFences[i]) != VK_SUCCESS) {
                throw std::runtime_error("failed to create synchronization objects for a frame!");
            }
        }
    }

    void updateFrameData(
        uint32_t frameIndex,
        const FrameTransforms& transforms) {

        profiling::ScopedTimer timer(profiling::Stage::MatrixCopy);
        auto& frame = frames[frameIndex];
        memcpy(frame.mapped, &transforms, sizeof(transforms));
        if (sceneMode) {
            if (!sceneObjects->empty()) memcpy(frame.modelsMapped, sceneObjects->data(), sceneObjects->size() * sizeof(SceneInstance));
        } else if (isCpu()) {
            memcpy(frame.modelsMapped, cpuSimulation->models().data(), size_t(particleCount) * sizeof(glm::mat4));
        }
    }

    void drawFrameInternal(
        const FrameTransforms& transforms,
        const SimulationParameters& simulation) {
        const bool measure = timingsEnabled();
        auto mark = [&]() { return measure ? FrameClock::now() : FrameClock::time_point{}; };
        auto since = [&](FrameClock::time_point start) { return measure ? frameElapsed(start) : 0.0; };
        auto cpuStart = mark();
        {
            profiling::ScopedTimer timer(profiling::Stage::FenceWait);
            if (vkWaitForFences(device, 1, &inFlightFences[currentFrame], VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
                throw std::runtime_error("failed to wait for frame fence");
            }
        }
        const double waitedMs = since(cpuStart);
        if (timingsEnabled()) {
            profiling::ScopedTimer timer(profiling::Stage::QueryRead);
            collectGpuTiming(currentFrame);
        }
        auto& timing = frames[currentFrame].timings;
        timing = FrameTimings{};
        timing.fenceWaitMs = waitedMs;

        uint32_t imageIndex;
        VkResult result;
        cpuStart = mark();
        {
            profiling::ScopedTimer timer(profiling::Stage::Acquire);
            result = vkAcquireNextImageKHR(device, swapChain, UINT64_MAX, imageAvailableSemaphores[currentFrame], VK_NULL_HANDLE, &imageIndex);
        }
        timing.acquireMs = since(cpuStart);

        if (result == VK_ERROR_OUT_OF_DATE_KHR) {
            recreateSwapChain();
            return;
        } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
            throw std::runtime_error("failed to acquire swap chain image!");
        }

        // The fence guarantees this frame slot is no longer being read by the GPU.
        if (!capturePath.empty()) prepareScreenshot();
        if (isCpu() && !sceneMode) {
            const auto cpu = cpuSimulation->step(simulation, measure);
            timing.cpuBuildMs = cpu.buildMs;
            timing.cpuForceMs = cpu.forceMs;
            timing.cpuIntegrateMs = cpu.integrateMs;
        }
        cpuStart = mark();
        updateFrameData(currentFrame, transforms);
        timing.uploadMs = since(cpuStart);

        cpuStart = mark();
        {
            profiling::ScopedTimer timer(profiling::Stage::RecordCommands);
            vkResetFences(device, 1, &inFlightFences[currentFrame]);
            vkResetCommandBuffer(commandBuffers[currentFrame], /*VkCommandBufferResetFlagBits*/ 0);
            recordCommandBuffer(commandBuffers[currentFrame], imageIndex, simulation);
        }
        timing.recordMs = since(cpuStart);

        VkSubmitInfo submitInfo{};
        submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

        VkSemaphore waitSemaphores[] = {imageAvailableSemaphores[currentFrame]};
        VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        submitInfo.waitSemaphoreCount = 1;
        submitInfo.pWaitSemaphores = waitSemaphores;
        submitInfo.pWaitDstStageMask = waitStages;

        submitInfo.commandBufferCount = 1;
        submitInfo.pCommandBuffers = &commandBuffers[currentFrame];

        VkSemaphore signalSemaphores[] = {renderFinishedSemaphores[imageIndex]};
        submitInfo.signalSemaphoreCount = 1;
        submitInfo.pSignalSemaphores = signalSemaphores;

        cpuStart = mark();
        {
            profiling::ScopedTimer timer(profiling::Stage::Submit);
            if (vkQueueSubmit(graphicsQueue, 1, &submitInfo, inFlightFences[currentFrame]) != VK_SUCCESS) {
                throw std::runtime_error("failed to submit draw command buffer!");
            }
        }
        timing.submitMs = since(cpuStart);
        if (!capturePath.empty()) {
            if (vkWaitForFences(device,1,&inFlightFences[currentFrame],VK_TRUE,UINT64_MAX)!=VK_SUCCESS) throw std::runtime_error("screenshot fence failed");
            saveScreenshot();
        }
        // Advance only after a successful submission (not on skipped acquire frames).
        if (!isCpu()) currentParticleBuffer = 1 - currentParticleBuffer;
        lastSubmittedFrame = currentFrame;
        if (timingsEnabled()) {
            if (timestampPool != VK_NULL_HANDLE) {
                auto& frame = frames[currentFrame];
                frame.timestampsPending = true;
                if constexpr (profiling::ENABLED) {
                    frame.profilingFrameId = profiling::profiler().currentFrameId();
                    profiling::profiler().expectGpu(frame.profilingFrameId);
                }
            }
        }

        VkPresentInfoKHR presentInfo{};
        presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

        presentInfo.waitSemaphoreCount = 1;
        presentInfo.pWaitSemaphores = signalSemaphores;

        VkSwapchainKHR swapChains[] = {swapChain};
        presentInfo.swapchainCount = 1;
        presentInfo.pSwapchains = swapChains;

        presentInfo.pImageIndices = &imageIndex;

        cpuStart = mark();
        {
            profiling::ScopedTimer timer(profiling::Stage::Present);
            result = vkQueuePresentKHR(presentQueue, &presentInfo);
        }
        timing.presentMs = since(cpuStart);

        if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR || framebufferResized) {
            framebufferResized = false;
            recreateSwapChain();
        } else if (result != VK_SUCCESS) {
            throw std::runtime_error("failed to present swap chain image!");
        }

        currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
    }

    void prepareScreenshot() {
        const VkDeviceSize bytes=VkDeviceSize(swapChainExtent.width)*swapChainExtent.height*4;
        if (bytes==screenshotBytes) return;
        if(screenshotMapped) vkUnmapMemory(device,screenshot.memory);
        if(screenshot.buffer) vkDestroyBuffer(device,screenshot.buffer,nullptr);
        if(screenshot.memory) vkFreeMemory(device,screenshot.memory,nullptr);
        screenshot={};screenshotMapped=nullptr;
        createBuffer(bytes,VK_BUFFER_USAGE_TRANSFER_DST_BIT,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,screenshot.buffer,screenshot.memory);
        if(vkMapMemory(device,screenshot.memory,0,bytes,0,&screenshotMapped)!=VK_SUCCESS) throw std::runtime_error("screenshot mapping failed");
        screenshotBytes=bytes;
    }
    void saveScreenshot() {
        if(swapChainImageFormat!=VK_FORMAT_B8G8R8A8_SRGB && swapChainImageFormat!=VK_FORMAT_B8G8R8A8_UNORM &&
            swapChainImageFormat!=VK_FORMAT_R8G8B8A8_SRGB && swapChainImageFormat!=VK_FORMAT_R8G8B8A8_UNORM) throw std::runtime_error("unsupported screenshot format");
        const bool bgra=swapChainImageFormat==VK_FORMAT_B8G8R8A8_SRGB||swapChainImageFormat==VK_FORMAT_B8G8R8A8_UNORM;
        unsigned char header[54]{};header[0]='B';header[1]='M';header[10]=54;header[14]=40;header[26]=1;header[28]=32;
        auto word=[&](int offset,uint32_t value) {for(int i=0;i<4;++i) header[offset+i]=static_cast<unsigned char>(value>>(i*8));};
        word(2,uint32_t(screenshotBytes)+54);word(18,swapChainExtent.width);word(22,swapChainExtent.height);word(34,uint32_t(screenshotBytes));
        std::ofstream file(capturePath,std::ios::binary);
        if(!file) throw std::runtime_error("cannot open screenshot output");
        file.write(reinterpret_cast<const char*>(header),sizeof(header));
        const auto* pixels=static_cast<const unsigned char*>(screenshotMapped);
        std::vector<unsigned char> row(size_t(swapChainExtent.width)*4);
        for(int y=int(swapChainExtent.height)-1;y>=0;--y) {
            std::memcpy(row.data(),pixels+size_t(y)*row.size(),row.size());
            if(!bgra) for(size_t x=0;x<row.size();x+=4) std::swap(row[x],row[x+2]);
            file.write(reinterpret_cast<const char*>(row.data()),row.size());
        }
        if(!file) throw std::runtime_error("screenshot write failed");
        std::cout<<"Saved screenshot "<<capturePath<<'\n';capturePath.clear();
    }

    VkShaderModule createShaderModule(const std::vector<char>& code) {
        VkShaderModuleCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        createInfo.codeSize = code.size();
        createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

        VkShaderModule shaderModule;
        if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
            throw std::runtime_error("failed to create shader module!");
        }

        return shaderModule;
    }

    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats) {
        for (const auto& availableFormat : availableFormats) {
            if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB && availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                return availableFormat;
            }
        }

        return availableFormats[0];
    }

    VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes) {
        if (options.uncappedPresentation && std::find(availablePresentModes.begin(),
                availablePresentModes.end(), VK_PRESENT_MODE_IMMEDIATE_KHR) != availablePresentModes.end()) {
            return VK_PRESENT_MODE_IMMEDIATE_KHR;
        }
        for (const auto& availablePresentMode : availablePresentModes) {
            if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
                return availablePresentMode;
            }
        }

        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) {
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
            return capabilities.currentExtent;
        } else {
            int width, height;
            glfwGetFramebufferSize(window, &width, &height);

            VkExtent2D actualExtent = {
                static_cast<uint32_t>(width),
                static_cast<uint32_t>(height)
            };

            actualExtent.width = std::clamp(actualExtent.width, capabilities.minImageExtent.width, capabilities.maxImageExtent.width);
            actualExtent.height = std::clamp(actualExtent.height, capabilities.minImageExtent.height, capabilities.maxImageExtent.height);

            return actualExtent;
        }
    }

    SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device) {
        SwapChainSupportDetails details;

        vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);

        uint32_t formatCount;
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);

        if (formatCount != 0) {
            details.formats.resize(formatCount);
            vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, details.formats.data());
        }

        uint32_t presentModeCount;
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);

        if (presentModeCount != 0) {
            details.presentModes.resize(presentModeCount);
            vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, details.presentModes.data());
        }

        return details;
    }

    bool isDeviceSuitable(VkPhysicalDevice device) {
        QueueFamilyIndices indices = findQueueFamilies(device);

        bool extensionsSupported = checkDeviceExtensionSupport(device);

        bool swapChainAdequate = false;
        if (extensionsSupported) {
            SwapChainSupportDetails swapChainSupport = querySwapChainSupport(device);
            swapChainAdequate = !swapChainSupport.formats.empty() && !swapChainSupport.presentModes.empty();
        }

        VkPhysicalDeviceFeatures supportedFeatures;
        vkGetPhysicalDeviceFeatures(device, &supportedFeatures);

        return indices.isComplete() && extensionsSupported && swapChainAdequate  && supportedFeatures.samplerAnisotropy;
    }

    bool checkDeviceExtensionSupport(VkPhysicalDevice device) {
        uint32_t extensionCount;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);

        std::vector<VkExtensionProperties> availableExtensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());

        std::set<std::string> requiredExtensions(deviceExtensions.begin(), deviceExtensions.end());

        for (const auto& extension : availableExtensions) {
            requiredExtensions.erase(extension.extensionName);
        }

        return requiredExtensions.empty();
    }

    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) {
        QueueFamilyIndices indices;

        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);

        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());

        int i = 0;
        for (const auto& queueFamily : queueFamilies) {
            const VkQueueFlags required = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
            if ((queueFamily.queueFlags & required) == required) {
                indices.graphicsFamily = i;
            }

            VkBool32 presentSupport = false;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);

            if (presentSupport) {
                indices.presentFamily = i;
            }

            if (indices.isComplete()) {
                break;
            }

            i++;
        }

        return indices;
    }

    std::vector<const char*> getRequiredExtensions() {
        uint32_t glfwExtensionCount = 0;
        const char** glfwExtensions;
        glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

        std::vector<const char*> extensions(glfwExtensions, glfwExtensions + glfwExtensionCount);

        if (enableValidationLayers) {
            extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }

        return extensions;
    }

    bool checkValidationLayerSupport() {
        uint32_t layerCount;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);

        std::vector<VkLayerProperties> availableLayers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

        for (const char* layerName : validationLayers) {
            bool layerFound = false;

            for (const auto& layerProperties : availableLayers) {
                if (strcmp(layerName, layerProperties.layerName) == 0) {
                    layerFound = true;
                    break;
                }
            }

            if (!layerFound) {
                return false;
            }
        }

        return true;
    }

    static std::vector<char> readFile(const std::string& filename) {
        std::ifstream file(filename, std::ios::ate | std::ios::binary);

        if (!file.is_open()) {
            throw std::runtime_error("failed to open file!");
        }

        size_t fileSize = (size_t) file.tellg();
        std::vector<char> buffer(fileSize);

        file.seekg(0);
        file.read(buffer.data(), fileSize);

        file.close();

        return buffer;
    }

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity, VkDebugUtilsMessageTypeFlagsEXT messageType, const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData, void* pUserData) {
        std::cerr << "validation layer: " << pCallbackData->pMessage << std::endl;

        return VK_FALSE;
    }
};

Renderer::Renderer(
    uint32_t width,
    uint32_t height,
    const char* title,
    const std::vector<Mesh>& meshes,
    const std::vector<gpu_particle>& initialParticles,
    const std::string& texturePath,
    const RendererOptions& options)
    : impl(std::make_unique<Impl>(width, height, title, meshes, initialParticles, texturePath, options)) {
}

Renderer::~Renderer() = default;

Renderer::Renderer(uint32_t width, uint32_t height, const char *title, const Mesh &mesh, uint32_t capacity,
                   bool measureTimings, const SceneShaders &shaders)
    : impl(std::make_unique<Impl>(
          width, height, title, std::vector<Mesh>{mesh}, std::vector<gpu_particle>{}, "",
          [measureTimings, &shaders] {
              RendererOptions o;
              o.measureTimings = measureTimings;
              o.sceneShaders = shaders;
              return o;
          }(),
          capacity)) {}

void Renderer::drawScene(const FrameTransforms &transforms, const std::vector<SceneInstance> &objects) {
    impl->drawScene(transforms, objects);
}
bool Renderer::readCompletedSceneTiming(FrameTimings& result) { return impl->readCompletedSceneTiming(result); }

void Renderer::captureNextFrame(const std::string& path) {impl->captureNextFrame(path);}

ComputeContext Renderer::computeContext() const { return impl->computeContext(); }

bool Renderer::shouldClose() const {
    return impl->shouldClose();
}

void Renderer::pollEvents() const {
    impl->pollEvents();
}

float Renderer::aspectRatio() const {
    return impl->aspectRatio();
}

void Renderer::drawFrame(
    const FrameTransforms& transforms,
    const SimulationParameters& simulation) {
    impl->drawFrame(transforms, simulation);
}

std::vector<gpu_particle> Renderer::readParticles() {
    return impl->readParticles();
}

FrameTimings Renderer::finishFrame() { return impl->finishFrame(); }

void* Renderer::nativeWindowHandle() const {
    return impl->nativeWindowHandle();
}
