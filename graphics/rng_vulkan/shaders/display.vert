#version 450

layout(location = 0) out vec2 uv;

void main() {
    // One full-screen triangle; no CPU vertex upload and no per-particle draw.
    uv = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
