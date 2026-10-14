#pragma once

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class CameraController;

// CPU-side geometry. The application owns and fills this ordinary memory.
struct Vertex {
    glm::vec3 position;
    glm::vec3 color;
    glm::vec2 textureCoordinate;

    bool operator==(const Vertex& other) const {
        return position == other.position &&
               color == other.color &&
               textureCoordinate == other.textureCoordinate;
    }
};

struct Mesh {
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
};

// Same 32-byte layout as gpu_particle in nbody.comp (std430).
struct alignas(16) gpu_particle {
    glm::vec4 positionMass; // x, y, z, mass
    glm::vec4 velocity;     // x, y, z, unused (zero)
};

struct SimulationParameters {
    float stepSize = 0.001f;
    float softening = 0.75f;
    float gravityConstant = 1.0f;
};

// The application computes these matrices; the renderer only transfers them.
struct FrameTransforms {
    alignas(16) glm::mat4 view{1.0f};
    alignas(16) glm::mat4 projection{1.0f};
};

class Renderer {
public:
    // Uploads the shared mesh and initial particles once. Subsequent particle
    // updates and model-matrix generation happen entirely on the GPU.
    Renderer(
        std::uint32_t width,
        std::uint32_t height,
        const char* title,
        const std::vector<Mesh>& meshes,
        const std::vector<gpu_particle>& initialParticles,
        const std::string& texturePath);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool shouldClose() const;
    void pollEvents() const;
    float aspectRatio() const;
    // One GPU simulation step, then draw all particles using the shared mesh.
    void drawFrame(
        const FrameTransforms& transforms,
        const SimulationParameters& simulation);

    // Debug/testing only: waits for the GPU and copies its current state back.
    // The normal rendering loop does not call this.
    std::vector<gpu_particle> readParticles();

private:
    friend class CameraController;

    // Used by platform-facing helpers such as CameraController. GLFW remains
    // out of this public header and out of the application code.
    void* nativeWindowHandle() const;

    class Impl;
    std::unique_ptr<Impl> impl;
};
