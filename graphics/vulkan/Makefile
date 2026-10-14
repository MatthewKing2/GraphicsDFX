VULKAN_SDK = /c/VulkanSDK/1.4.341.1
CXX = clang++
GLSLC = "$(VULKAN_SDK)/Bin/glslc.exe"
CXXFLAGS = -std=c++17 -Wall -O3

SRC = src/main.cpp src/renderer.cpp src/camera.cpp
OUT = build/vulkan_app.exe
SHADERS = build/vert.spv build/frag.spv build/nbody.spv

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

.PHONY: all shaders test

all: shaders $(OUT)

shaders: $(SHADERS)

build/vert.spv: shaders/shader.vert
	$(GLSLC) $< -o $@

build/frag.spv: shaders/shader.frag
	$(GLSLC) $< -o $@

build/nbody.spv: shaders/nbody.comp
	$(GLSLC) $< -o $@

$(OUT): $(SRC) src/renderer.hpp src/camera.hpp src/profiling.hpp
	$(CXX) $(SRC) $(CXXFLAGS) $(INCLUDES) $(LIBS) -o $(OUT)

build/nbody_test.exe: tests/nbody_test.cpp src/renderer.cpp src/renderer.hpp src/profiling.hpp
	$(CXX) tests/nbody_test.cpp src/renderer.cpp $(CXXFLAGS) $(INCLUDES) $(LIBS) -o $@

# Run from this directory so shader/texture paths resolve correctly.
test: shaders build/nbody_test.exe
	./build/nbody_test.exe
