#include "philox.glsl"

layout(push_constant) uniform Parameters {
    uint count;
    uint blocks;
    uint epoch;
    uint seed;
    uint workload;
    uint mode;
    uint visual_samples;
    float step_size;
} params;

const uint IMAGE_SIZE = 512u;

uvec4 randomBlock(uint id, uint block) {
    return philox(uvec4(id, block, params.epoch, params.workload),
                  uvec2(params.seed, 0u));
}

bool inCircle(vec2 samplePosition) {
    precise vec2 squared = samplePosition * samplePosition;
    precise float radiusSquared = squared.x + squared.y;
    return radiusSquared <= 1.0;
}
