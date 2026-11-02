#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct Options {
    std::uint32_t count=65536, blocks=16, frames=0, warmup=20, seed=42;
    std::uint32_t workload=0, mode=0, visualSamples=32768, deviceIndex=0;
    std::uint32_t isolate=0; // 0=normal, 1=RNG only, 2=consumer only
    float stepSize=0.01f;
    bool headless=false, visualize=true, validation=false, verify=false;
    std::string csv, image, rngStream;
};
struct Timing {
    double rng=0, simulation=0, compute=0, visualPrepare=0, render=0, gpu=0, cpu=0;
    double rngInput=0, rngStaging=0, upload=0;
};
using State = std::array<std::uint32_t,4>;

class Gpu {
public:
    explicit Gpu(const Options& options);
    ~Gpu();
    Gpu(const Gpu&)=delete;
    Gpu& operator=(const Gpu&)=delete;
    bool poll();
    Timing frame(std::uint32_t epoch);
    std::vector<State> readStates();
    std::vector<State> readRandoms();
    void saveImage(const std::string& path);
    const std::string& deviceName() const;
    std::uint32_t driverVersion() const;
    float timestampPeriod() const;
    std::uint32_t timestampBits() const;
    const char* timestampMethod() const;
    unsigned validationErrors() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
