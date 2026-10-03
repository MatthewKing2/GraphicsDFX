#pragma once

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

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
struct SceneTransforms {
    alignas(16) glm::mat4 model;
    alignas(16) glm::mat4 view;
    alignas(16) glm::mat4 projection;
};

class Renderer {
public:
    Renderer(
        std::uint32_t width,
        std::uint32_t height,
        const char* title,
        const Mesh& mesh,
        const std::string& texturePath);
    ~Renderer();

    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    bool shouldClose() const;
    void pollEvents() const;
    float aspectRatio() const;
    void drawFrame(const SceneTransforms& transforms);

private:
    class Impl;
    std::unique_ptr<Impl> impl;
};
