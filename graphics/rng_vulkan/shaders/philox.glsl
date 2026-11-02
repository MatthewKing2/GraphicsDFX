// Philox4x32-10; Salmon et al., SC11, doi:10.1145/2063384.2063405.
// All arithmetic is 32-bit unsigned. umulExtended supplies both product halves
// without requiring Vulkan's optional shaderInt64 feature.
uvec4 philox(uvec4 counter, uvec2 key) {
    for (uint round = 0u; round < 10u; ++round) {
        uint highA, lowA, highB, lowB;
        umulExtended(0xd2511f53u, counter.x, highA, lowA);
        umulExtended(0xcd9e8d57u, counter.z, highB, lowB);
        counter = uvec4(highB ^ counter.y ^ key.x, lowB,
                        highA ^ counter.w ^ key.y, lowA);
        key += uvec2(0x9e3779b9u, 0xbb67ae85u);
    }
    return counter;
}

vec4 uniform4(uvec4 value) {
    return vec4(value >> 8u) * (1.0 / 16777216.0);
}
