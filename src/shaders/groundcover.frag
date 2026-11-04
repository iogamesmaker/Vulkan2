#version 450
// the ground cover plants: alpha tested leaves (two sided), normal mapped, lit like the trees' leaves in
// lighting.comp; the dead branches like bark

#define GBUFFER_WRITE
#include "gbuffer.glsl"
#include "groundcovercommon.glsl"
#include "groundcoveralpha.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inPrevPosition;
layout(location = 2) in vec3 inNormal;
layout(location = 3) in vec2 inUV;
layout(location = 4) in float inHeight;
layout(location = 5) flat in int inSpecies;
layout(location = 6) flat in float inTint;

// normal mapping without stored tangents (Schueler 2013, props.frag)
vec3 perturbNormal(vec3 N, vec3 dp1, vec3 dp2, vec2 duv1, vec2 duv2, vec3 mapNormal) {
	vec3 dp2perp = cross(dp2, N);
	vec3 dp1perp = cross(N, dp1);
	vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
	vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
	float invmax = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-12));
	return normalize(mat3(T * invmax, B * invmax, N) * mapNormal);
}

void main() {
	vec4 texel;
	if (!groundCoverCovered(inSpecies, inUV, texel)) discard;
	vec3 albedo = pow(texel.rgb, vec3(2.2));
	// a little variation per plant: drier / yellower ones where it's dry, darker greener ones in the damp
	albedo *= mix(vec3(1.08, 1.04, 0.85), vec3(0.85, 0.95, 0.9), inTint);

	vec3 normal = normalize(inNormal);
	if (!gl_FrontFacing) normal = -normal; // the leaves are two sided
	vec2 n = texture(textures[inSpecies * 2 + 1], inUV).rg * 2.0 - 1.0;
	normal = perturbNormal(normal, dFdx(inPosition), dFdy(inPosition), dFdx(inUV), dFdy(inUV), vec3(n, sqrt(max(1.0 - dot(n, n), 0.0))));

	// darker down inside the plant, where the leaves shade each other and the ground is close
	float ao = mix(0.45, 1.0, smoothstep(0.0, 0.8, inHeight));
	if (inSpecies == GC_BRANCHES) writeGBuffer(albedo, ao, normal, MAT_BARK, 0.85, mix(0.7, 1.0, ao));
	else writeGBuffer(albedo, ao, normal, MAT_LEAF, 1.0, mix(0.6, 1.0, ao));
	writeVelocity(pc.viewproj, inPosition, inPrevPosition);
}
