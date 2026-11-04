#version 450
#extension GL_EXT_nonuniform_qualifier : require // the material changes per triangle
// shadow pass for trees: only the alpha test of the leaf cards matters

layout(location = 2) in vec2 inUV;
layout(location = 3) flat in int inMaterial;
// unused here, declared so the interface matches trees.vert
layout(location = 4) in float inAO;
layout(location = 5) flat in float inDryness;
layout(location = 6) flat in float inFade;
layout(location = 7) flat in float inSeed;

layout(set = 0, binding = 1) uniform sampler2D treeTextures[10];

void main() {
	if (inMaterial >= 2 && texture(treeTextures[nonuniformEXT(inMaterial)], inUV).a < 0.5) discard;
}
