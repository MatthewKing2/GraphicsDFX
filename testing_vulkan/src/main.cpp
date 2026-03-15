// #include <vulkan/vulkan.h>  //Include the Vulkan header which provides the functions, structures and enumerations.  
#define GLFW_INCLUDE_VULKAN     // Replace vulkan header with the GLFW one (this automaticlaly loads the vulkan one)
#include <GLFW/glfw3.h>

#include <iostream>
#include <stdexcept>
#include <cstdlib>
#include <vector>

class HelloTriangleApplication {
private:
    GLFWwindow* window;             // Stores pointer to the window we writing stuff to
    const uint32_t WIDTH = 800;     // Window Width
    const uint32_t HEIGHT = 600;    // Window Height
    VkInstance instance;

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

    void initVulkan() {
        createInstance();
    }

    void createInstance() {
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
        VkInstanceCreateInfo createInfo{};
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