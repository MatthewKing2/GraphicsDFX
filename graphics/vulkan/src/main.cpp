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


// -1   -0.5    0   0.5     1 
// -0.5   *          *
// 0
// 0.5    *          *
// 1


Mesh makeQuad() {
    Mesh matthew_quad;  // confused about the memory managment here, is this on the stack or the heap. id guess stack but then idk how we are returning it? i guess that we can return it by just pushing the values onto the stack? can return more than just an interger value? should know this tbh

    Vertex matthew_v1{};
    matthew_v1.position = {-0.5f, -0.5f, 0.5f};         // glm::vec3 is 3-component fp32 vector, position = x,y,z
    matthew_v1.color = {168.0f, 50.0f, 147.0f};         // glm::vec3 is 3-component fp32 vector, color    = r,g,b
    matthew_v1.textureCoordinate = {0.0f, 0.0f};        // glm::vec2 is 2-component fp32 vector, text coordinates are now the textecure maps to the surface, ignoring for now
    matthew_quad.vertices.push_back(matthew_v1);
    matthew_quad.indices.push_back(0);

    Vertex matthew_v2{};
    matthew_v2.position = {-0.5f, 0.5f, 0.5f};          // glm::vec3 is 3-component fp32 vector, position = x,y,z
    matthew_v2.color = {168.0f, 50.0f, 147.0f};         // glm::vec3 is 3-component fp32 vector, color    = r,g,b
    matthew_v2.textureCoordinate = {0.0f, 0.0f};        // glm::vec2 is 2-component fp32 vector, text coordinates are now the textecure maps to the surface, ignoring for now
    matthew_quad.vertices.push_back(matthew_v2);
    matthew_quad.indices.push_back(1);

    Vertex matthew_v3{};
    matthew_v3.position = {0.5f, -0.5f, 0.5f};          // glm::vec3 is 3-component fp32 vector, position = x,y,z
    matthew_v3.color = {168.0f, 50.0f, 147.0f};         // glm::vec3 is 3-component fp32 vector, color    = r,g,b
    matthew_v3.textureCoordinate = {0.0f, 0.0f};        // glm::vec2 is 2-component fp32 vector, text coordinates are now the textecure maps to the surface, ignoring for now
    matthew_quad.vertices.push_back(matthew_v3);
    matthew_quad.indices.push_back(2);

    Vertex matthew_v4{};
    matthew_v4.position = {0.5f, 0.5f, 0.5f};           // glm::vec3 is 3-component fp32 vector, position = x,y,z
    matthew_v4.color = {168.0f, 50.0f, 147.0f};         // glm::vec3 is 3-component fp32 vector, color    = r,g,b
    matthew_v4.textureCoordinate = {0.0f, 0.0f};        // glm::vec2 is 2-component fp32 vector, text coordinates are now the textecure maps to the surface, ignoring for now
    matthew_quad.vertices.push_back(matthew_v4);
    matthew_quad.indices.push_back(2);                  //  
    matthew_quad.indices.push_back(1);
    matthew_quad.indices.push_back(3);
    // indices tells the GPU which vertices to connect, using their positions in the vertices vector.
    // For example, when drawing triangles: indices = {0, 1, 2,  2, 1, 3};
    // Each group of three describes one triangle:
        // - First triangle: vertices[0], vertices[1], vertices[2].
        // - Second triangle: vertices[2], vertices[1], vertices[3].

    return matthew_quad;
}


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
        glm::vec3(0.0f, 1.0f, 0.0f));

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
        // Mesh mesh = loadObj(MODEL_PATH);
        Mesh mesh = makeQuad();

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
