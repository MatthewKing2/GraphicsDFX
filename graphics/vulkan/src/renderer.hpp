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

// The application computes these matrices; the renderer only transfers them.
struct FrameTransforms {
    alignas(16) glm::mat4 view{1.0f};
    alignas(16) glm::mat4 projection{1.0f};
};

class Renderer {
public:
    // Uploads a fixed list of meshes once. Later edits to the CPU meshes do not
    // change the uploaded geometry; the caller may also release the CPU copies.
    Renderer(
        std::uint32_t width,
        std::uint32_t height,
        const char* title,
        const std::vector<Mesh>& meshes,
        const std::string& texturePath);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool shouldClose() const;
    void pollEvents() const;
    float aspectRatio() const;
    // modelMatrices[i] transforms meshes[i]; the counts must match exactly.
    // Only matrices are transferred each frame. No CPU pointers are retained.
    void drawFrame(
        const FrameTransforms& transforms,
        const std::vector<glm::mat4>& modelMatrices);

private:
    friend class CameraController;

    // Used by platform-facing helpers such as CameraController. GLFW remains
    // out of this public header and out of the application code.
    void* nativeWindowHandle() const;

    class Impl;
    std::unique_ptr<Impl> impl;
};
