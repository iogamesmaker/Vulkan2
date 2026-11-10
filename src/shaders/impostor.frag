#version 450
// far tree impostors, lit with the baked normals like the real trees (sun + moon, sky, shadows, aerial perspective)

#define GBUFFER_WRITE
#include "gbuffer.glsl"
#include "treecommon.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec3 inRight;
layout(location = 3) in vec3 inToCamera;
layout(location = 4) flat in float inDryness;
layout(location = 5) flat in float inFade;
layout(location = 6) flat in float inSeed;
layout(location = 7) in vec3 inUp;
layout(location = 8) in vec2 inUV2;
layout(location = 9) flat in float inBlend;


layout(set = 0, binding = 1) uniform sampler2D impostorAlbedo;
layout(set = 0, binding = 2) uniform sampler2D impostorNormal;

layout(set = 1, binding = 0) uniform sampler2D transmittanceLUT;
layout(set = 1, binding = 2) uniform sampler2D skyviewLUT;
layout(set = 1, binding = 3) uniform sampler3D aerialLUT;

#define ATMOSPHERE_SET 1
#define ATMOSPHERE_BINDING 4
#include "atmosphere.glsl"


void main() {
	// the fade dither and the alpha test are in impostor_prepass.frag: only the visible card gets here (depth EQUAL)
	vec4 albedoTexel = mix(texture(impostorAlbedo, inUV), texture(impostorAlbedo, inUV2), inBlend);

	vec4 normalTexel = mix(texture(impostorNormal, inUV), texture(impostorNormal, inUV2), inBlend);
	vec3 baked = normalTexel.xyz * 2.0 - 1.0; // x right, y up, z towards the viewer of the bake
	vec3 normal = normalize(inRight * baked.x + inUp * baked.y + inToCamera * baked.z);
	float ao = normalTexel.w;

	vec3 albedo = pow(albedoTexel.rgb, vec3(2.2));
	albedo *= mix(vec3(0.85, 0.95, 0.85), vec3(1.1, 1.05, 0.9), inSeed);
	float luma = dot(albedo, vec3(0.3, 0.59, 0.11));
	albedo = mix(albedo, luma * vec3(1.6, 1.25, 0.7), clamp(inDryness, 0.0, 1.0) * 0.7);

	albedo = applySnow(albedo, snowCover(max(-inDryness, 0.0), normal, inPosition)); // clumps on the baked normals, like trees.frag

	writeGBuffer(albedo, ao * mix(0.6, 1.0, ao), normal, MAT_IMPOSTOR, 1.0, mix(0.6, 1.0, ao)); // lit in lighting.comp
}
