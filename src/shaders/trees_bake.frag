#version 450
#extension GL_EXT_nonuniform_qualifier : require
// bakes a tree mesh into the impostor atlas: colour + coverage, and the normal (in the bake's view space) + AO

layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) flat in int inMaterial;
layout(location = 4) in float inAO;
// unused here, declared so the interface matches trees.vert
layout(location = 0) in vec3 inPosition;
layout(location = 5) flat in float inDryness;
layout(location = 6) flat in float inFade;
layout(location = 7) flat in float inSeed;
layout(location = 9) flat in float inTint; // the tree type's colouring

layout(location = 0) out vec4 outAlbedo;
layout(location = 1) out vec4 outNormal;

layout(set = 0, binding = 1) uniform sampler2D treeTextures[10];

#include "treecommon.glsl" // pc.sundir = direction towards the bake's viewer

void main() {
	vec4 texel = texture(treeTextures[nonuniformEXT(inMaterial)], inUV);
	bool leaves = inMaterial == 2 || inMaterial == 3;
	if (leaves && texel.a < 0.5) discard;
	vec3 normal = normalize(inNormal);
	if (!leaves && !gl_FrontFacing) normal = -normal;
	outAlbedo = vec4(pow(treeTint(pow(texel.rgb, vec3(2.2)), inTint, leaves, inUV), vec3(1.0 / 2.2)), 1.0); // the same colours as the meshes
	// the impostor card faces the camera, so store the normal relative to this view (x right, y up, z to the viewer)
	vec3 toViewer = pc.sundir;
	vec3 right = normalize(cross(vec3(0.0, 1.0, 0.0), toViewer));
	vec3 up = cross(toViewer, right);
	vec3 local = vec3(dot(normal, right), dot(normal, up), dot(normal, toViewer));
	outNormal = vec4(local * 0.5 + 0.5, inAO);
}
