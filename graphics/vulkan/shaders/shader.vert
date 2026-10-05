#version 450

// Shared by every draw in this frame.
layout(binding = 0, std140) uniform FrameTransforms {
    mat4 view;
    mat4 proj;
} ubo;

// The CPU records a new model matrix before each mesh's draw.
layout(push_constant, std430) uniform ModelTransform {
    mat4 model;
} objectTransform;

// Inputs: 
layout(location = 0) in vec3 inPosition;        // x, y, z position of verticie 
layout(location = 1) in vec3 inColor;           // r, g, b color of that verticie
layout(location = 2) in vec2 inTexCoord;        // u, v, (aka x,y) of this verex on the texture map

// Outputs: 
layout(location = 0) out vec3 fragColor;        // Output to .frag (fragment shader)
layout(location = 1) out vec2 fragTexCoord;     // output to .frag (fragment shader)

void main() {
    gl_Position = ubo.proj * ubo.view * objectTransform.model * vec4(inPosition, 1.0); // gl_Position is the built-in clip-space output; w=1 lets the model matrix translate a position.
    fragColor = inColor;        // pass color to fragment shader
    fragTexCoord = inTexCoord;  // pass texture coordinate to fragment shader 
}
