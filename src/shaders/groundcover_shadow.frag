#version 450
#extension GL_EXT_nonuniform_qualifier : require
// shadow pass for the ground cover: depth only, the same cutout as groundcover.frag

#include "groundcovercommon.glsl"
#include "groundcoveralpha.glsl"

layout(location = 3) in vec2 inUV;
layout(location = 7) flat in int inTexture;

void main() {
	vec4 texel;
	if (!groundCoverCovered(inTexture, inUV, texel)) discard;
}
