#version 450
// depth prepass for the far tree impostors (the trees' own prepass does the same for the meshes): the fade dither and
// the alpha test once per pixel, before the terrain's material pass. impostor.frag then shades only the card that's
// visible (depth test EQUAL), not every card of a distant forest behind it, and the terrain under the forests isn't
// shaded at all

#include "treecommon.glsl" // treeFaded

layout(location = 1) in vec2 inUV;
layout(location = 5) flat in float inFade;
layout(location = 8) in vec2 inUV2;
layout(location = 9) flat in float inBlend;
// unused here, declared so the interface matches impostor.vert
layout(location = 0) in vec3 inPosition;
layout(location = 2) in vec3 inRight;
layout(location = 3) in vec3 inToCamera;
layout(location = 4) flat in float inDryness;
layout(location = 6) flat in float inSeed;
layout(location = 7) in vec3 inUp;

layout(set = 0, binding = 1) uniform sampler2D impostorAlbedo;

void main() {
	if (treeFaded(inFade, gl_FragCoord.xy, true)) discard; // inverted dither, see treeFaded
	// (impostor.frag's sharpened alpha, (a - 0.5) / fwidth(a) + 0.5 < 0.5, is the same test as a < 0.5)
	if (mix(texture(impostorAlbedo, inUV).a, texture(impostorAlbedo, inUV2).a, inBlend) < 0.5) discard;
}
