VULKAN_SDK = /c/VulkanSDK/1.4.341.1
CXX = clang++
GLSLC = "$(VULKAN_SDK)/Bin/glslc.exe"
CXXFLAGS = -std=c++17 -Wall

SRC = src/main.cpp src/renderer.cpp src/camera.cpp
OUT = build/vulkan_app.exe
SHADERS = build/vert.spv build/frag.spv

INCLUDES = \
-I"../glm" \
-I"../Libraries/glfw-3.4.bin.WIN64/include" \
-I"../Libraries/stb" \
-I"../Libraries/tinyobjloader" \
-I"$(VULKAN_SDK)/Include"

LIBS = \
"../Libraries/glfw-3.4.bin.WIN64/lib-vc2022/glfw3.lib" \
"$(VULKAN_SDK)/Lib/vulkan-1.lib" \
-luser32 \
-lgdi32 \
-lshell32 \
-lole32

.PHONY: all shaders

all: shaders $(OUT)

shaders: $(SHADERS)

build/vert.spv: shaders/shader.vert
	$(GLSLC) $< -o $@

build/frag.spv: shaders/shader.frag
	$(GLSLC) $< -o $@

$(OUT): $(SRC) src/renderer.hpp src/camera.hpp
	$(CXX) $(SRC) $(CXXFLAGS) $(INCLUDES) $(LIBS) -o $(OUT)
