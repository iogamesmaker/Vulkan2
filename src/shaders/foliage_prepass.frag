#version 450
// depth prepass for grass / wheat / flowers: only the alpha test, so foliage.frag (depth test EQUAL) shades each
// pixel once instead of every blade behind it. wheat fields are wall to wall cards, that's a lot of overdraw

layout(location = 1) in vec2 inUV;
layout(location = 3) in float inHeight;
layout(location = 4) flat in int inKind;
// unused here, declared so the interface matches foliage.vert
layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec3 inNormal;
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
