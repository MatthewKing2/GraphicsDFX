#include "renderer.hpp"

#define TINYOBJLOADER_IMPLEMENTATION
#include <tiny_obj_loader.h>

#include <glm/gtc/matrix_transform.hpp>

#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_map>

namespace {

constexpr uint32_t WINDOW_WIDTH = 800;
constexpr uint32_t WINDOW_HEIGHT = 600;
const std::string MODEL_PATH = "models/viking_room.obj";
const std::string TEXTURE_PATH = "textures/viking_room.png";

void hashCombine(std::size_t& seed, float value) {
    seed ^= std::hash<float>{}(value) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
}

struct VertexHash {
    std::size_t operator()(const Vertex& vertex) const {
        std::size_t seed = 0;
        hashCombine(seed, vertex.position.x);
        hashCombine(seed, vertex.position.y);
        hashCombine(seed, vertex.position.z);
        hashCombine(seed, vertex.color.x);
        hashCombine(seed, vertex.color.y);
        hashCombine(seed, vertex.color.z);
        hashCombine(seed, vertex.textureCoordinate.x);
        hashCombine(seed, vertex.textureCoordinate.y);
        return seed;
    }
};

Mesh loadObj(const std::string& path) {
    tinyobj::attrib_t attributes;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warning;
    std::string error;

    if (!tinyobj::LoadObj(
            &attributes,
            &shapes,
            &materials,
            &warning,
            &error,
            path.c_str(),
            nullptr,
            true)) {
        throw std::runtime_error(error);
    }

    if (!warning.empty()) {
        std::cout << "OBJ warning: " << warning << '\n';
    }

    Mesh mesh;
    std::unordered_map<Vertex, uint32_t, VertexHash> uniqueVertices;

    for (const auto& shape : shapes) {
        for (const auto& index : shape.mesh.indices) {
            Vertex vertex{};
            vertex.position = {
                attributes.vertices[3 * index.vertex_index + 0],
                attributes.vertices[3 * index.vertex_index + 1],
                attributes.vertices[3 * index.vertex_index + 2]
            };
            vertex.color = {1.0f, 1.0f, 1.0f};

            if (index.texcoord_index >= 0) {
                vertex.textureCoordinate = {
                    attributes.texcoords[2 * index.texcoord_index + 0],
                    1.0f - attributes.texcoords[2 * index.texcoord_index + 1]
                };
            }

            auto [entry, inserted] =
                uniqueVertices.emplace(vertex, static_cast<uint32_t>(mesh.vertices.size()));
            if (inserted) {
                mesh.vertices.push_back(vertex);
            }
            mesh.indices.push_back(entry->second);
        }
    }

    return mesh;
}

SceneTransforms makeTransforms(float timeSeconds, float aspectRatio) {
    SceneTransforms transforms{};

    transforms.model = glm::rotate(
        glm::mat4(1.0f),
        timeSeconds * glm::radians(90.0f),
        glm::vec3(0.0f, 0.0f, 1.0f));

    transforms.view = glm::lookAt(
        glm::vec3(2.0f, 2.0f, 2.0f),
        glm::vec3(0.0f, 0.0f, 0.0f),
        glm::vec3(0.0f, 0.0f, 1.0f));

    transforms.projection =
        glm::perspective(glm::radians(45.0f), aspectRatio, 0.1f, 10.0f);

    // GLM uses an OpenGL-style Y axis; Vulkan's clip-space Y axis is inverted.
    transforms.projection[1][1] *= -1.0f;
    return transforms;
}

} // namespace

int main() {
    try {
        Mesh mesh = loadObj(MODEL_PATH);
        Renderer renderer(
            WINDOW_WIDTH,
            WINDOW_HEIGHT,
            "Vulkan learning renderer",
            mesh,
            TEXTURE_PATH);

        const auto startTime = std::chrono::steady_clock::now();
        while (!renderer.shouldClose()) {
            renderer.pollEvents();

            const float timeSeconds = std::chrono::duration<float>(
                std::chrono::steady_clock::now() - startTime).count();

            renderer.drawFrame(makeTransforms(timeSeconds, renderer.aspectRatio()));
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
