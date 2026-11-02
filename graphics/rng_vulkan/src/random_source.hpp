#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>

namespace rnglab {

// RNG1 wire protocol, version 1. All fields and payload words are little-endian.
// Header: magic, version, count, blocks, epoch, seed, workload, flags (8 uint32).
// Payload: uvec4 randoms[blocks * count], indexed block * count + entity.
// Values: magic=0x31474e52 (bytes "RNG1"), version=1, flags=0.
// Workload: 0=pi, 1=uniform-increment walk. Payload size=count*blocks*16.
// A stream must provide one frame per epoch, including application warmup.
constexpr std::uint32_t randomStreamMagic = 0x31474e52u;
constexpr std::uint32_t randomStreamVersion = 1u;
constexpr std::size_t randomStreamHeaderBytes = 32;

// Blocking byte-stream adapter for a file, FIFO, or readable driver device.
// On return from readFrame(), the complete payload is in the caller's mapped
// staging buffer; only then may the caller submit a Vulkan copy/consumer.
// Device-driver reads must themselves wait for DMA completion before returning.
//
// This does not configure a board or DMA engine. XDMA character devices can
// impose address, size, alignment and ioctl/control requirements; a board's
// driver/producer must adapt those requirements to this framed byte stream.
class RandomStream {
public:
    explicit RandomStream(const std::string& path);
    ~RandomStream();
    RandomStream(const RandomStream&) = delete;
    RandomStream& operator=(const RandomStream&) = delete;

    void readFrame(void* mapped, std::size_t payloadBytes,
                   std::uint32_t count, std::uint32_t blocks,
                   std::uint32_t epoch, std::uint32_t seed,
                   std::uint32_t workload);

private:
    void readExact(void* destination, std::size_t bytes, const char* description);
    std::string path_;
#ifdef _WIN32
    std::FILE* file_ = nullptr;
#else
    int fd_ = -1;
#endif
};

} // namespace rnglab
