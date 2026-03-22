CXX = clang++
CXXFLAGS = -std=c++17 -Wall

SRC = "src/main.cpp"
OUT = "build/vulkan_app.exe"

INCLUDES = \
-I"../glm" \
-I"../Libraries/glfw-3.4.bin.WIN64/include" \
-I"/c/VulkanSDK/1.4.341.1/Include"

LIBS = \
"../Libraries/glfw-3.4.bin.WIN64/lib-vc2022/glfw3.lib" \
"/c/VulkanSDK/1.4.341.1/Lib/vulkan-1.lib" \
-luser32 \
-lgdi32 \
-lshell32 \
-lole32

all:
	$(CXX) $(SRC) $(CXXFLAGS) $(INCLUDES) $(LIBS) -o $(OUT)
