#include "nbody.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

using CpuClock = std::chrono::steady_clock;
static double elapsed(CpuClock::time_point start) {
    return std::chrono::duration<double, std::milli>(CpuClock::now() - start).count();
}
static uint32_t spreadBits(uint32_t x) {
    x = (x | (x << 16)) & 0x030000ffu;
    x = (x | (x << 8)) & 0x0300f00fu;
    x = (x | (x << 4)) & 0x030c30c3u;
    x = (x | (x << 2)) & 0x09249249u;
    return x;
}
static glm::vec3 force(glm::vec3 position, glm::vec4 source, const SimulationParameters& p) {
    const glm::vec3 dist = glm::vec3(source) - position;
    const float inverse = 1.0f / std::sqrt(glm::dot(dist, dist) + p.softening * p.softening);
    return (p.gravityConstant * source.w * inverse * inverse * inverse) * dist;
}

CpuNBody::CpuNBody(const std::vector<gpu_particle>& initial, bool barnesHut)
    : useTree(barnesHut), state(initial), next(initial),
      accelerations(initial.size()), modelMatrices(initial.size()) {
    if (useTree) {
        while (leafCount < initial.size()) leafCount *= 2;
        order.resize(initial.size());
        ranks.resize(initial.size());
        tree.resize(size_t(leafCount) * 2);
    }
}

void CpuNBody::buildTree() {
    glm::vec3 minimum(std::numeric_limits<float>::max());
    glm::vec3 maximum(-std::numeric_limits<float>::max());
    for (const auto& particle : state) {
        minimum = glm::min(minimum, glm::vec3(particle.positionMass));
        maximum = glm::max(maximum, glm::vec3(particle.positionMass));
    }
    const glm::vec3 extent = glm::max(maximum - minimum, glm::vec3(1.0e-6f));
    for (uint32_t i = 0; i < state.size(); ++i) {
        const auto normalized = glm::clamp((glm::vec3(state[i].positionMass) - minimum) / extent,
            glm::vec3(0.0f), glm::vec3(1.0f));
        const glm::uvec3 q(normalized * 1023.0f);
        order[i] = {spreadBits(q.x) | (spreadBits(q.y) << 1) | (spreadBits(q.z) << 2), i};
    }
    std::sort(order.begin(), order.end(), [](const Key& a, const Key& b) {
        return a.code < b.code || (a.code == b.code && a.index < b.index);
    });
    for (uint32_t rank = 0; rank < leafCount; ++rank) {
        auto& node = tree[leafCount + rank];
        if (rank < state.size()) {
            const auto index = order[rank].index;
            ranks[index] = rank;
            node.centerMass = state[index].positionMass;
            node.boundsMin = glm::vec4(glm::vec3(node.centerMass), float(rank));
            node.boundsMax = glm::vec4(glm::vec3(node.centerMass), 1.0f);
        } else {
            node.centerMass = glm::vec4(0.0f);
            node.boundsMin = glm::vec4(glm::vec3(std::numeric_limits<float>::max()), float(rank));
            node.boundsMax = glm::vec4(glm::vec3(-std::numeric_limits<float>::max()), 1.0f);
        }
    }
    // Children have larger heap indices, so this order is bottom-up.
    for (uint32_t i = leafCount - 1; i > 0; --i) {
        const auto& left = tree[i * 2];
        const auto& right = tree[i * 2 + 1];
        auto& node = tree[i];
        const float mass = left.centerMass.w + right.centerMass.w;
        const glm::vec3 center = mass > 0.0f
            ? (glm::vec3(left.centerMass) * left.centerMass.w
                + glm::vec3(right.centerMass) * right.centerMass.w) / mass : glm::vec3(0.0f);
        node.centerMass = glm::vec4(center, mass);
        node.boundsMin = glm::vec4(glm::min(glm::vec3(left.boundsMin), glm::vec3(right.boundsMin)), left.boundsMin.w);
        node.boundsMax = glm::vec4(glm::max(glm::vec3(left.boundsMax), glm::vec3(right.boundsMax)),
            left.boundsMax.w + right.boundsMax.w);
    }
}

glm::vec3 CpuNBody::treeAcceleration(uint32_t i, const SimulationParameters& p) const {
    const glm::vec3 position(state[i].positionMass);
    glm::vec3 acceleration(0.0f);
    uint32_t index = 1;
    const float rank = float(ranks[i]);
    while (index) {
        const auto& node = tree[index];
        const bool containsMe = rank >= node.boundsMin.w && rank < node.boundsMin.w + node.boundsMax.w;
        const glm::vec3 dist = glm::vec3(node.centerMass) - position;
        const glm::vec3 dimensions = glm::vec3(node.boundsMax) - glm::vec3(node.boundsMin);
        const float size = std::max({dimensions.x, dimensions.y, dimensions.z});
        const bool leaf = index >= leafCount;
        const bool accept = !containsMe && (leaf || size * size < p.theta * p.theta * glm::dot(dist, dist));
        if (node.centerMass.w != 0.0f && accept) {
            acceleration += force(position, node.centerMass, p);
        }
        if (node.centerMass.w != 0.0f && !accept && !leaf) index *= 2;
        else {
            while (index > 1 && (index & 1u)) index /= 2;
            index = index == 1 ? 0 : index + 1;
        }
    }
    return acceleration;
}

CpuStepTimings CpuNBody::step(const SimulationParameters& p, bool measureTimings) {
    CpuStepTimings timings;
    auto mark = [&]() { return measureTimings ? CpuClock::now() : CpuClock::time_point{}; };
    auto since = [&](CpuClock::time_point start) { return measureTimings ? elapsed(start) : 0.0; };
    auto start = mark();
    if (useTree) buildTree();
    timings.buildMs = useTree ? since(start) : 0.0;
    start = mark();
    for (uint32_t i = 0; i < state.size(); ++i) {
        glm::vec3 acceleration(0.0f);
        if (useTree) acceleration = treeAcceleration(i, p);
        else {
            const glm::vec3 position(state[i].positionMass);
            for (uint32_t j = 0; j < state.size(); ++j) {
                if (i != j) acceleration += force(position, state[j].positionMass, p);
            }
        }
        accelerations[i] = acceleration;
    }
    timings.forceMs = since(start);
    start = mark();
    for (uint32_t i = 0; i < state.size(); ++i) {
        const glm::vec3 velocity = glm::vec3(state[i].velocity) + p.stepSize * accelerations[i];
        const glm::vec3 position = glm::vec3(state[i].positionMass) + p.stepSize * velocity;
        next[i] = {glm::vec4(position, state[i].positionMass.w), glm::vec4(velocity, 0.0f)};
        modelMatrices[i] = glm::mat4(1.0f);
        modelMatrices[i][3] = glm::vec4(position, 1.0f);
    }
    state.swap(next);
    timings.integrateMs = since(start);
    return timings;
}
