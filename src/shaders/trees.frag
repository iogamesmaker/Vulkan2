#version 450
#extension GL_EXT_nonuniform_qualifier : require // the material changes per triangle
// bark + alpha tested leaf cards, lit like the terrain plus light through the leaves, aerial perspective since
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
layout(location = 9) flat in float inTint;	   // the tree type's colouring (treeTint)


// 0 bark conifer, 1 bark broadleaf, 2 conifer branch card, 3 broadleaf cluster card, 4/5 bark normals, 6/7 bark roughness
layout(set = 0, binding = 1) uniform sampler2D treeTextures[10];

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

// parallax occlusion mapping on the bark: the furrows get real depth up close. the bark has no stored tangents,
// the tangent frame comes from the screen space derivatives (dP/du, dP/dv solved from the 2x2 system)
#define BARK_POM_DISTANCE 3.0 // world units (~18m), faded out over the last half
#define BARK_POM_DEPTH 0.005  // world units (~3cm) between the bottom of a furrow and the top of a ridge
#define BARK_POM_STEPS 16

float barkHeight(int material, vec2 uv, vec2 duv1, vec2 duv2) {
	return textureGrad(treeTextures[nonuniformEXT(material + 8)], uv, duv1, duv2).r;
}

// steps along the view ray below the surface until it's under the height field, returns the shifted uv and how
// deep it hit (0 top, 1 bottom)
vec2 barkParallax(int material, vec2 uv, vec2 duv1, vec2 duv2, vec2 deltaUV, float fade, out float depth) {
	float steps = max(ceil(float(BARK_POM_STEPS) * fade), 4.0);
	float layer = 1.0 / steps;
	vec2 step = deltaUV * layer;
	float rayDepth = 0.0;
	float surfaceDepth = 1.0 - barkHeight(material, uv, duv1, duv2);
	float previousSurface = surfaceDepth;
	for (int i = 0; i < BARK_POM_STEPS; i++) {
		if (float(i) >= steps || rayDepth >= surfaceDepth) break;
		uv -= step;
		rayDepth += layer;
		previousSurface = surfaceDepth;
		surfaceDepth = 1.0 - barkHeight(material, uv, duv1, duv2);
	}
	float after = surfaceDepth - rayDepth;
	float before = previousSurface - (rayDepth - layer);
	float t = clamp(after / min(after - before, -1e-4), 0.0, 1.0);
	depth = rayDepth - layer * t;
	return uv + step * t;
}

void main() {
	if (treeFaded(inFade, gl_FragCoord.xy, false)) discard; // dithered fade: edge of the range, LOD crossfade

	// derivatives up here, the bark branch below isn't uniform across the quad
	vec3 dp1 = dFdx(inPosition), dp2 = dFdy(inPosition);
	vec2 duv1 = dFdx(inUV), duv2 = dFdy(inUV);

	bool leaves = inMaterial == 2 || inMaterial == 3;
	vec3 normal = normalize(inNormal);
	if (!leaves && !gl_FrontFacing) normal = -normal;
	float viewDistance = distance(inPosition, pc.campos);

	// bark parallax: shift the uv along the view ray through the height map (the barks' height maps are 8 slots on)
	vec2 uv = inUV;
	float cavity = 1.0;
	float pomFade = leaves || ubo.pomDistance <= 0.0 ? 0.0 : 1.0 - smoothstep(BARK_POM_DISTANCE * 0.5, BARK_POM_DISTANCE, viewDistance);
	if (pomFade > 0.0) {
		float det = duv1.x * duv2.y - duv2.x * duv1.y;
		if (abs(det) > 1e-12) {
			vec3 dPdu = (dp1 * duv2.y - dp2 * duv1.y) / det; // world units per uv unit along u
			vec3 dPdv = (dp2 * duv1.x - dp1 * duv2.x) / det;
			vec3 toEye = normalize(pc.campos - inPosition);
			float NdotV = max(dot(toEye, normal), 0.2);
			// world offset across the surface for the full depth, then into uv units along each axis
			vec3 across = (toEye - normal * dot(toEye, normal)) / NdotV * BARK_POM_DEPTH * pomFade;
			vec2 deltaUV = vec2(dot(across, dPdu) / max(dot(dPdu, dPdu), 1e-12), dot(across, dPdv) / max(dot(dPdv, dPdv), 1e-12));
			float depth;
			uv = barkParallax(inMaterial, inUV, duv1, duv2, deltaUV, pomFade, depth);
			cavity = mix(1.0, mix(1.0, 0.55, depth), pomFade); // the bottoms of the furrows get less light
		}
	}

	vec4 texel = textureGrad(treeTextures[nonuniformEXT(inMaterial)], uv, duv1, duv2);
	if (leaves) {
		float alpha = (texel.a - 0.5) / max(fwidth(texel.a), 1e-4) + 0.5;
		if (alpha < 0.5) discard;
	}

	vec3 albedo = treeTint(pow(texel.rgb, vec3(2.2)), inTint, leaves, uv);
	if (leaves) {
		albedo *= mix(vec3(0.85, 0.95, 0.85), vec3(1.1, 1.05, 0.9), inSeed);
		// dry places get olive / brown bushes
		float luma = dot(albedo, vec3(0.3, 0.59, 0.11));
		albedo = mix(albedo, luma * vec3(1.6, 1.25, 0.7), clamp(inDryness, 0.0, 1.0) * 0.7);
	}

	// bark: normal map + roughness (the barks' maps are 4 / 6 slots after their colour maps). bark is never
	// glossy, so the roughness map only gets to vary it between quite and very rough
	float roughness = 0.9;
	if (!leaves) {
		vec2 n = textureGrad(treeTextures[nonuniformEXT(inMaterial + 4)], uv, duv1, duv2).rg * 2.0 - 1.0;
		normal = perturbNormal(normal, dp1, dp2, duv1, duv2, vec3(n, sqrt(max(1.0 - dot(n, n), 0.0))));
		roughness = mix(0.7, 1.0, textureGrad(treeTextures[nonuniformEXT(inMaterial + 6)], uv, duv1, duv2).r);
	}

	// snow clumps on everything facing up in cold places (antialiased in snowCover, so no far fallback needed)
	float snow = snowCover(max(-inDryness, 0.0), normal, inPosition);
	albedo = applySnow(albedo, snow);
	roughness = mix(roughness, 0.6, snow);
	// lit in lighting.comp. the tree's own AO darkens the direct light a bit too, the bark's cavities both
	float aoDirect = mix(0.6, 1.0, inAO);
	// (leaves: the canopy AO once for the ambient, squared it made the inside of every crown black)
	if (leaves) writeGBuffer(albedo, inAO, normal, MAT_LEAF, 1.0, aoDirect);
	else writeGBuffer(albedo, inAO * aoDirect * cavity, normal, MAT_BARK, roughness, aoDirect * cavity);
	writeVelocity(pc.viewproj, inPosition, inPrevPosition); // leaves and branches sway
}
