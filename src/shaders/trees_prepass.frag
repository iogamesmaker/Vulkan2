#version 450
#extension GL_EXT_nonuniform_qualifier : require // the material changes per triangle
// depth prepass for the trees: the alpha test (and the fade dither) once per pixel, so the expensive shading in
// trees.frag only runs for the leaf that's actually visible (depth test EQUAL) instead of every card behind it

#include "treecommon.glsl" // treeFaded

layout(location = 2) in vec2 inUV;
layout(location = 3) flat in int inMaterial;
// unused here, declared so the interface matches trees.vert
layout(location = 4) in float inAO;
layout(location = 5) flat in float inDryness;
layout(location = 6) flat in float inFade;
layout(location = 7) flat in float inSeed;

layout(set = 0, binding = 1) uniform sampler2D treeTextures[TREE_PACKS * 4]; // per pack: bark, bark normal, leaves, leaf normal


void main() {
	if (treeFaded(inFade, gl_FragCoord.xy, false)) discard; // the same dithered fade as trees.frag
	if ((inMaterial & 1) == 1 && texture(treeTextures[nonuniformEXT((inMaterial >> 1) * 4 + 2)], inUV).a < 0.5) discard;
}
