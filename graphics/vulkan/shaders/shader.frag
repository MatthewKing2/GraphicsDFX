#version 450

layout(binding = 1) uniform sampler2D texSampler;

// Inputs: 
layout(location = 0) in vec3 fragColor;         // r, g, b
layout(location = 1) in vec2 fragTexCoord;      // u, v (aka x, y)

// Outputs: 
layout(location = 0) out vec4 outColor;         // r, g, b, alpha

void main() {
    // outColor = texture(texSampler, fragTexCoord);
    outColor = vec4(fragColor, 1.0);            // copy fragColor in to outColor, alpha = 1
}