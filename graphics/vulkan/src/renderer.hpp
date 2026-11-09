#pragma once

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class CameraController;
class GameInput;
struct ComputeContext;

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
    float theta = 0.5f; // Barnes-Hut opening angle: smaller is more accurate.
};

enum class SimulationMode { CpuAllPairs, CpuBarnesHut, GpuAllPairs, GpuBarnesHut };

struct RendererOptions {
    SimulationMode mode = SimulationMode::GpuAllPairs;
    bool measureTimings = false;
    bool uncappedPresentation = false;
};

struct FrameTimings {
    double cpuBuildMs = 0, cpuForceMs = 0, cpuIntegrateMs = 0;
    double uploadMs = 0, recordMs = 0, fenceWaitMs = 0, acquireMs = 0;
    double submitMs = 0, presentMs = 0;
    double gpuBuildMs = 0, gpuForceMs = 0, gpuRenderMs = 0;
};

// The application computes these matrices; the renderer only transfers them.
struct FrameTransforms {
    alignas(16) glm::mat4 view{1.0f};
    alignas(16) glm::mat4 projection{1.0f};
};

// Game-owned objects, independent of the N-body simulation. tint.w < 0 means
// a clip-space HUD rectangle; otherwise transform is an ordinary world matrix.
struct alignas(16) SceneInstance {
    glm::mat4 transform{1.0f};
    glm::vec4 tint{1.0f};
};

class Renderer {
public:
    // Uploads the shared mesh once. The selected backend owns particle updates;
    // GPU modes upload initial particles once and retain evolving state on GPU.
    Renderer(
        std::uint32_t width,
        std::uint32_t height,
        const char* title,
        const std::vector<Mesh>& meshes,
        const std::vector<gpu_particle>& initialParticles,
        const std::string& texturePath,
        const RendererOptions& options = {});
    ~Renderer();

    Renderer(std::uint32_t width, std::uint32_t height, const char* title,
        const Mesh& mesh, std::uint32_t instanceCapacity);
    void drawScene(const FrameTransforms&, const std::vector<SceneInstance>&);
    // Debug-only: the next submitted scene frame is copied to a BMP. This one
    // frame waits for readback; normal interactive rendering never does this.
    void captureNextFrame(const std::string& path);
    // Prediction uses this renderer's device/queue, on the same host thread.
    // The predictor must be destroyed before the renderer.
    ComputeContext computeContext() const;

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool shouldClose() const;
    void pollEvents() const;
    float aspectRatio() const;
    // One simulation step on the selected backend, then render the shared mesh.
    void drawFrame(
        const FrameTransforms& transforms,
        const SimulationParameters& simulation);

    // Debug/testing only: waits for the GPU and copies its current state back.
    // The normal rendering loop does not call this.
    std::vector<gpu_particle> readParticles();

    // Benchmark only: wait for the most recently submitted frame, then read its
    // timestamps. This measures completed frames rather than queued CPU work.
    FrameTimings finishFrame();

private:
    friend class CameraController;
    friend class GameInput;

    // Used by platform-facing helpers such as CameraController. GLFW remains
    // out of this public header and out of the application code.
    void* nativeWindowHandle() const;

    class Impl;
    std::unique_ptr<Impl> impl;
};
