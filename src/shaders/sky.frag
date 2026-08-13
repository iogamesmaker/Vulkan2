#version 450

layout(location = 0) out vec4 outColor;
layout(location = 0) in vec2 texcoord;

vec3 rayDir(vec2 uv, mat4 invViewProj) {
    vec4 ndc = vec4(uv * 2.0 - 1.0, 1.0, 1.0);
    vec4 dir = invViewProj * ndc;
    return normalize(dir.xyz / dir.w);
}

void main() {
    vec3 color = vec3(0.0, 0.0, 0.4);
    
    outColor = vec4(color, 1.0f);
}