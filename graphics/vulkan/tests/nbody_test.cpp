#include "../src/renderer.hpp"
#include "../src/nbody.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <stdexcept>

// Keep a small CPU reference so shader changes can be checked numerically.
static void cpuStep(std::vector<gpu_particle>& particles, const SimulationParameters& params) {
    auto next = particles;
    for (size_t i = 0; i < particles.size(); ++i) {
        glm::vec3 acceleration(0.0f);
        for (size_t j = 0; j < particles.size(); ++j) {
            if (i == j) continue;
            const glm::vec3 dist = glm::vec3(particles[j].positionMass)
                - glm::vec3(particles[i].positionMass);
            const float denominator = std::pow(glm::dot(dist, dist)
                + params.softening * params.softening, 1.5f);
            acceleration += (params.gravityConstant * particles[j].positionMass.w / denominator) * dist;
        }
        const glm::vec3 velocity = glm::vec3(particles[i].velocity) + params.stepSize * acceleration;
        const glm::vec3 position = glm::vec3(particles[i].positionMass) + params.stepSize * velocity;
        next[i].positionMass = glm::vec4(position, particles[i].positionMass.w);
        next[i].velocity = glm::vec4(velocity, 0.0f);
    }
    particles = std::move(next);
}

static float compare(const std::vector<gpu_particle>& expected,
                     const std::vector<gpu_particle>& actual) {
    if (expected.size() != actual.size()) throw std::runtime_error("particle count mismatch");
    float worst = 0.0f;
    for (size_t i = 0; i < expected.size(); ++i) {
        for (int field = 0; field < 2; ++field) {
            const auto& a = field == 0 ? actual[i].positionMass : actual[i].velocity;
            const auto& e = field == 0 ? expected[i].positionMass : expected[i].velocity;
            for (int component = 0; component < 4; ++component) {
                const float error = std::abs(a[component] - e[component]);
                if (!std::isfinite(a[component]) || error > 2.0e-5f * (1.0f + std::abs(e[component]))) {
                    throw std::runtime_error("GPU/CPU mismatch at particle " + std::to_string(i));
                }
                worst = std::max(worst, error);
            }
        }
        if (actual[i].positionMass.w != expected[i].positionMass.w || actual[i].velocity.w != 0.0f) {
            throw std::runtime_error("mass or velocity padding changed");
        }
    }
    return worst;
}

static Mesh testMesh() {
    Mesh mesh;
    mesh.vertices = {
        {{-0.02f, -0.02f, 0.0f}, {1.0f, 1.0f, 1.0f}, {}},
        {{ 0.02f, -0.02f, 0.0f}, {1.0f, 1.0f, 1.0f}, {}},
        {{ 0.00f,  0.02f, 0.0f}, {1.0f, 1.0f, 1.0f}, {}}
    };
    mesh.indices = {0, 1, 2};
    return mesh;
}

static void runCase(const char* label, std::vector<gpu_particle> particles, int steps,
                    SimulationMode mode = SimulationMode::GpuAllPairs) {
    const SimulationParameters params = {0.001f, 0.75f, 1.0f, 0.0f};
    RendererOptions options;
    options.mode = mode;
    Renderer renderer(320, 240, "GPU N-body correctness test", {testMesh()},
        particles, "textures/viking_room.png", options);
    compare(particles, renderer.readParticles()); // Check the initial upload/layout.

    auto invalid = params;
    invalid.softening = 0.0f;
    bool rejected = false;
    try { renderer.drawFrame(FrameTransforms{}, invalid); }
    catch (const std::invalid_argument&) { rejected = true; }
    if (!rejected) throw std::runtime_error("zero softening was not rejected");

    // Queue multiple frames without per-step readback to exercise in-flight reuse.
    for (int step = 0; step < steps; ++step) {
        renderer.pollEvents();
        renderer.drawFrame(FrameTransforms{}, params);
        cpuStep(particles, params);
    }
    const float worst = compare(particles, renderer.readParticles());
    std::cout << "PASS " << label << ": " << particles.size() << " particles, "
              << steps << " steps, max absolute error=" << worst << '\n';

    // A readback must not interfere with the next simulation step.
    renderer.drawFrame(FrameTransforms{}, params);
    cpuStep(particles, params);
    compare(particles, renderer.readParticles());
}

