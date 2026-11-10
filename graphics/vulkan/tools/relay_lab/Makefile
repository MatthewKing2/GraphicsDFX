VULKAN_SDK ?= /c/VulkanSDK/1.4.341.1
OUT = ../../build/relay_lab
CXX = clang++
.PHONY: all test
all: $(OUT)/compute.exe $(OUT)/relay.spv
$(OUT):
	python -c "from pathlib import Path; Path('$(OUT)').mkdir(parents=True, exist_ok=True)"
$(OUT)/compute.exe: compute.cpp ../../src/prediction.cpp ../../src/prediction.hpp ../../shaders/reactor_kernel.inc ../../shaders/prediction_kernel.inc ../../shaders/tunnel_math.inc ../../shaders/tunnel_config.inc | $(OUT)
	$(CXX) -std=c++17 -O3 -DNDEBUG -Wall -Wextra -Wno-missing-field-initializers compute.cpp ../../src/prediction.cpp -I"$(VULKAN_SDK)/Include" "$(VULKAN_SDK)/Lib/vulkan-1.lib" -o $@
$(OUT)/relay.spv: relay.comp ../../shaders/reactor_kernel.inc ../../shaders/tunnel_math.inc ../../shaders/tunnel_config.inc | $(OUT)
	"$(VULKAN_SDK)/Bin/glslc.exe" -O $< -o $@
test: all
	$(OUT)/compute.exe --validate
	python lab.py --self-test
