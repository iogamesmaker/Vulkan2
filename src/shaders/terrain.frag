#version 450

layout(location = 0) out vec4 outColor;

layout(location = 0) in vec3 fragPos;
layout(location = 1) in vec2 fragUV;
layout(location = 5) in flat int LOD;

layout(set = 0, binding = 0) uniform sampler2D heightmap;

layout(push_constant) uniform constants {
	mat4 proj;
	mat4 view;
	ivec2 offset;
	ivec2 screen;
	float tessellationFactor;
	float factor;
} pc;

void main() {
	if(fragUV.x > 0.99 || fragUV.y > 0.99 || fragUV.x < 0.01 || fragUV.y < 0.01) discard;

    vec2 size = vec2(textureSize(heightmap, 0));
    vec2 readCoord = fragUV + (pc.offset * (1.0 / size));

    float hR = textureOffset(heightmap, readCoord, ivec2(1, 0)).r;
    float hL = textureOffset(heightmap, readCoord, ivec2(-1, 0)).r;
    float hT = textureOffset(heightmap, readCoord, ivec2(0, 1)).r;
    float hB = textureOffset(heightmap, readCoord, ivec2(0, -1)).r;

    vec3 va = normalize(vec3(1.0, 0.0, (hR - hL) * 256.0f));
    vec3 vb = normalize(vec3(0.0, 1.0, (hT - hB) * 256.0f));
    vec3 normal = normalize(cross(va, vb));
	
    outColor = vec4(texture(heightmap, readCoord).xyz, 1.0);
}