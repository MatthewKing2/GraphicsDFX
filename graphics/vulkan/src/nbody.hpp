#pragma once
#include "renderer.hpp"

// Heap-indexed binary spatial tree: root 1, children 2*i and 2*i+1.
// Leaves are particles ordered by Morton code. CPU/GPU use identical layouts.
struct alignas(16) BhNode {
    glm::vec4 centerMass; // center of mass XYZ, total mass W
    glm::vec4 boundsMin;  // AABB minimum XYZ, first sorted rank W
    glm::vec4 boundsMax;  // AABB maximum XYZ, number of leaf slots W
};

struct CpuStepTimings { double buildMs = 0, forceMs = 0, integrateMs = 0; };

class CpuNBody {
public:
    CpuNBody(const std::vector<gpu_particle>& initial, bool barnesHut);
    CpuStepTimings step(const SimulationParameters& params, bool measureTimings = false);
    const std::vector<gpu_particle>& particles() const { return state; }
    const std::vector<glm::mat4>& models() const { return modelMatrices; }
private:
    struct Key { uint32_t code, index; };
    bool useTree;
    uint32_t leafCount = 1;
    std::vector<gpu_particle> state, next;
    std::vector<glm::vec3> accelerations;
    std::vector<glm::mat4> modelMatrices;
    std::vector<Key> order;
    std::vector<uint32_t> ranks;
    std::vector<BhNode> tree;
    void buildTree();
    glm::vec3 treeAcceleration(uint32_t particle, const SimulationParameters& params) const;
};
