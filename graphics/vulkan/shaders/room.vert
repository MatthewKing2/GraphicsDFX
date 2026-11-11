#version 450
layout(set=0, binding=0, std140) uniform FrameTransforms {
    mat4 view;
    mat4 proj;
    vec4 atmosphere;
} camera;
struct Instance { mat4 transform; vec4 tint; };
layout(set=0, binding=2, std430) readonly buffer Instances { Instance objects[]; } scene;
layout(location=0) in vec3 inPosition;
layout(location=1) in vec3 inColor;
layout(location=2) in vec2 inTexCoord;
layout(location=0) out vec3 color;
layout(location=1) out vec3 worldPosition;
layout(location=2) out vec3 viewPosition;
layout(location=3) flat out float material;
layout(location=4) flat out vec4 atmosphere;
void main() {
    Instance object = scene.objects[gl_InstanceIndex];
    vec3 local = inPosition;
    // 5: solid triangular prism, 6: luminous triangular artery surface.
    // The shared cube's top edge collapses to an apex; no new mesh per facet.
    if (object.tint.w == 5.0 || object.tint.w == 6.0)
        local.x *= 0.5 - local.z;
    vec4 world = object.transform * vec4(local, 1);
    worldPosition = world.xyz;
    viewPosition = (camera.view * world).xyz;
    material = object.tint.w;
    atmosphere = camera.atmosphere;
    color = object.tint.rgb;
    // Panel objects already contain their letterboxed clip transform, including
    // explicit depth layers. Their readability is independent of world fog.
    gl_Position = material < 0.0 ? vec4(world.xyz, 1) : camera.proj * camera.view * world;
}
