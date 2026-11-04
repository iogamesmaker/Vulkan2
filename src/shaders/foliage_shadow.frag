#version 450
// shadow pass for grass / wheat / flowers: only the alpha test matters

layout(location = 1) in vec2 inUV;
layout(location = 4) flat in int inKind;
// unused here, declared so the interface matches foliage.vert
layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in float inHeight;
layout(location = 5) flat in float inSeed;
layout(location = 6) flat in float inMoisture;

layout(set = 0, binding = 1) uniform sampler2D bladeAtlas;
layout(set = 0, binding = 2) uniform sampler2D flowerAtlas;
layout(set = 0, binding = 3) uniform sampler2D wheatAtlas;
layout(set = 0, binding = 4) uniform sampler2D undergrowthAtlas;

#include "foliagealpha.glsl"

void main() {
	vec4 texel;
	if (!foliageCovered(inKind, inUV, texel)) discard;
}
