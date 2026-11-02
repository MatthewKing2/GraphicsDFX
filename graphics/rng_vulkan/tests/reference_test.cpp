#include "reference.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
}

int main() {
    try {
        using namespace rnglab;
        // Published Random123 known-answer vectors, Philox4x32 with 10 rounds:
        // https://github.com/DEShawResearch/random123/blob/main/tests/kat_vectors
        require(philox({0, 0, 0, 0}, {0, 0}) ==
                    Words{0x6627e8d5u, 0xe169c58du, 0xbc57ac4cu, 0x9b00dbd8u},
                "zero-counter Philox known-answer vector failed");
        constexpr auto max = std::numeric_limits<std::uint32_t>::max();
        require(philox({max, max, max, max}, {max, max}) ==
                    Words{0x408f276du, 0x41c83b0eu, 0xa20bc7c6u, 0x6d5451fdu},
                "all-ones Philox known-answer vector failed");
        require(philox({0x243f6a88u, 0x85a308d3u, 0x13198a2eu, 0x03707344u},
                       {0xa4093822u, 0x299f31d0u}) ==
                    Words{0xd16cfe09u, 0x94fdccebu, 0x5001e420u, 0x24126ea1u},
                "mixed-counter Philox known-answer vector failed");
        require(uniform(0) == 0.0f, "uniform zero endpoint failed");
        require(uniform(max) == 1.0f - 1.0f / 16777216.0f,
                "uniform maximum endpoint failed");
        require(uniform(max) < 1.0f, "uniform includes forbidden endpoint");
        require(randomBlock(123, 456, 789, 0, 42) == randomBlock(123, 456, 789, 0, 42),
                "counter mapping is not deterministic");
        require(randomBlock(123, 456, 789, 0, 42) != randomBlock(123, 456, 789, 1, 42),
                "workload domains are not separated");
        require(inCircle(0, 0) && inCircle(1, 0) && !inCircle(1, 1),
                "Monte Carlo geometry failed");

        std::uint64_t hits = 0;
        double sum = 0;
        constexpr unsigned count = 65536;
        for (unsigned id = 0; id < count; ++id) {
            const auto words = randomBlock(id, 0, 0, 0, 42);
            for (const auto word : words) sum += uniform(word);
            hits += inCircle(uniform(words[0]), uniform(words[1]));
            hits += inCircle(uniform(words[2]), uniform(words[3]));
        }
        const double mean = sum / (4.0 * count);
        const double pi = 4.0 * static_cast<double>(hits) / (2.0 * count);
        // These are broad deterministic sanity checks, not a statistical
        // certification of the RNG. Known-answer vectors guard its arithmetic.
        require(std::abs(mean - 0.5) < 0.01, "uniform mean sanity check failed");
        require(std::abs(pi - 3.141592653589793) < 0.025,
                "Monte Carlo pi sanity check failed");
        require(walkCoordinate(0.999f, uniform(max), 0.01f) < -0.98f,
                "positive periodic crossing failed");
        require(walkCoordinate(-0.999f, 0.0f, 0.01f) > 0.98f,
                "negative periodic crossing failed");

        double walkMeanX = 0;
        double walkMeanY = 0;
        double walkMSD = 0;
        constexpr unsigned walkers = 16384;
        constexpr unsigned blocks = 64;
        constexpr float step = 0.01f;
        for (unsigned id = 0; id < walkers; ++id) {
            const auto state = simulateState({}, id, blocks, 0, 42, 1, step);
            const double x = fromBits(state[0]);
            const double y = fromBits(state[1]);
            require(x >= -1 && x < 1 && y >= -1 && y < 1,
                    "walk escaped periodic domain");
            walkMeanX += x;
            walkMeanY += y;
            walkMSD += x * x + y * y;
        }
        walkMeanX /= walkers;
        walkMeanY /= walkers;
        walkMSD /= walkers;
        // Each coordinate increment is uniform [-step,+step), with variance
        // step^2/3. Two steps/block gives E[r^2] = 4*blocks*step^2/3 before
        // wrapping matters. This case stays many standard deviations inside
        // the boundary, so it tests the expected early-time diffusion rate.
        const double expectedMSD = 4.0 * blocks * step * step / 3.0;
        require(std::abs(walkMeanX) < 0.003 && std::abs(walkMeanY) < 0.003,
                "random walk has unexpectedly large drift");
        require(std::abs(walkMSD / expectedMSD - 1.0) < 0.06,
                "random-walk diffusion rate sanity check failed");

        const auto epoch0 = simulateState({}, 9, 7, 0, 42, 1, step);
        const auto epoch1 = simulateState(epoch0, 9, 7, 1, 42, 1, step);
        require(epoch1 != epoch0, "walk state did not advance in the next epoch");
        require(simulateState(epoch1, 9, 7, 0, 42, 1, step) == epoch0,
                "epoch zero did not reset walk to center");
        std::cout << "Philox KATs, uniform conversion, domains, pi and diffusion sanity passed.\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
