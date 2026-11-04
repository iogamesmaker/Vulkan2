#version 450
// shadow pass for the ground cover: depth only, the same cutout as groundcover.frag

#include "groundcovercommon.glsl"
#include "groundcoveralpha.glsl"

layout(location = 3) in vec2 inUV;
layout(location = 5) flat in int inSpecies;

void main() {
	vec4 texel;
	if (!groundCoverCovered(inSpecies, inUV, texel)) discard;
}
