#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#include <iostream>
#include <stdexcept>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <optional>
#include <set>
#include <cstdint> 
#include <limits> 
#include <algorithm> 
#include <fstream>


// Validation Layers (enblaed or disabled at compile time, based on NDEBUG flag)
// #######################################################################################
const std::vector<const char*> validationLayers = { "VK_LAYER_KHRONOS_validation" };
const bool enableValidationLayers = true;

// List of Required Device Extensions (for the swap chain)
// #######################################################################################
const std::vector<const char*> deviceExtensions = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME
};


class HelloTriangleApplication {
private:
    GLFWwindow* window;             // Stores pointer to the window we writing stuff to
    const uint32_t WIDTH = 800;     // Window Width
    const uint32_t HEIGHT = 600;    // Window Height
    VkInstance instance;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;   // This is the GPU Physical Device we are using (struct of info about it)
    struct QueueFamilyIndices {     // Struct that holds the different types of Qs
        std::optional<uint32_t> graphicsFamily;
        std::optional<uint32_t> presentFamily;
        bool isComplete() { return graphicsFamily.has_value() && presentFamily.has_value(); }
    };
    VkDevice device;                // Logical Device 
    VkQueue graphicsQueue;          // Handler to the Graphics Q (so we can interface it)
    VkQueue presentQueue;           // Handler to the Presnet Q (present shit to the screen)
    VkSurfaceKHR surface;           // surface that will be drawn to 
    struct SwapChainSupportDetails {                    // Information on what this physical device supports wrt swap chain
        VkSurfaceCapabilitiesKHR capabilities;          // this shit doesnt actually have any info, its just a type we return and use
        std::vector<VkSurfaceFormatKHR> formats;
        std::vector<VkPresentModeKHR> presentModes;
    };
    VkSwapchainKHR swapChain;                   // Swap Chain
    std::vector<VkImage> swapChainImages;       // Vector of images in the Swap Chain
    VkFormat swapChainImageFormat;              // Image Format 
    VkExtent2D swapChainExtent;                 // Chain Extent 
    std::vector<VkImageView> swapChainImageViews;   // Vector to store the VIEWs into Images in the Swap Chain 
    VkPipelineLayout pipelineLayout;            // Struct defining the pipeline layout 
    VkRenderPass renderPass;                    // Struct defining the render pass
    VkPipeline graphicsPipeline;                // The Graphics Peipline (top dawg)
    std::vector<VkFramebuffer> swapChainFramebuffers;   // Hold the frame buffers 
    VkCommandPool commandPool;                  // Command Pools (manage Command Buffers)
    VkCommandBuffer commandBuffer;              // Command Buffers 
    // Schronization 
    VkSemaphore imageAvailableSemaphore;        // We'll need one semaphore to signal that an image has been acquired from the swapchain and is ready for rendering
    VkSemaphore renderFinishedSemaphore;        // another one to signal that rendering has finished and presentation can happen
    VkFence inFlightFence;                      // and a fence to make sure only one frame is rendering at a time.

public:
    void run() {
        initWindow();   // Initalized the window that we write to
        initVulkan();
        mainLoop();
        cleanup();
    }

private:
    void initWindow() {
        glfwInit();

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);   // Init the GLFW library. Tell it not to use OpenGL API (we doing vulkan)
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);     // Diabling resizable windows b/c they require extra work

        window = glfwCreateWindow(WIDTH, HEIGHT, "Vulkan", nullptr, nullptr);    // Args = Width, Height, Title, Monitor*, OpenGL*. 
    }


    // Draw the frame to the window 
        // At a high level, rendering a frame in Vulkan consists of a common set of steps:
            // 1) Wait for the previous frame to finish
            // 2) Acquire an image from the swap chain
            // 3) Record a command buffer which draws the scene onto that image
            // 4) Submit the recorded command buffer
            // 5) Present the swap chain image
            // While we will expand the drawing function in later chapters, for now this is the core of our render loop.
        // Shchronization 
            // A core design philosophy in Vulkan is that synchronization of execution on the GPU is explicit. 
            // In this chapter there are a number of events that we need to order explicitly because they happen on the GPU, such as:
                // - Acquire an image from the swap chain
                // - Execute commands that draw onto the acquired image
                // - Present that image to the screen for presentation, returning it to the swapchain
            // Each of these events is set in motion using a single function call, but are all executed asynchronously. The function calls will return before the operations are actually finished.
            // In summary, semaphores are used to specify the execution order of operations on the GPU while fences are used to keep the CPU and GPU in sync with each-other.
    // ----------------------------------------------------------------------------------------------------------------------------------
    void drawFrame(){
        // At the start of the frame, we want to wait until the previous frame has finished, so that the command buffer and semaphores are available to use. To do that, we call vkWaitForFences:
            vkWaitForFences(device, 1, &inFlightFence, VK_TRUE, UINT64_MAX);
        // Manually Reset the fench after done waiting on it 
            vkResetFences(device, 1, &inFlightFence);
        // Aquire an Image from the Swap Chain (based on logical device and swap chain, and semphaore image vaiable)
            uint32_t imageIndex;    // want the index of the image so we know who we got 
            vkAcquireNextImageKHR(device, swapChain, UINT64_MAX, imageAvailableSemaphore, VK_NULL_HANDLE, &imageIndex);
        // With the imageIndex specifying the swap chain image to use in hand, we can now record the command buffer.
            vkResetCommandBuffer(commandBuffer, 0);             // Reset the command buffer 
            recordCommandBuffer(commandBuffer, imageIndex);     // Record command buffer for this image index 
        // Submitting the Command Buffer 
            // Configureation Info: 
                VkSubmitInfo submitInfo{};
                submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
                VkSemaphore waitSemaphores[] = {imageAvailableSemaphore};       // Wait on this sempahore 
                VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
                submitInfo.waitSemaphoreCount = 1;
                submitInfo.pWaitSemaphores = waitSemaphores;
                submitInfo.pWaitDstStageMask = waitStages;
                submitInfo.commandBufferCount = 1;
                submitInfo.pCommandBuffers = &commandBuffer;
                VkSemaphore signalSemaphores[] = {renderFinishedSemaphore};     // Singal this sempahore when done 
                submitInfo.signalSemaphoreCount = 1;
                submitInfo.pSignalSemaphores = signalSemaphores;
            // Submit it 
            if (vkQueueSubmit(graphicsQueue, 1, &submitInfo, inFlightFence) != VK_SUCCESS) { throw std::runtime_error("failed to submit draw command buffer!"); }
        // Presentation: 
            VkPresentInfoKHR presentInfo{};
            presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
            presentInfo.waitSemaphoreCount = 1;
            presentInfo.pWaitSemaphores = signalSemaphores; // wait for singals semphahore before presenting to screen (b/c when singal seamphore is done that means the GPUs computation for our triangle is done, therefore its ready to be writen to the screen)
            // Swap Chain stuff: 
            VkSwapchainKHR swapChains[] = {swapChain};
            presentInfo.swapchainCount = 1;
            presentInfo.pSwapchains = swapChains;
            presentInfo.pImageIndices = &imageIndex;
            presentInfo.pResults = nullptr; // Optional
            // Actually Presneit it! YOu should now have triangel!!!!!!!!!!!!!!!!!! 
            vkQueuePresentKHR(presentQueue, &presentInfo);
    }
    // ----------------------------------------------------------------------------------------------------------------------------------

    
    // Need shcrnization between CPU and GPU 
    // ----------------------------------------------------------------------------------------------------------------------------------
    void createSyncObjects(){
        // Semphore information: 
            VkSemaphoreCreateInfo semaphoreInfo{};
            semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        // Fench information: 
            VkFenceCreateInfo fenceInfo{};
            fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT; // start in singals state b/c otherwise on cold start, wait forever for singal that never comes 
        // Create 2 semphaore, and 1 fenche: 
        if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &imageAvailableSemaphore) != VK_SUCCESS ||
            vkCreateSemaphore(device, &semaphoreInfo, nullptr, &renderFinishedSemaphore) != VK_SUCCESS ||
            vkCreateFence(device, &fenceInfo, nullptr, &inFlightFence) != VK_SUCCESS) {
            throw std::runtime_error("failed to create semaphores!");
        }
    }
    // ----------------------------------------------------------------------------------------------------------------------------------


    // Write commands (called record for some reason) to the COmmand Buffer   
    // We'll now start working on the recordCommandBuffer function that writes the commands we want to execute into a command buffer. The VkCommandBuffer used will be passed in as a parameter, as well as the index of the current swapchain image we want to write to.
    // ----------------------------------------------------------------------------------------------------------------------------------
    void recordCommandBuffer(VkCommandBuffer commandBuffer, uint32_t imageIndex) {
        // Command Buffer config info  
            VkCommandBufferBeginInfo beginInfo{};
            beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            beginInfo.flags = 0; // Optional
            beginInfo.pInheritanceInfo = nullptr; // Optional
        // Create the record buffer 
            if (vkBeginCommandBuffer(commandBuffer, &beginInfo) != VK_SUCCESS) { throw std::runtime_error("failed to begin recording command buffer!"); }
        // Drawing starts by beginning the render pass with vkCmdBeginRenderPass. The render pass is configured using some parameters in a VkRenderPassBeginInfo struct.
            VkRenderPassBeginInfo renderPassInfo{};
            renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            renderPassInfo.renderPass = renderPass;
            renderPassInfo.framebuffer = swapChainFramebuffers[imageIndex];
        // Some more paremters: 
            renderPassInfo.renderArea.offset = {0, 0};
            renderPassInfo.renderArea.extent = swapChainExtent;
            VkClearValue clearColor = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
            renderPassInfo.clearValueCount = 1;
            renderPassInfo.pClearValues = &clearColor;
        // Begin the Render Pass: 
        // -------------------------------------------------------
            vkCmdBeginRenderPass(commandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
            // Basic Drawing Commands: 
                // Bind the Pipeline 
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, graphicsPipeline);
                // Since we made viewport and scissor dynamic, need to specify information here: 
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
                // Draw CMD for the triangle 
                vkCmdDraw(commandBuffer, 3, 1, 0, 0);
                    // First Paremter = Command Buffer 
                    // vertexCount: Even though we don't have a vertex buffer, we technically still have 3 vertices to draw.
                    // instanceCount: Used for instanced rendering, use 1 if you're not doing that.
                    // firstVertex: Used as an offset into the vertex buffer, defines the lowest value of gl_VertexIndex.
                    // firstInstance: Used as an offset for instanced rendering, defines the lowest value of gl_InstanceIndex
            // End of Render Pass: 
            vkCmdEndRenderPass(commandBuffer);
        // -------------------------------------------------------
        // Done recording the command buffer 
            if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) { throw std::runtime_error("failed to record command buffer!"); }
            std::cout << "Recorded Command Buffer!" << std::endl;
    }
    // ----------------------------------------------------------------------------------------------------------------------------------


    // Command Buffers
    // ----------------------------------------------------------------------------------------------------------------------------------
    void createCommandBuffer(){
        // Allocation Information 
            VkCommandBufferAllocateInfo allocInfo{};
            allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            allocInfo.commandPool = commandPool;
            allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            allocInfo.commandBufferCount = 1;
        // Create Command Buffers based on allocation information 
            if (vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer) != VK_SUCCESS) { throw std::runtime_error("failed to allocate command buffers!"); }
            std::cout << "Created Command Buffer!" << std::endl;
    }
    // ----------------------------------------------------------------------------------------------------------------------------------



    // Command pools manage the memory that is used to store the COmmand buffers and command buffers are allocated from them. 
    // ----------------------------------------------------------------------------------------------------------------------------------
    void createCommandPool(){
        // Configure it 
            QueueFamilyIndices queueFamilyIndices = findQueueFamilies(physicalDevice);
            VkCommandPoolCreateInfo poolInfo{};
            poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();
        // Create it: 
            if (vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool) != VK_SUCCESS) { throw std::runtime_error("failed to create command pool!"); }
            std::cout << "Created Command Pool!" << std::endl;
    }
    // ----------------------------------------------------------------------------------------------------------------------------------


    // Create the Frame Buffer 
    // ----------------------------------------------------------------------------------------------------------------------------------
    void createFramebuffers(){
        // Resize the frame buffers container to be able to hold the size of the swap chain vector (same size)
            swapChainFramebuffers.resize(swapChainImageViews.size());
        // For each image view (recall that an image view is what is in the swap chain, each image view maps to an image)
            for (size_t i = 0; i < swapChainImageViews.size(); i++) {
                VkImageView attachments[] = { swapChainImageViews[i] };
                VkFramebufferCreateInfo framebufferInfo{};
                framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
                framebufferInfo.renderPass = renderPass;
                framebufferInfo.attachmentCount = 1;
                framebufferInfo.pAttachments = attachments;
                framebufferInfo.width = swapChainExtent.width;
                framebufferInfo.height = swapChainExtent.height;
                framebufferInfo.layers = 1;
                if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &swapChainFramebuffers[i]) != VK_SUCCESS) { throw std::runtime_error("failed to create framebuffer!"); }
                std::cout << "Frame Buffer created #" << i << std::endl;
            }
        // Print Debug: 
        std::cout << "Done creating Frame Buffers" << std::endl;
    }
    // ----------------------------------------------------------------------------------------------------------------------------------


    // Define the Render Pass (tell vulkan about frame buffer attachments, color and depth buffers, and samples per each of them) 
    // ----------------------------------------------------------------------------------------------------------------------------------
    void createRenderPass(){
        // In our case we'll have just a single color buffer attachment represented by one of the images from the swap chain.
            VkAttachmentDescription colorAttachment{};
            colorAttachment.format = swapChainImageFormat;
            colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT; // 1 sample 
        // Define the Load Op and Store OP 
            colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; // In our case we're going to use the clear operation to clear the framebuffer to black before drawing a new frame. 
            colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE; // We're interested in seeing the rendered triangle on the screen, so we're going with the store operation here.
        // Not using the stencil bufer, so dont care about this 
            colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        // Some more non sense about iamges something or other 
            colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        // Apparently you can make multiple passes (no clue what a pass is) and this can get better perf, but we dont care 
            VkAttachmentReference colorAttachmentRef{};
            colorAttachmentRef.attachment = 0;
            colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        // Subpass (type of pass) 
            VkSubpassDescription subpass{};
            subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
            subpass.colorAttachmentCount = 1;
            subpass.pColorAttachments = &colorAttachmentRef;
        // Subpass Depency something or other b/c of the seamphore schrinization that we needed to do  
            VkSubpassDependency dependency{};
            dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
            dependency.dstSubpass = 0;
            dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.srcAccessMask = 0;
            dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        // Render Pass: 
            VkRenderPassCreateInfo renderPassInfo{};
            renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
            renderPassInfo.attachmentCount = 1;
            renderPassInfo.pAttachments = &colorAttachment;
            renderPassInfo.subpassCount = 1;
            renderPassInfo.pSubpasses = &subpass;
            // Depcny stuff 
            renderPassInfo.dependencyCount = 1;
            renderPassInfo.pDependencies = &dependency;
        // Create the render pass object 
            if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass) != VK_SUCCESS) { throw std::runtime_error("failed to create render pass!"); }
        // Print Debug 
            std::cout << "Created the Render Pass!" << std::endl;
    }
    // ----------------------------------------------------------------------------------------------------------------------------------


    // Create the Graphis Peipeline 
    // ----------------------------------------------------------------------------------------------------------------------------------
    void createGraphicsPipeline(){
        // Load in the SPIR-V Byte Code for the Shaders (Vertext, Fragment)
            auto vertShaderCode = readFile("build/vert.spv");
            auto fragShaderCode = readFile("build/frag.spv");
        // Wrap the Shaders with the Light Weight Shader Module wrapper (just a size and ptr into it wrapper)
            VkShaderModule vertShaderModule = createShaderModule(vertShaderCode);
            VkShaderModule fragShaderModule = createShaderModule(fragShaderCode);
        // To use the (VERTEX) shader, need to assign to a specific graphics peipleine stage 
            VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
            vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
            vertShaderStageInfo.module = vertShaderModule;
            vertShaderStageInfo.pName = "main";                 // entry point into the shader (ie, do the main function!)
                // See that we could have 1 shader, with mutliple funtion entry points
        // To use the (FRAGMENT) shader, need to assign to a specific graphics peipleine stage 
            VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
            fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            fragShaderStageInfo.module = fragShaderModule;
            fragShaderStageInfo.pName = "main";
        // Array that contains the info structs for the VERTEX and FRAGMENT shader SPIR-V byte-code 
            VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};
        // Define the format of the vertex data that will be passed to the vertex shader (that i wrote) 
            VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
            vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
            vertexInputInfo.vertexBindingDescriptionCount = 0;
            vertexInputInfo.pVertexBindingDescriptions = nullptr; // Optional
            vertexInputInfo.vertexAttributeDescriptionCount = 0;
            vertexInputInfo.pVertexAttributeDescriptions = nullptr; // Optional
        // Vertex Input Assembly. Defines what gemeotry will be drawn (point, line, triange)
            VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
            inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
            inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            inputAssembly.primitiveRestartEnable = VK_FALSE;
        // Some nonsense about viewports and scissors 
            VkPipelineViewportStateCreateInfo viewportState{};
            viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
            viewportState.viewportCount = 1;
            viewportState.scissorCount = 1;
        // Define the Rasterizer 
            VkPipelineRasterizationStateCreateInfo rasterizer{};
            rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
            rasterizer.depthClampEnable = VK_FALSE;
            rasterizer.rasterizerDiscardEnable = VK_FALSE;
            rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
                // VK_POLYGON_MODE_FILL: fill the area of the polygon with fragments
                // VK_POLYGON_MODE_LINE: polygon edges are drawn as lines
                // VK_POLYGON_MODE_POINT: polygon vertices are drawn as points
            rasterizer.lineWidth = 1.0f;
            rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
            rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
            rasterizer.depthBiasEnable = VK_FALSE;
            rasterizer.depthBiasConstantFactor = 0.0f; // Optional
            rasterizer.depthBiasClamp = 0.0f; // Optional
            rasterizer.depthBiasSlopeFactor = 0.0f; // Optional
        // Multisimapling (some anitaliasing feature)
            VkPipelineMultisampleStateCreateInfo multisampling{};
            multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
            multisampling.sampleShadingEnable = VK_FALSE;
            multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            multisampling.minSampleShading = 1.0f; // Optional
            multisampling.pSampleMask = nullptr; // Optional
            multisampling.alphaToCoverageEnable = VK_FALSE; // Optional
            multisampling.alphaToOneEnable = VK_FALSE; // Optional
        // Color Blending (Attachment) information 
            VkPipelineColorBlendAttachmentState colorBlendAttachment{};
            colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            colorBlendAttachment.blendEnable = VK_FALSE;
            colorBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE; // Optional
            colorBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO; // Optional
            colorBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD; // Optional
            colorBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; // Optional
            colorBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO; // Optional
            colorBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD; // Optional
        // Color Blending information 
            VkPipelineColorBlendStateCreateInfo colorBlending{};
            colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
            colorBlending.logicOpEnable = VK_FALSE;
            colorBlending.logicOp = VK_LOGIC_OP_COPY; // Optional
            colorBlending.attachmentCount = 1;
            colorBlending.pAttachments = &colorBlendAttachment;
            colorBlending.blendConstants[0] = 0.0f; // Optional
            colorBlending.blendConstants[1] = 0.0f; // Optional
            colorBlending.blendConstants[2] = 0.0f; // Optional
            colorBlending.blendConstants[3] = 0.0f; // Optional
        // Dynamically specify the viewport and scissor window 
            std::vector<VkDynamicState> dynamicStates = {
                VK_DYNAMIC_STATE_VIEWPORT,
                VK_DYNAMIC_STATE_SCISSOR
            };
            VkPipelineDynamicStateCreateInfo dynamicState{};
            dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
            dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
            dynamicState.pDynamicStates = dynamicStates.data();
        // Create the pieplein layout object 
            VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
            pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            pipelineLayoutInfo.setLayoutCount = 0; // Optional
            pipelineLayoutInfo.pSetLayouts = nullptr; // Optional
            pipelineLayoutInfo.pushConstantRangeCount = 0; // Optional
            pipelineLayoutInfo.pPushConstantRanges = nullptr; // Optional
            if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipelineLayout) != VK_SUCCESS) { throw std::runtime_error("failed to create pipeline layout!"); }
        
        // Create the ACTUAL Graphics Peipeline OBJECT 
            VkGraphicsPipelineCreateInfo pipelineInfo{};
            pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
            pipelineInfo.stageCount = 2;
            // Shader (Programable) Stages: 
            pipelineInfo.pStages = shaderStages;
            // Fixed Function Stages: 
            pipelineInfo.pVertexInputState = &vertexInputInfo;
            pipelineInfo.pInputAssemblyState = &inputAssembly;
            pipelineInfo.pViewportState = &viewportState;
            pipelineInfo.pRasterizationState = &rasterizer;
            pipelineInfo.pMultisampleState = &multisampling;
            pipelineInfo.pDepthStencilState = nullptr; // Optional
            pipelineInfo.pColorBlendState = &colorBlending;
            pipelineInfo.pDynamicState = &dynamicState;
            // Handler 
            pipelineInfo.layout = pipelineLayout;
            // Refernce the Render Pass 
            pipelineInfo.renderPass = renderPass;
            pipelineInfo.subpass = 0;
            // Basically if you wanted you could create the peipline object based on another piepline ojbect, but we are not doing that here  
            pipelineInfo.basePipelineHandle = VK_NULL_HANDLE; // Optional
            pipelineInfo.basePipelineIndex = -1; // Optional
            // Create the guy 
            if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &graphicsPipeline) != VK_SUCCESS) { throw std::runtime_error("failed to create graphics pipeline!"); }
        // Able to destorey the Shader Wrapper now (some non sense about when the complication happesn idk)
            vkDestroyShaderModule(device, fragShaderModule, nullptr);
            vkDestroyShaderModule(device, vertShaderModule, nullptr); 
        // Print debug 
            std::cout << "Created the Graphics Pipeline!" << std::endl;
    }
    // ----------------------------------------------------------------------------------------------------------------------------------


    // Helper function to wrap a shader in a shader module (light weight wrapper)
    // ----------------------------------------------------------------------------------------------------------------------------------
    VkShaderModule createShaderModule(const std::vector<char>& code) {
        // Specify a pointer to the buffer with the bytecode and the length of it  
            VkShaderModuleCreateInfo createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            createInfo.codeSize = code.size();
            createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());
        // Create the Shader Module (wrapper)
            VkShaderModule shaderModule;
            if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) { throw std::runtime_error("failed to create shader module!"); }
            return shaderModule;
    }
    // ----------------------------------------------------------------------------------------------------------------------------------



    // Load in the SPIR-V Byte code for Shaders (fragment, vertex)
    // ----------------------------------------------------------------------------------------------------------------------------------
    static std::vector<char> readFile(const std::string& filename) {
        // load the binary data from the files. 
            std::ifstream file(filename, std::ios::ate | std::ios::binary);
            if (!file.is_open()) { throw std::runtime_error("failed to open file!"); }
        // use the read position to determine the size of the file and allocate a buffer:
            size_t fileSize = (size_t) file.tellg();
            std::vector<char> buffer(fileSize);
        // After that, we can seek back to the beginning of the file and read all of the bytes at once:
            file.seekg(0);
            file.read(buffer.data(), fileSize);
        // And finally close the file and return the bytes:
            file.close();
            return buffer;
    }
    // ----------------------------------------------------------------------------------------------------------------------------------


    // Create Image View. (now that we have swap chain with iamges, need a way to "view" these images and interface with them)
    // ----------------------------------------------------------------------------------------------------------------------------------
    void createImageViews() {
        swapChainImageViews.resize(swapChainImages.size()); // Size the Views based on the size of vector of images in the swap chain 
        // Iterate over all of the swap chain images: 
        for (size_t i = 0; i < swapChainImages.size(); i++) {
            // Misc
            VkImageViewCreateInfo createInfo{};                             // Create view info wrt this Image in the swap chain
            createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;    // Type = VIEW Info 
            createInfo.image = swapChainImages[i];                          // The image is one at index "i" in the swap chain vector
            createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;                    // Treat images as 1D, 2D, or 3D textures 
            createInfo.format = swapChainImageFormat;                       // idk
            // Components
            createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;    // default mapping 
            createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;    // default mapping 
            createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;    // default mapping 
            createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;    // default mapping 
            // SubresourceRange
            createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; // Our images are used as "color targets" 
            createInfo.subresourceRange.baseMipLevel = 0;
            createInfo.subresourceRange.levelCount = 1;
            createInfo.subresourceRange.baseArrayLayer = 0;
            createInfo.subresourceRange.layerCount = 1;
            // Create the Image View (for this image in the swap chain) 
            if (vkCreateImageView(device, &createInfo, nullptr, &swapChainImageViews[i]) != VK_SUCCESS) {
                throw std::runtime_error("failed to create image views!");
            }
            std::cout << "Created Image view for Image #" << i << " in Swap Chain" << std::endl;
        }
        std::cout << "Done creating image views!" << std::endl;
    }
    // ----------------------------------------------------------------------------------------------------------------------------------


    // Swap Chain: Create (based on Surface Format, Present Mode, and Swap Extent) 
    // ----------------------------------------------------------------------------------------------------------------------------------
    void createSwapChain() {
        // Use the information from the 3 helper functions (based on Surface Format, Present Mode, and Swap Extent)
            SwapChainSupportDetails swapChainSupport = querySwapChainSupport(physicalDevice);
            VkSurfaceFormatKHR surfaceFormat = chooseSwapSurfaceFormat(swapChainSupport.formats);
            VkPresentModeKHR presentMode = chooseSwapPresentMode(swapChainSupport.presentModes);
            VkExtent2D extent = chooseSwapExtent(swapChainSupport.capabilities);
        // Sepcify the number of images we want in the swap chain 
        std::cout << "Create Swap Chain: Specifying images for swap chain ..." << std::endl;
            uint32_t imageCount = swapChainSupport.capabilities.minImageCount + 1;
            if (swapChainSupport.capabilities.maxImageCount > 0 && imageCount > swapChainSupport.capabilities.maxImageCount) {
                imageCount = swapChainSupport.capabilities.maxImageCount;
            }
        // Create Swap Chain Object 
        std::cout << "Create Swap Chain: Creating Swap Chain object ..." << std::endl;
            VkSwapchainCreateInfoKHR createInfo{};
            createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
            createInfo.surface = surface;                                       // Suface that we set up (windows window surface)
            createInfo.minImageCount = imageCount;                              // Image Count
            createInfo.imageFormat = surfaceFormat.format;                      // Surface Format - Format
            createInfo.imageColorSpace = surfaceFormat.colorSpace;              // Surface Format - Color Space
            createInfo.imageExtent = extent;                                    // Swap Extent 
            createInfo.imageArrayLayers = 1;                                    // 1 for normal applications
            createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;        // 
        // Abritating the Swap Chain between multiple Q families (presnetaiton Q, and grpahis Q)
        std::cout << "Create Swap Chain: Arbitating the the swap chains between Qs ..." << std::endl;
            QueueFamilyIndices indices = findQueueFamilies(physicalDevice);
            uint32_t queueFamilyIndices[] = {indices.graphicsFamily.value(), indices.presentFamily.value()};
            if (indices.graphicsFamily != indices.presentFamily) {
                createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
                createInfo.queueFamilyIndexCount = 2;
                createInfo.pQueueFamilyIndices = queueFamilyIndices;
            } else {
                createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
                createInfo.queueFamilyIndexCount = 0; // Optional
                createInfo.pQueueFamilyIndices = nullptr; // Optional
            }
        // More settings: 
        std::cout << "Create Swap Chain: More Settings ..." << std::endl;
            createInfo.preTransform = swapChainSupport.capabilities.currentTransform;   // Dont do any rotations 
            createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;              // Blending (transparent winodws)
            createInfo.presentMode = presentMode;
            createInfo.clipped = VK_TRUE;                                               // Ignore pixles that are behind another window 
            createInfo.oldSwapchain = VK_NULL_HANDLE;                                   // When window is resized, everything gets fucked, so have to create new swap chain, and will still wnat access to old swap chain
        // Create the Swap Chain Object 
        std::cout << "Create Swap Chain: Create object ..." << std::endl;
            // VkSwapchainKHR swapChain;
            if (vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapChain) != VK_SUCCESS) {
                throw std::runtime_error("failed to create swap chain!");
            }
        // Fill out the handler to the images in the swap chain 
        std::cout << "Create Swap Chain: Fill out handler to get images ..." << std::endl;
            vkGetSwapchainImagesKHR(device, swapChain, &imageCount, nullptr);
            swapChainImages.resize(imageCount); // Note that b/c of how we specified the number of images in the chain, the allocator could have give us more
            vkGetSwapchainImagesKHR(device, swapChain, &imageCount, swapChainImages.data());    // Therefoere need to resize to that size ^ 
        // Specifiy the extent and format in out class memebers so have access to it later 
            std::cout << "Create Swap Chain: Fill in extent and format vars ..." << std::endl;
            swapChainImageFormat = surfaceFormat.format;
            swapChainExtent = extent;
        // Debug msg: 
            std::cout << "Created Swap Chain!" << std::endl;
    }
    // ----------------------------------------------------------------------------------------------------------------------------------



    // Swap Chain: Swap Extent (some nonsense about choosing the right resolution)
    // ----------------------------------------------------------------------------------------------------------------------------------
    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities) {
        if (capabilities.currentExtent.width != std::numeric_limits<uint32_t>::max()) {
                std::cout << "Swap Chain: Chose Swap Extent" << std::endl;
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
            std::cout << "Swap Chain: Chose Swap Extent" << std::endl;
            return actualExtent;
        }
    }
    // ----------------------------------------------------------------------------------------------------------------------------------


    // Swap Chain: Present Mode 
    // ----------------------------------------------------------------------------------------------------------------------------------
    VkPresentModeKHR chooseSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes) {
        // Theres different modes to which we can present images from the swap chain to the screen: 
            // VK_PRESENT_MODE_IMMEDIATE_KHR: Images submitted by your application are transferred to the screen right away, which may result in tearing.
            // VK_PRESENT_MODE_FIFO_KHR: The swap chain is a queue where the display takes an image from the front of the queue when the display is refreshed and the program inserts rendered images at the back of the queue. If the queue is full then the program has to wait. This is most similar to vertical sync as found in modern games. The moment that the display is refreshed is known as "vertical blank".
            // VK_PRESENT_MODE_FIFO_RELAXED_KHR: This mode only differs from the previous one if the application is late and the queue was empty at the last vertical blank. Instead of waiting for the next vertical blank, the image is transferred right away when it finally arrives. This may result in visible tearing.
            // VK_PRESENT_MODE_MAILBOX_KHR: This is another variation of the second mode. Instead of blocking the application when the queue is full, the images that are already queued are simply replaced with the newer ones. This mode can be used to render frames as fast as possible while still avoiding tearing, resulting in fewer latency issues than standard vertical sync. This is commonly known as "triple buffering", although the existence of three buffers alone does not necessarily mean that the framerate is unlocked.
        // We will go with the last one b/c writer of tutorial likes it best (for perf)
            for (const auto& availablePresentMode : availablePresentModes) {
                if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR) { 
                    std::cout << "Swap Chain: Chose Presnetatoin Mode" << std::endl;
                    return availablePresentMode; 
                }
            }
        // If dont find the MAILBOX one we want, setlle for the 2nd option
            std::cout << "Swap Chain: Chose Presnetatoin Mode" << std::endl;
            return VK_PRESENT_MODE_FIFO_KHR;        
    }
    // ----------------------------------------------------------------------------------------------------------------------------------


    // Swap Chain: Surface Format (no clue what this means)
    // ----------------------------------------------------------------------------------------------------------------------------------
    VkSurfaceFormatKHR chooseSwapSurfaceFormat(const std::vector<VkSurfaceFormatKHR>& availableFormats) {
        // Return Type Struct has: Format, and Color Space Member 
            // Format = Colors Channels and Types, ex: For example, VK_FORMAT_B8G8R8A8_SRGB means that we store the B, G, R and alpha channels in that order with an 8 bit unsigned integer for a total of 32 bits per pixel. 
            // Color Space: The colorSpace member indicates if the SRGB color space is supported or not using the VK_COLOR_SPACE_SRGB_NONLINEAR_KHR flag. For the color space we'll use SRGB if it is available, because it results in more accurate perceived colors. It is also pretty much the standard color space for images, like the textures we'll use later on. 
        // See if the desired combo is avialbe (return index into vector)
            for (const auto& availableFormat : availableFormats) {
                if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB && availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { 
                    std::cout << "Swap Chain: Chose Surface Format" << std::endl;
                    return availableFormat; 
                }
            } 
        // If not return first element in vector, the first format, (not sure why, we just settle for this according to tutorial)
            std::cout << "Swap Chain: Chose Surface Format" << std::endl;
            return availableFormats[0];
    }
    // ----------------------------------------------------------------------------------------------------------------------------------



    // Fill out the struct that contains the swap chain information for this PHysical Device (and surface combination)
        // Called by "bool isDeviceSuitable()""
    // ----------------------------------------------------------------------------------------------------------------------------------
    SwapChainSupportDetails querySwapChainSupport(VkPhysicalDevice device) {
        // Get the details of the (device && surface) wrt swap chain information 
            SwapChainSupportDetails details;
            vkGetPhysicalDeviceSurfaceCapabilitiesKHR(device, surface, &details.capabilities);
        // Formats: 
            // Count the number of supported formats
            uint32_t formatCount;
            vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, nullptr);
            // If the format count is non-zero, then make the "details" the correct size to take all of the data
            if (formatCount != 0) { details.formats.resize(formatCount); vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, details.formats.data()); }
        // Presnetation Modes: 
            // Count the number of supported presetation modes
            uint32_t presentModeCount;
            vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, nullptr);
            // Mkae the variable the correct size to hold the data, if count is non zero
            if (presentModeCount != 0) { details.presentModes.resize(presentModeCount); vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface, &presentModeCount, details.presentModes.data()); }
        // Return Details
            std::cout << "Swap Chain: Querying Swap Chain Support" << std::endl;
            return details;
    }
    // ----------------------------------------------------------------------------------------------------------------------------------


    // Need to create the "Surface" (links the vulkan --> rendering window b/c platform agnostic)
    // ----------------------------------------------------------------------------------------------------------------------------------
    void createSurface() {
        if (glfwCreateWindowSurface(instance, window, nullptr, &surface) != VK_SUCCESS) { throw std::runtime_error("failed to create window surface!"); }
    }
    // ----------------------------------------------------------------------------------------------------------------------------------


    // Creat the Logical Device so that you can interface the Physical Device (Now that we have a Physical Device, and know what Q families we have)
    // ----------------------------------------------------------------------------------------------------------------------------------
        void createLogicalDevice() {
            // Specifying the queues to be created
            // Specify Qs priority (determines which Q scheduler sends to GPU first)
                QueueFamilyIndices indices = findQueueFamilies(physicalDevice);
                std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
                std::set<uint32_t> uniqueQueueFamilies = {
                    indices.graphicsFamily.value(),
                    indices.presentFamily.value()
                };
                float queuePriority = 1.0f;
                for (uint32_t queueFamily : uniqueQueueFamilies) {
                    VkDeviceQueueCreateInfo queueCreateInfo{};
                    queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
                    queueCreateInfo.queueFamilyIndex = queueFamily;
                    queueCreateInfo.queueCount = 1;
                    queueCreateInfo.pQueuePriorities = &queuePriority;
                    queueCreateInfos.push_back(queueCreateInfo);
                }
            // Define the features this logical device has (based on physical device)
                VkPhysicalDeviceFeatures deviceFeatures{};  // No definition ==> all memeber == FALSE (ie DNE)
            // Create the Logical Device 
                VkDeviceCreateInfo createInfo{};
                createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
            // Add pointers to the queue creation info and device features structs 
                createInfo.queueCreateInfoCount = static_cast<uint32_t>(queueCreateInfos.size());
                createInfo.pQueueCreateInfos = queueCreateInfos.data();
                createInfo.pEnabledFeatures = &deviceFeatures;
            // Some non sense to be compatible with older versoins of vulkan 
                // This is for swap chain creation 
                    createInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
                    createInfo.ppEnabledExtensionNames = deviceExtensions.data();
                if (enableValidationLayers) {
                    createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
                    createInfo.ppEnabledLayerNames = validationLayers.data();
                } else { createInfo.enabledLayerCount = 0; }
            // Instantiate the Logical Device (vkCREATEdevice)
                if (vkCreateDevice(physicalDevice, &createInfo, nullptr, &device) != VK_SUCCESS) { throw std::runtime_error("failed to create logical device!"); }
                    // Specifying to the instance information about: // Qs // PHysical Device // Suprted features
            // Retrieving queue handles 
                vkGetDeviceQueue(device, indices.graphicsFamily.value(), 0, &graphicsQueue);
                vkGetDeviceQueue(device, indices.presentFamily.value(), 0, &presentQueue);
            // Done 
            std::cout << "Logical Device Created!" << std::endl;
        }
    // ----------------------------------------------------------------------------------------------------------------------------------


    // Based on the GPU Physical Device, see what Q families (types) it has 
    // ----------------------------------------------------------------------------------------------------------------------------------
    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device) {
        QueueFamilyIndices indices;
        // Count the  number of Q Families there are for this GPU (physical device)
            uint32_t queueFamilyCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, nullptr);
        // Based on that number, make a vector of that size, to hold all the different Qs
            std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
            vkGetPhysicalDeviceQueueFamilyProperties(device, &queueFamilyCount, queueFamilies.data());
        // Look for a Q that supports "VK_QUEUE_GRAPHICS_BIT" ()
            int i = 0;
            for (const auto& queueFamily : queueFamilies) {
                if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) { indices.graphicsFamily = i; }
                
                // Something something about the Q needing the right presneting mode for the surface 
                VkBool32 presentSupport = false;
                vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &presentSupport);
                if (presentSupport) { indices.presentFamily = i; }

                if (indices.isComplete()) { break; }

                i++;
            }
        return indices;
    }
    // ----------------------------------------------------------------------------------------------------------------------------------


    // Pick a GPU to be used: 
    // ----------------------------------------------------------------------------------------------------------------------------------
    // Actually picks the GPU
    void pickPhysicalDevice() {
        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
        if (deviceCount == 0) { throw std::runtime_error("failed to find GPUs with Vulkan support!"); }
        else{ std::cout << "There exsits GPUs with Vulkan Support on this device!" << std::endl; }
        // allocate an array to hold all of the VkPhysicalDevice handles.
        std::vector<VkPhysicalDevice> devices(deviceCount);                     // create vector of size "device Count"
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());     // Enumerate, putting data for each GPU into above Vector 
        // Iterates through the devices, and runs them through the "is sutable" function
        for (const auto& device : devices) {
            if (isDeviceSuitable(device)) { physicalDevice = device; break; }
        }
        if (physicalDevice == VK_NULL_HANDLE) { throw std::runtime_error("failed to find a suitable GPU!"); }
        else{ std::cout << "Found a stuable GPU to go with :)" << std::endl; }
    }
    // Looks at the GPU devices, and returns true if its sutable for what we want 
    bool isDeviceSuitable(VkPhysicalDevice device) {
        // Look at the Q types this GPU supports, and return true, if this GPU has "graphics Family" which I defined as "having a value" if the device as the "VK_QUEUE_GPU_BIT" or some such
            QueueFamilyIndices indices = findQueueFamilies(device);
            // return indices.graphicsFamily.has_value();
        // Checking for the desired swap chain support on this physical device: 
            bool extensionsSupported = checkDeviceExtensionSupport(device);
            // Look at the swap chain struct (based on the physical device) to see 
            bool swapChainAdequate = false;
            if (extensionsSupported) {
                SwapChainSupportDetails swapChainSupport = querySwapChainSupport(device);
                swapChainAdequate = !swapChainSupport.formats.empty() && !swapChainSupport.presentModes.empty();
            }
        // Return T/F
            return indices.isComplete() && extensionsSupported && swapChainAdequate;
        // In the future could do something more advanced: 
            // ---------------------------------------------------------------
            // VkPhysicalDeviceProperties deviceProperties;
            // VkPhysicalDeviceFeatures deviceFeatures;
            // vkGetPhysicalDeviceProperties(device, &deviceProperties);   // Gets GPU Properties
            // vkGetPhysicalDeviceFeatures(device, &deviceFeatures);       // Gets GPU Features
            // return deviceProperties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && deviceFeatures.geometryShader;
            //     // Want physical GPU with Geomtry shader support
            // ---------------------------------------------------------------
    }
    bool checkDeviceExtensionSupport(VkPhysicalDevice device) {
        // Count the number of esntensions 
        uint32_t extensionCount;
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, nullptr);
        // Create a vector of that size to hold all of the extensions
        std::vector<VkExtensionProperties> availableExtensions(extensionCount);
        vkEnumerateDeviceExtensionProperties(device, nullptr, &extensionCount, availableExtensions.data());
        // Get the required Extensions from the list defined globally at the top of this file "deviceExtensions" 
        std::set<std::string> requiredExtensions(deviceExtensions.begin(), deviceExtensions.end());
        // Iterate through the extensions this physical device defines, and see if they have the extension we want
        for (const auto& extension : availableExtensions) { requiredExtensions.erase(extension.extensionName); }
        return requiredExtensions.empty();
    }
    // ----------------------------------------------------------------------------------------------------------------------------------



    bool checkValidationLayerSupport() {
        // Get a <vector> of all of the validation layers that are supported 
        // -------------------------------------------------------------------------------
        uint32_t layerCount;
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
        std::vector<VkLayerProperties> availableLayers(layerCount);
        vkEnumerateInstanceLayerProperties(&layerCount, availableLayers.data());

        // Make sure all of the validation layers we want are included in the avialbe layers
        // -------------------------------------------------------------------------------
        for (const char* layerName : validationLayers) {
            bool layerFound = false;
            for (const auto& layerProperties : availableLayers) {
                if (strcmp(layerName, layerProperties.layerName) == 0) { layerFound = true; break; }
            }
            if (!layerFound) { return false; }
        }

        // If made it this far, we good: 
        return true;
    }


    void initVulkan() {
        createInstance();           // Create Vulkan Instance
        // setupDebugMessenger();   // I skipped this part of tutorial
        createSurface();            // Something that links Vulkan agnostic API to Windows Rendering Screen
        pickPhysicalDevice();       // Pick a Physical Device, and determine its feature set / Qs
        createLogicalDevice();      // Create Logical Device to interface Physical Device
        createSwapChain();          // Create Swap Chain (images to be written to the screen)
        createImageViews();         // Create the Views into the images in the above Swap Chain
        createRenderPass();         // Define the Render Pass
        createGraphicsPipeline();   // Create the Graphics Pipeline 
        createFramebuffers();       // Create the frame buffers 
        createCommandPool();        // Create the Command Pool (this manges the Command Buffers Memeory and such)
        createCommandBuffer();      // Create the COmmand Buffers
        createSyncObjects();        // Sychrnization between CPU and GPU (and between GPU Qs)
    }


    void createInstance() {
        // Make sure all requested validation layers (debug shit) are accounted for 
        // -------------------------------------------------------------------------------
        VkInstanceCreateInfo createInfo{};
        if (enableValidationLayers){
            createInfo.enabledLayerCount = static_cast<uint32_t>(validationLayers.size());
            createInfo.ppEnabledLayerNames = validationLayers.data();
            if(!checkValidationLayerSupport()) { throw std::runtime_error("validation layers requested, but not available!"); }
            else{std::cout << "Validation turned on!" << std::endl;}
        }
        else{createInfo.enabledLayerCount = 0;}

        // Optonally give extra info to the graphics driver (struct)
        // -------------------------------------------------------------------------------
        // This data is technically optional, but it may provide some useful 
        // information to the driver in order to optimize our specific application 
        // (e.g. because it uses a well-known graphics engine with certain special 
        // behavior). This struct is called VkApplicationInfo:
        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "Hello Triangle";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "No Engine";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_0;

        // Vulkan Instance information (struct)
        // -------------------------------------------------------------------------------
        // This next struct is not optional and tells the Vulkan driver which global 
        // extensions and validation layers we want to use. Global here means that 
        // they apply to the entire program and not a specific device, which will 
        // become clear in the next few chapters.
        createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        createInfo.pApplicationInfo = &appInfo;
        // The next two layers specify the desired global extensions. As mentioned in 
        // the overview chapter, Vulkan is a platform agnostic API, which means that 
        // you need an extension to interface with the window system. GLFW has a handy 
        // built-in function that returns the extension(s) it needs to do that which we 
        // can pass to the struct: 
        uint32_t glfwExtensionCount = 0;
        const char** glfwExtensions;
        glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);
        // The last two members of the struct determine the global validation layers to 
        // enable. We'll talk about these more in-depth in the next chapter, so just 
        // leave these empty for now.
        createInfo.enabledExtensionCount = glfwExtensionCount;
        createInfo.ppEnabledExtensionNames = glfwExtensions;
        createInfo.enabledLayerCount = 0;

        // See what extensions we have: 
        // -------------------------------------------------------------------------------
        uint32_t extensionCount = 0;
        vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr);
        std::vector<VkExtensionProperties> extensions(extensionCount);
        vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions.data());
        std::cout << "available extensions:\n";
        for (const auto& extension : extensions) { std::cout << '\t' << extension.extensionName << '\n'; }

        // Instantiate the Vulkan Object
        // -------------------------------------------------------------------------------
        // As you'll see, the general pattern that object creation function parameters in Vulkan follow is:
            // Pointer to struct with creation info
            // Pointer to custom allocator callbacks, always nullptr in this tutorial
            // Pointer to the variable that stores the handle to the new object
        VkResult result = vkCreateInstance(&createInfo, nullptr, &instance);
        if (result != VK_SUCCESS) { throw std::runtime_error("failed to create instance!"); }
    }


    // "Forever" Loop: 
    // -----------------------------------------------------------------------------------
    void mainLoop() {
        while (!glfwWindowShouldClose(window)) {    // while you should not close the window: 
            glfwPollEvents();                       // polls for events, like clicking the X button
            drawFrame();                            // Drag the Frame! :fire: 
        }
    }
    // -----------------------------------------------------------------------------------


    void cleanup() {
        vkDestroySemaphore(device, imageAvailableSemaphore, nullptr);   // Semaphore 1
        vkDestroySemaphore(device, renderFinishedSemaphore, nullptr);   // Semaphore 2
        vkDestroyFence(device, inFlightFence, nullptr);                 // Fence 1
        vkDestroyCommandPool(device, commandPool, nullptr);         // Destory the Command Pool
        for (auto framebuffer : swapChainFramebuffers) { vkDestroyFramebuffer(device, framebuffer, nullptr); }  // destory each frame buffer (vector of buffers that map 1:1 with image views who map 1:1 with iamges)
        vkDestroyPipeline(device, graphicsPipeline, nullptr);       // Destroy the piepline 
        vkDestroyPipelineLayout(device, pipelineLayout, nullptr);   // Destory the peipleine layout 
        vkDestroyRenderPass(device, renderPass, nullptr);           // Desotry the render pass
        for (auto imageView : swapChainImageViews) { vkDestroyImageView(device, imageView, nullptr); }  // For each image view (wrt to the swap chain) we need to destory it (becase we manually created it)
        vkDestroySwapchainKHR(device, swapChain, nullptr);  // Destroy the swap chain
        vkDestroyDevice(device, nullptr);       // Destorys the logical device 
        vkDestroySurfaceKHR(instance, surface, nullptr);    // Destore the surface 
        vkDestroyInstance(instance, nullptr);   // Desotry the vulkan instance
        glfwDestroyWindow(window);              // destory the window
        glfwTerminate();                        // turn off glfw 
    }
};

int main() {
    HelloTriangleApplication app;

    try {
        app.run();
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}