#version 450

layout(set = 0, binding = 0, std140) uniform FrameTransforms {
    mat4 view;
    mat4 proj;
} ubo;

layout(set = 0, binding = 2, std430) readonly buffer ModelMatrices {
    mat4 models[];
} instances;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inColor;
layout(location = 2) in vec2 inTexCoord;

layout(location = 0) out vec3 fragColor;
layout(location = 1) out vec2 fragTexCoord;

void main() {
    mat4 model = instances.models[gl_InstanceIndex];

    gl_Position = ubo.proj * ubo.view * model * vec4(inPosition, 1.0);
    fragColor = inColor;
    fragTexCoord = inTexCoord;
}