// #include <vulkan/vulkan.h>  //Include the Vulkan header which provides the functions, structures and enumerations.  
#define GLFW_INCLUDE_VULKAN     // Replace vulkan header with the GLFW one (this automaticlaly loads the vulkan one)
#include <GLFW/glfw3.h>
#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <vector>
#include <optional>


// Validation Layers (enblaed or disabled at compile time, based on NDEBUG flag)
// #######################################################################################
const std::vector<const char*> validationLayers = { "VK_LAYER_KHRONOS_validation" };
#ifdef NDEBUG
    const bool enableValidationLayers = false;
#else
    const bool enableValidationLayers = true;
#endif



class HelloTriangleApplication {
private:
    GLFWwindow* window;             // Stores pointer to the window we writing stuff to
    const uint32_t WIDTH = 800;     // Window Width
    const uint32_t HEIGHT = 600;    // Window Height
    VkInstance instance;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;   // This is the GPU Physical Device we are using (struct of info about it)
    struct QueueFamilyIndices {     // Struct that holds the different types of Qs
        std::optional<uint32_t> graphicsFamily;
        bool isComplete() { return graphicsFamily.has_value(); }
    };

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
            return indices.isComplete(); // same as above just more abstraction :) 
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
        createInstance();
        // setupDebugMessenger(); // I skipped this part of tutorial
        pickPhysicalDevice();
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


    void mainLoop() {
        // Forever Loop: 
        // -----------------------------------------------------------------------------------
        while (!glfwWindowShouldClose(window)) {    // while you should not close the window: 
            glfwPollEvents();                       // polls for events, like clicking the X button
        }
    }

    void cleanup() {
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