static void checkAccuracy(const char* label, std::vector<gpu_particle> particles) {
    for (auto& particle : particles) particle.velocity = glm::vec4(0.0f);
    const SimulationParameters params{0.001f, 0.75f, 1.0f, 0.5f};
    CpuNBody direct(particles, false), approximate(particles, true);
    direct.step(params); approximate.step(params);
    RendererOptions options;
    options.mode = SimulationMode::GpuBarnesHut;
    Renderer renderer(320, 240, "Barnes-Hut accuracy test", {testMesh()}, particles,
        "textures/viking_room.png", options);
    renderer.drawFrame(FrameTransforms{}, params);
    compare(approximate.particles(), renderer.readParticles());
    double errorSquared = 0.0, referenceSquared = 0.0;
    for (size_t i = 0; i < particles.size(); ++i) {
        const glm::vec3 expected(direct.particles()[i].velocity);
        const glm::vec3 error = glm::vec3(approximate.particles()[i].velocity) - expected;
        errorSquared += glm::dot(error, error);
        referenceSquared += glm::dot(expected, expected);
    }
    const double relativeRms = std::sqrt(errorSquared / referenceSquared);
    if (relativeRms > 0.05) throw std::runtime_error("Barnes-Hut force error exceeds 5%");
    std::cout << "PASS " << label << " Barnes-Hut CPU/GPU match, theta=0.5 relative acceleration RMS="
              << relativeRms << '\n';
    // Evolving state must rebuild a correct tree, not just handle one snapshot.
    for (int step = 0; step < 5; ++step) {
        approximate.step(params);
        renderer.drawFrame(FrameTransforms{}, params);
        compare(approximate.particles(), renderer.readParticles());
    }
}

int main() {
    try {
        runCase("stationary", {{{0, 0, 0, 1}, {0, 0, 0, 0}}}, 3);
        runCase("linear motion", {{{0, 0, 0, 1}, {1, -2, 3, 0}}}, 3);
        runCase("symmetric pair", {
            {{-1, 0, 0, 1}, {0, 0, 0, 0}},
            {{ 1, 0, 0, 1}, {0, 0, 0, 0}}
        }, 8);

        std::mt19937 engine(12345);
        std::uniform_real_distribution<float> position(-2.0f, 2.0f);
        std::uniform_real_distribution<float> velocity(-0.2f, 0.2f);
        std::uniform_real_distribution<float> mass(0.1f, 2.0f);
        for (int count : {129, 1500}) {
            std::vector<gpu_particle> particles;
            for (int i = 0; i < count; ++i) {
                particles.push_back({
                    {position(engine), position(engine), position(engine), mass(engine)},
                    {velocity(engine), velocity(engine), velocity(engine), 0.0f}});
            }
            // Coincident particles check that softening keeps the force finite.
            particles[1].positionMass = glm::vec4(glm::vec3(particles[0].positionMass), 2.0f);
            runCase("random 3D / partial workgroup", particles, count == 129 ? 25 : 3);
            if (count == 129) {
                for (auto mode : {SimulationMode::CpuAllPairs, SimulationMode::CpuBarnesHut,
                                  SimulationMode::GpuBarnesHut}) {
                    runCase("four-mode exact check (theta=0)", particles, 3, mode);
                    runCase("coincident zero-mass / one leaf", {{{0, 0, 0, 0}, {1, 0, 0, 0}}}, 2, mode);
                }
            } else {
                checkAccuracy("random 3D", particles);
            }
        }
        // Same disk, seed, total mass and central object as the benchmark.
        std::mt19937 diskEngine(12345);
        std::uniform_real_distribution<float> radiusDistribution(0.2f, 4.0f);
        std::uniform_real_distribution<float> angleDistribution(0.0f, float(2 * 3.14156));
        std::vector<gpu_particle> disk;
        for (int i = 0; i < 1499; ++i) {
            const float radius = radiusDistribution(diskEngine), angle = angleDistribution(diskEngine);
            disk.push_back({{radius * std::cos(angle), radius * std::sin(angle), 0.0f, 750.0f / 1499}, glm::vec4(0.0f)});
        }
        disk.push_back({{0, 0, 0, 500}, glm::vec4(0.0f)});
        checkAccuracy("benchmark disk", disk);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
