# GraphicsDFX

Senior-design graphics and FPGA acceleration experiments.

- `graphics/vulkan`: Matthew's GPU N-body simulation and Vulkan renderer.
- [RNG workload experiments](graphics/rng_vulkan/README.md): Monte Carlo pi and
  two-dimensional diffusion, visualizers, GPU profiling, and external RNG input.
- [Profiling comparison](graphics/rng_vulkan/PROFILING_COMPARISON.md): how the
  N-body and RNG measurement methods relate.
- [Jetson and FPGA integration](graphics/rng_vulkan/JETSON_FPGA.md): the staged
  PCIe input contract, buffer ownership, and end-to-end timing requirements.

Build and run the RNG experiment:

```sh
cmake -S graphics/rng_vulkan -B graphics/rng_vulkan/build -DCMAKE_BUILD_TYPE=Release
cmake --build graphics/rng_vulkan/build --parallel
ctest --test-dir graphics/rng_vulkan/build --output-on-failure
graphics/rng_vulkan/run.sh --workload pi --mode fused
```

The retained measurements are from Apple M5 development hardware. Jetson and
physical FPGA/PCIe performance still require testing on the target system.
