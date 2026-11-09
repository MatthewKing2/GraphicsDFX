#version 450
layout(set=0, binding=0, std140) uniform FrameTransforms { mat4 view; mat4 proj; } camera;
struct Instance { mat4 transform; vec4 tint; };
layout(set=0, binding=2, std430) readonly buffer Instances { Instance objects[]; } scene;
layout(location=0) in vec3 inPosition;
layout(location=1) in vec3 inColor;
layout(location=2) in vec2 inTexCoord;
layout(location=0) out vec3 fragColor;
layout(location=1) out vec2 fragTexCoord;
void main() {
    Instance object = scene.objects[gl_InstanceIndex];
    vec3 localPosition = inPosition;
    // Collapse the upper two corners into a sharp triangular-prism apex.
    if (object.tint.w == 2.0) localPosition.x *= 0.5 - localPosition.z;
    vec4 position = object.transform * vec4(localPosition, 1);
    if (object.tint.w < 0) {
        // Later HUD rectangles are slightly closer so text draws over panels.
        gl_Position = vec4(position.xy, 0.001 - float(gl_InstanceIndex)*0.00000002, 1);
        fragColor = object.tint.rgb;
    } else {
        gl_Position = camera.proj * camera.view * position;
        float distance = max(0.0, -(camera.view * position).z);
        float fog = 1.0 / (1.0 + distance * distance / 30000.0);
        fragColor = object.tint.rgb * (object.tint.w == 3.0 ? vec3(1) : inColor) *
            (object.tint.w == 3.0 ? 1.0 : fog);
    }
    fragTexCoord = inTexCoord;
}
