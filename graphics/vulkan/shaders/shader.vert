#version 450

layout(binding = 0) uniform UniformBufferObject {
    mat4 model;
    mat4 view;
    mat4 proj;
} ubo;

// Inputs: 
layout(location = 0) in vec3 inPosition;        // x, y, z position of verticie 
layout(location = 1) in vec3 inColor;           // r, g, b color of that verticie
layout(location = 2) in vec2 inTexCoord;        // u, v, (aka x,y) of this verex on the texture map

// Outputs: 
layout(location = 0) out vec3 fragColor;        // Output to .frag (fragment shader)
layout(location = 1) out vec2 fragTexCoord;     // output to .frag (fragment shader)

void main() {
    gl_Position = ubo.proj * ubo.view * ubo.model * vec4(inPosition, 1.0);  // gl_Position is a primitive. proj, view, and model matricy us a fourth "w" to allow for translation. {vertex position, w=1} gets transformed on the gpu
    fragColor = inColor;        // pass color to fragment shader
    fragTexCoord = inTexCoord;  // pass texture coordinate to fragment shader 
}