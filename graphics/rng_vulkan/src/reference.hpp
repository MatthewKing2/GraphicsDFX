#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace rnglab {

using Words = std::array<std::uint32_t, 4>;
using Key = std::array<std::uint32_t, 2>;

// Philox4x32-10, independently expressed from the published algorithm.
// Specification: Salmon et al., SC11, doi:10.1145/2063384.2063405.
// The test vectors in tests/reference_test.cpp come from Random123's
// tests/kat_vectors; no upstream implementation is required at build time.
inline Words philox(Words counter, Key key) {
    for (unsigned round = 0; round < 10; ++round) {
        const auto a = std::uint64_t{0xd2511f53u} * counter[0];
        const auto b = std::uint64_t{0xcd9e8d57u} * counter[2];
        counter = {static_cast<std::uint32_t>(b >> 32) ^ counter[1] ^ key[0],
                   static_cast<std::uint32_t>(b),
                   static_cast<std::uint32_t>(a >> 32) ^ counter[3] ^ key[1],
                   static_cast<std::uint32_t>(a)};
        key[0] += 0x9e3779b9u;
        key[1] += 0xbb67ae85u;
    }
    return counter;
}

// Dropping the low eight bits makes every conversion exact in binary32,
// including the upper endpoint: output is in [0,1), never rounded to 1.
inline float uniform(std::uint32_t value) {
    return static_cast<float>(value >> 8) * (1.0f / 16777216.0f);
}

inline Words randomBlock(std::uint32_t id, std::uint32_t block,
                         std::uint32_t epoch, std::uint32_t domain,
                         std::uint32_t seed) {
    return philox({id, block, epoch, domain}, {seed, 0});
}

inline bool inCircle(float x, float y) {
    // Separate products match GLSL 'precise' and avoid platform-dependent FMA.
    const float xx = x * x;
    const float yy = y * y;
    return xx + yy <= 1.0f;
}

inline std::uint32_t floatBits(float value) {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

inline float fromBits(std::uint32_t bits) {
    float value;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline float walkCoordinate(float position, float sample, float stepSize) {
    const float centered = sample * 2.0f - 1.0f;
    const float increment = centered * stepSize;
    const float next = position + increment;
    // A step size <= 1 guarantees at most one boundary crossing per step.
    return next < -1.0f ? next + 2.0f : (next >= 1.0f ? next - 2.0f : next);
}

// CPU oracle for checking GPU readbacks. Workload 0 is independent pi sampling;
// workload 1 is a persistent periodic random walk with uniform XY increments.
inline Words simulateState(Words old, std::uint32_t id, std::uint32_t blocks,
                           std::uint32_t epoch, std::uint32_t seed,
                           std::uint32_t workload, float stepSize) {
    Words last{};
    std::uint32_t hits = 0;
    float x = epoch == 0 ? 0.0f : fromBits(old[0]);
    float y = epoch == 0 ? 0.0f : fromBits(old[1]);
    for (std::uint32_t block = 0; block < blocks; ++block) {
        last = randomBlock(id, block, epoch, workload, seed);
        const float a = uniform(last[0]);
        const float b = uniform(last[1]);
        const float c = uniform(last[2]);
        const float d = uniform(last[3]);
        if (workload == 0) {
            hits += inCircle(a, b);
            hits += inCircle(c, d);
        } else {
            x = walkCoordinate(x, a, stepSize);
            y = walkCoordinate(y, b, stepSize);
            x = walkCoordinate(x, c, stepSize);
            y = walkCoordinate(y, d, stepSize);
        }
    }
    return workload == 0 ? Words{hits, last[2], last[3], 0}
                         : Words{floatBits(x), floatBits(y), 0, 0};
}

} // namespace rnglab
