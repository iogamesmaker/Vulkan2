#version 450
#extension GL_EXT_nonuniform_qualifier : require // the material changes per triangle
// bark + alpha tested leaf clusters of the imported trees, lit like the terrain plus light through the leaves, aerial perspective since
// trees are drawn out to a couple of km. outputs HDR

#define GBUFFER_WRITE
#include "gbuffer.glsl"
#include "treecommon.glsl"

layout(early_fragment_tests) in; // depth EQUAL after the prepass, so the discards below never change the depth

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) flat in int inMaterial;
layout(location = 4) in float inAO;
layout(location = 5) flat in float inDryness;
layout(location = 6) flat in float inFade;
layout(location = 7) flat in float inSeed;
layout(location = 8) in vec3 inPrevPosition; // with last frame's wind
layout(location = 9) flat in float inTint;	   // unused


// per pack (treemeshes.glsl): bark colour, bark normal, leaf atlas (colour + coverage), leaf normal
layout(set = 0, binding = 1) uniform sampler2D treeTextures[TREE_PACKS * 4];

layout(set = 1, binding = 0) uniform sampler2D transmittanceLUT;
layout(set = 1, binding = 2) uniform sampler2D skyviewLUT;
layout(set = 1, binding = 3) uniform sampler3D aerialLUT;

#define ATMOSPHERE_SET 1
#define ATMOSPHERE_BINDING 4
#include "atmosphere.glsl"


// normal mapping without stored tangents: the tangent frame from screen space derivatives (Schueler 2013)
vec3 perturbNormal(vec3 N, vec3 dp1, vec3 dp2, vec2 duv1, vec2 duv2, vec3 mapNormal) {
	vec3 dp2perp = cross(dp2, N);
	vec3 dp1perp = cross(N, dp1);
	vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
	vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
	float invmax = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-12));
	return normalize(mat3(T * invmax, B * invmax, N) * mapNormal);
}

void main() {
	if (treeFaded(inFade, gl_FragCoord.xy, false)) discard; // dithered fade: edge of the range, LOD crossfade

	// derivatives up here, before anything that isn't uniform across the quad
	vec3 dp1 = dFdx(inPosition), dp2 = dFdy(inPosition);
	vec2 duv1 = dFdx(inUV), duv2 = dFdy(inUV);

	bool leaves = (inMaterial & 1) == 1;
	uint pack = uint(inMaterial >> 1);
	int slot = int(pack) * 4 + (leaves ? 2 : 0);
	vec3 normal = normalize(inNormal);
	// bark seen from inside (two sided): the side facing the camera. the leaf clusters' normals point out of the
	// crown (the packs bend them for a soft, round canopy): the same on both sides of a card
	if (!leaves && !gl_FrontFacing) normal = -normal;

	vec4 texel = textureGrad(treeTextures[nonuniformEXT(slot)], inUV, duv1, duv2);
	if (leaves) {
		float alpha = (texel.a - 0.5) / max(fwidth(texel.a), 1e-4) + 0.5;
		if (alpha < 0.5) discard;
	}

	vec3 albedo = packColour(pow(texel.rgb, vec3(2.2)), pack, leaves);
	if (leaves) {
		albedo *= mix(vec3(0.85, 0.95, 0.85), vec3(1.1, 1.05, 0.9), inSeed);
		// dry places get olive / brown crowns
		float luma = dot(albedo, vec3(0.3, 0.59, 0.11));
		albedo = mix(albedo, luma * vec3(1.6, 1.25, 0.7), clamp(inDryness, 0.0, 1.0) * 0.7);
	}

	// the normal maps (bark: the furrows, leaves: the veins and the cluster's shape), a little softer on the leaves
	vec2 n = textureGrad(treeTextures[nonuniformEXT(slot + 1)], inUV, duv1, duv2).rg * 2.0 - 1.0;
	if (leaves) n *= 0.6;
	normal = perturbNormal(normal, dp1, dp2, duv1, duv2, vec3(n, sqrt(max(1.0 - dot(n, n), 0.0))));
	float roughness = leaves ? 0.75 : 0.9;

	// snow clumps on everything facing up in cold places (antialiased in snowCover, so no far fallback needed)
	float snow = snowCover(max(-inDryness, 0.0), normal, inPosition);
	albedo = applySnow(albedo, snow);
	roughness = mix(roughness, 0.6, snow);
	// lit in lighting.comp. the tree's own AO darkens the direct light a bit too
	float aoDirect = mix(0.6, 1.0, inAO);
	// (leaves: the canopy AO once for the ambient, squared it made the inside of every crown black)
	if (leaves) writeGBuffer(albedo, inAO, normal, MAT_LEAF, roughness, aoDirect);
	else writeGBuffer(albedo, inAO * aoDirect, normal, MAT_BARK, roughness, aoDirect);
	writeVelocity(pc.viewproj, inPosition, inPrevPosition); // leaves and branches sway
}
