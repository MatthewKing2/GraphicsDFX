#include "random_source.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <stdexcept>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

namespace rnglab {
namespace {
std::uint32_t littleWord(const unsigned char* bytes) {
    return std::uint32_t(bytes[0]) | (std::uint32_t(bytes[1]) << 8) |
           (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 24);
}
}

RandomStream::RandomStream(const std::string& path) : path_(path) {
    if(path.empty()) throw std::invalid_argument("RNG stream path must not be empty");
#ifdef _WIN32
    file_ = std::fopen(path.c_str(), "rb");
    if(!file_) throw std::runtime_error("Cannot open RNG stream '" + path + "': " + std::strerror(errno));
#else
    do {
        fd_ = ::open(path.c_str(), O_RDONLY);
    } while(fd_ < 0 && errno == EINTR);
    if(fd_ < 0) throw std::runtime_error("Cannot open RNG stream '" + path + "': " + std::strerror(errno));
#endif
}

RandomStream::~RandomStream() {
#ifdef _WIN32
    if(file_) std::fclose(file_);
#else
    if(fd_ >= 0) ::close(fd_);
#endif
}

void RandomStream::readExact(void* destination, std::size_t bytes, const char* description) {
    auto* output = static_cast<unsigned char*>(destination);
    std::size_t received = 0;
    while(received < bytes) {
#ifdef _WIN32
        const std::size_t amount = std::fread(output + received, 1, bytes - received, file_);
        if(amount == 0 && std::ferror(file_)) {
            if(errno == EINTR) { std::clearerr(file_); continue; }
            throw std::runtime_error("RNG stream read failed for '" + path_ + "': " + std::strerror(errno));
        }
#else
        const auto request = std::min(bytes - received,
            static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
        const ssize_t amount = ::read(fd_, output + received, request);
        if(amount < 0) {
            if(errno == EINTR) continue;
            throw std::runtime_error("RNG stream read failed for '" + path_ + "': " + std::strerror(errno));
        }
#endif
        if(amount == 0) {
            throw std::runtime_error("Truncated RNG stream '" + path_ + "' while reading " + description +
                ": received " + std::to_string(received) + " of " + std::to_string(bytes) + " bytes (EOF)");
        }
        received += static_cast<std::size_t>(amount);
    }
}

void RandomStream::readFrame(void* mapped, std::size_t payloadBytes,
                             std::uint32_t count, std::uint32_t blocks,
                             std::uint32_t epoch, std::uint32_t seed,
                             std::uint32_t workload) {
    if(!mapped || !count || !blocks || workload > 1)
        throw std::invalid_argument("Invalid destination or dimensions/workload for RNG frame");
    if(std::uint64_t(count) * blocks > std::numeric_limits<std::size_t>::max() / 16 ||
       payloadBytes != std::uint64_t(count) * blocks * 16)
        throw std::invalid_argument("RNG frame payload byte count does not match count*blocks*16");

    std::array<unsigned char, randomStreamHeaderBytes> header{};
    readExact(header.data(), header.size(), "frame header");
    const std::array<std::uint32_t, 8> expected{
        randomStreamMagic, randomStreamVersion, count, blocks, epoch, seed, workload, 0};
    constexpr const char* names[]{"magic", "version", "count", "blocks", "epoch", "seed", "workload", "flags"};
    for(std::size_t field = 0; field < expected.size(); ++field) {
        const std::uint32_t actual = littleWord(header.data() + field * 4);
        if(actual != expected[field]) {
            throw std::runtime_error("RNG frame " + std::string(names[field]) + " mismatch in '" + path_ +
                "': expected " + std::to_string(expected[field]) + ", received " + std::to_string(actual));
        }
    }
    readExact(mapped, payloadBytes, "frame payload");
    // ARM64 Jetson, Apple Silicon and x86 hosts are little-endian. Keep the
    // reader's scalar representation portable if used with a big-endian host.
    const std::uint32_t endianProbe = 1;
    if(*reinterpret_cast<const unsigned char*>(&endianProbe) != 1) {
        auto* bytes = static_cast<unsigned char*>(mapped);
        for(std::size_t offset = 0; offset < payloadBytes; offset += 4) {
            const auto value = littleWord(bytes + offset);
            std::memcpy(bytes + offset, &value, sizeof(value));
        }
    }
}

} // namespace rnglab
