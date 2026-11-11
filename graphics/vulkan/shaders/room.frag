#version 450
layout(location=0) in vec3 color;
layout(location=1) in vec3 worldPosition;
layout(location=2) in vec3 viewPosition;
layout(location=3) flat in float material;
layout(location=4) flat in vec4 atmosphere;
layout(location=0) out vec4 outColor;
float hash(vec3 p) {
    return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453);
}
void main() {
    if (material < 0.0) { outColor = vec4(color, 1); return; }
    float time = atmosphere.x;
    float distance = length(viewPosition);
    vec3 normal = normalize(cross(dFdx(worldPosition), dFdy(worldPosition)));
    vec3 shaded;
    if (material == 6.0) {
        // Travelling heartbeat, not unrelated random twinkling per triangle.
        float phase = time * 2.6 - worldPosition.y * 1.9 + worldPosition.z * 1.5 + worldPosition.x;
        float pulse = pow(0.5 + 0.5 * sin(phase), 5.0);
        shaded = color * (0.13 + pulse * 1.65);
    } else if (material == 3.0) {
        shaded = color; // diegetic monitor/etching
    } else {
        vec3 light = vec3(.13, .14, .19) * atmosphere.w;
        vec3 lamps[4] = vec3[4](vec3(-3.2,2.0,-1.5), vec3(3.4,2.8,1.4),
                               vec3(0,2.1,-3.5), vec3(0,3.5,3.0));
        vec3 tints[4] = vec3[4](vec3(.75,.11,1.1), vec3(.43,.08,.8),
                               vec3(.29,.39,.51), vec3(.12,.14,.24));
        for (int i=0; i<4; ++i) {
            vec3 d = lamps[i] - worldPosition;
            float diffuse = .16 + .84 * abs(dot(normal, normalize(d)));
            light += tints[i] * diffuse * (1.7 / (1.0 + dot(d,d)));
        }
        // World-anchored corrosion and coarse mineral grain; no bitmap textures.
        float organic = sin(dot(worldPosition, vec3(3.1,5.7,2.3)) +
                            2.0 * sin(dot(worldPosition, vec3(7.1,.3,4.7))));
        float dirt = .78 + .18 * organic;
        dirt *= mix(.86, 1.0, hash(floor(worldPosition * 140.0)));
        float streak = .86 + .14 * sin(worldPosition.y * .7 +
                                      sin(worldPosition.x * 19.0 + worldPosition.z * 8.0));
        shaded = color * light * dirt * streak;
    }
    // Depth/height haze with slowly drifting density. This is inexpensive
    // analytic fog, not an expensive volumetric fluid simulation.
    float lowMist = exp(-max(worldPosition.y, 0.0) * .85);
    float drift = .84 + .16 * sin(worldPosition.x * .6 + worldPosition.z * .8 - time * .18);
    float transmission = exp(-distance * atmosphere.y * (1.0 + lowMist * 1.3) * drift);
    vec3 fogColor = vec3(.012, .009, .022) + lowMist * vec3(.006, .005, .009);
    shaded = mix(fogColor, shaded, transmission);
    float grain = hash(vec3(floor(gl_FragCoord.xy), floor(time * 12.0))) - .5;
    shaded *= 1.0 + grain * atmosphere.z;
    outColor = vec4(max(shaded, vec3(0)), 1);
}
