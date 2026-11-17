#version 450
#extension GL_EXT_nonuniform_qualifier : require
// the ground cover plants: alpha tested leaves (two sided), normal mapped, lit like the trees' leaves in
// lighting.comp; the dead branches like bark

#define GBUFFER_WRITE
#include "gbuffer.glsl"
#include "groundcovercommon.glsl"
#include "groundcoveralpha.glsl"
#include "normalmap.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inPrevPosition;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inUV;
layout(location = 4) in float inHeight;
layout(location = 5) flat in int inSpecies;
layout(location = 6) flat in float inTint;
layout(location = 7) flat in int inTexture;
layout(location = 8) in vec3 inCrown;

void main() {
	vec4 texel;
	if (!groundCoverCovered(inTexture, inUV, texel)) discard;
	vec3 albedo = pow(texel.rgb, vec3(2.2));
	// the look (groundcovercommon.glsl: some species are another's model coloured differently) and the tint
	bool snowy = inTint >= 16.0; // (groundcover.comp: + 16 in the snow climate)
	float packedTint = snowy ? inTint - 16.0 : inTint;
	int look = int(packedTint * 0.5);
	float tint = packedTint - 2.0 * float(look);
	// a little variation per plant: drier / yellower ones where it's dry, darker greener ones in the damp
	albedo *= mix(vec3(1.08, 1.04, 0.85), vec3(0.85, 0.95, 0.9), tint);
	float luma = dot(albedo, vec3(0.3, 0.59, 0.11));
	float plant = fract(tint * 7.31); // per plant (the tint has a random part of its own)
	if (look == GC_LOOK_BRACKEN) {
		// a fresh yellow green, some plants turning rusty
		vec3 fresh = albedo * vec3(1.25, 1.12, 0.5);
		vec3 rust = luma * vec3(2.3, 1.25, 0.45);
		albedo = mix(fresh, rust, smoothstep(0.5, 0.9, plant) * 0.7);
	} else if (look == GC_LOOK_AUTUMN) {
		albedo = luma * mix(vec3(2.6, 0.55, 0.3), vec3(2.3, 1.15, 0.25), plant); // red to orange
	} else if (look == GC_LOOK_STRAW) {
		albedo = luma * mix(vec3(1.9, 1.55, 0.85), vec3(1.6, 1.45, 0.8), plant) * 1.15; // pale straw, some a bit greyer
	} else if (look == GC_LOOK_SEDGE) {
		albedo *= vec3(0.7, 0.85, 0.85); // dark blue green
	}

	vec3 dp1 = dFdx(inPosition), dp2 = dFdy(inPosition);
	vec3 normal = normalTowardsCamera(normalize(inNormal), dp1, dp2, pc.campos - inPosition); // the leaves are two sided
	vec2 n = texture(textures[nonuniformEXT(inTexture * 2 + 1)], inUV).rg * 2.0 - 1.0;
	normal = perturbNormal(normal, dp1, dp2, dFdx(inUV), dFdy(inUV), vec3(n, sqrt(max(1.0 - dot(n, n), 0.0))));

	// the snowy forest: dead, frost bitten colours, and snow lying on whatever faces up (a little more on the upper parts)
	if (snowy) {
		float l = dot(albedo, vec3(0.3, 0.59, 0.11));
		if (inSpecies == GC_FERN || look == GC_LOOK_BRACKEN) albedo = l * vec3(2.0, 1.15, 0.5); // dead, rusty
		else albedo = mix(albedo, l * vec3(1.1, 1.0, 0.85), 0.45); // greyed, frost bitten
		float lying = smoothstep(0.3, 0.75, normal.y) * mix(0.6, 1.0, smoothstep(0.2, 0.9, inHeight));
		albedo = mix(albedo, vec3(0.8, 0.83, 0.88), lying * 0.85);
	}
	// darker down inside the plant, where the leaves shade each other and the ground is close
	float ao = mix(0.45, 1.0, smoothstep(0.0, 0.8, inHeight));
	// the leafy bushes (thousands of small leaf cards): lit as one rounded crown, like the trees' leaves. each card's
	// own normal, turned to the camera, mostly faced away from the sun: the bushes came out nearly black. the normal
	// leans out from the crown's centre, and the self shading is gentler (the screen space AO already darkens inside)
	bool leafyBush = inSpecies == GC_SHRUB || inSpecies == GC_SHRUB_GREEN || inSpecies == GC_SHRUB_FLOWER || inSpecies == GC_LILAC_TREE
				  || inSpecies == GC_LILAC_SAPLING || inSpecies == GC_BUSH_SMALL || inSpecies == GC_BUSH || inSpecies == GC_ROOIBOS;
	if (leafyBush) {
		vec3 crown = dot(inCrown, inCrown) > 1e-10 ? normalize(inCrown) : vec3(0.0, 1.0, 0.0);
		normal = normalize(mix(normal, crown, 0.65));
		ao = mix(0.7, 1.0, smoothstep(0.0, 0.7, inHeight));
	}
	if (inSpecies == GC_BRANCHES || inSpecies == GC_ROOTS || inSpecies == GC_QUIVER_BRANCH || inSpecies == GC_MUSHROOMS
		|| inSpecies == GC_QUIVER_BRANCH2 || inSpecies == GC_ROOT_CLUSTER2) writeGBuffer(albedo, ao, normal, MAT_BARK, 0.85, mix(0.7, 1.0, ao));
	else writeGBuffer(albedo, ao, normal, MAT_LEAF, 1.0, mix(leafyBush ? 0.85 : 0.6, 1.0, ao));
	writeVelocity(pc.viewproj, inPosition, inPrevPosition);
}
