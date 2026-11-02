#version 450
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;
layout(set = 0, binding = 2, std430) readonly buffer PixelBuffer {
    uint pixels[];
};

void main() {
    uvec2 bin = min(uvec2(clamp(uv, 0.0, 1.0) * float(IMAGE_SIZE)),
                    uvec2(IMAGE_SIZE - 1u));
    uint value = pixels[bin.y * IMAGE_SIZE + bin.x];
    vec3 background = vec3(0.018, 0.027, 0.055);
    vec3 result = background;
    if (params.workload == 0u) {
        // Quarter-circle boundary remains clear even for a small display sample.
        float edge = 1.0 - smoothstep(0.0015, 0.005, abs(length(uv) - 1.0));
        result = mix(background, vec3(0.45, 0.58, 0.73), edge);
        if ((value & 1u) != 0u) result = vec3(0.13, 0.90, 0.72);
        if ((value & 2u) != 0u) result = vec3(1.0, 0.32, 0.25);
    } else {
        // Logarithmic mapping reveals both the core and tail of the diffusion
        // cloud without an extra min/max reduction of the full image.
        float brightness = clamp(log2(1.0 + float(value)) / 6.0, 0.0, 1.0);
        vec3 cold = vec3(0.08, 0.40, 0.85);
        vec3 hot = vec3(1.0, 0.83, 0.26);
        result = mix(background, mix(cold, hot, brightness), min(brightness * 3.0, 1.0));
    }
    color = vec4(result, 1.0);
}
