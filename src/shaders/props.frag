#version 450
#extension GL_EXT_nonuniform_qualifier : require // the mesh (its textures) changes per draw
// rocks, logs and stumps: the photoscans with their own colour and normal maps (Props::loadMesh), moss grown on the
// tops where it's wet and shaded. deferred, lit in lighting.comp

#define GBUFFER_WRITE
#include "gbuffer.glsl"
#include "propscommon.glsl"
#include "propmeshes.glsl" // PROP_FIRST_LOG
#include "normalmap.glsl"
#include "noise.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) flat in int inMaterial; // the mesh
layout(location = 4) in float inAO;
layout(location = 5) flat in float inMoss;
layout(location = 6) flat in float inSeed;
layout(location = 7) flat in vec2 inDither;
layout(location = 8) flat in vec2 inBase;

// per mesh: albedo, normal (Poly Haven, assets/props), the moss (the terrain's grass texture), then the ambientCG
// stones Rock030 and Rock062 (albedo, normal each, assets/props/stone) for propStone()
layout(set = 0, binding = 1) uniform sampler2D textures[PROP_MESHES * 2 + 5];
#define STONE_TEXTURES (PROP_MESHES * 2 + 1)

void main() {
	// the LOD hand over (propscommon.glsl): interleaved gradient noise, moved every frame so the TAA blends the two
	if (inDither.x > 0.0 || inDither.y < 1.0) {
		float frame = mod(floor(pc.time * 60.0), 64.0);
		vec2 p = gl_FragCoord.xy + 5.588238 * frame;
		float d = interleavedGradientNoise(p);
		if (d < inDither.x || d >= inDither.y) discard;
	}
	vec3 normal = normalize(inNormal);
	// (no gl_FrontFacing flip: the projection flips y, the side facing the camera counts as the back. the scans are
	// closed where they're seen, only open at the bottom that's in the ground)
	vec3 dp1 = dFdx(inPosition), dp2 = dFdy(inPosition);
	vec2 duv1 = dFdx(inUV), duv2 = dFdy(inUV);
	uint mesh = uint(inMaterial);
	bool wood = mesh >= PROP_FIRST_LOG;

	vec3 albedo;
	vec2 n;
	if (propStone(mesh)) {
		// a tiling stone projected from three sides (blended by the normal, sharpened so the seams are narrow,
		// whiteout normal blend), ~3m repeat, a different spot of it and one of the two stones per rock
		int stone = STONE_TEXTURES + (fract(inSeed * 7.31) < 0.5 ? 0 : 2);
		vec3 w = pow(abs(normal), vec3(4.0));
		w /= w.x + w.y + w.z;
		vec3 p = inPosition / 0.5 + inSeed * 37.0;
		vec2 uvX = p.zy, uvY = p.xz, uvZ = p.xy;
		albedo = texture(textures[nonuniformEXT(stone)], uvX).rgb * w.x + texture(textures[nonuniformEXT(stone)], uvY).rgb * w.y
			   + texture(textures[nonuniformEXT(stone)], uvZ).rgb * w.z;
		albedo = pow(albedo, vec3(2.2));
		vec3 nX = unpackNormal(texture(textures[nonuniformEXT(stone + 1)], uvX).rg);
		vec3 nY = unpackNormal(texture(textures[nonuniformEXT(stone + 1)], uvY).rg);
		vec3 nZ = unpackNormal(texture(textures[nonuniformEXT(stone + 1)], uvZ).rg);
		vec3 tX = vec3(nX.xy + normal.zy, abs(nX.z) * normal.x);
		vec3 tY = vec3(nY.xy + normal.xz, abs(nY.z) * normal.y);
		vec3 tZ = vec3(nZ.xy + normal.xy, abs(nZ.z) * normal.z);
		normal = normalize(tX.zyx * w.x + tY.xzy * w.y + tZ.xyz * w.z);
		n = nY.xy; // (the moss' patchiness below)
	} else {
		albedo = pow(texture(textures[nonuniformEXT(mesh * 2u)], inUV).rgb, vec3(2.2));
		n = texture(textures[nonuniformEXT(mesh * 2u + 1u)], inUV).rg * 2.0 - 1.0;
		normal = perturbNormal(normal, dp1, dp2, duv1, duv2, vec3(n, sqrt(max(1.0 - dot(n, n), 0.0))));
	}
	// a bit of variation per prop: no two rocks of a scan quite the same colour
	albedo *= mix(0.85, 1.1, fract(inSeed * 13.7));
	if (wood) albedo *= 0.9; // lying on the forest floor, weathered
	else {
		// stone reads grey from afar: the scans' warm brown looked like clods of earth on the meadows (terrainmaterial.glsl
		// greys the terrain's rock the same way)
		float luma = dot(albedo, vec3(0.3, 0.59, 0.11));
		albedo = mix(albedo, luma * vec3(0.96, 0.97, 1.02), 0.45);
	}

	// moss on the tops in wet, shaded places, patchy by the normal map's relief
	vec2 mossUV = inPosition.xz * 2.5;
	// (negative moss is snow, over 1 sand in the desert, propplacement.glsl)
	float sand = clamp(inMoss - 1.0, 0.0, 1.0);
	float wet = inMoss > 1.0 ? 0.0 : max(inMoss, 0.0), snowAmount = max(-inMoss, 0.0);
	float mossCover = smoothstep(wood ? 0.45 : 0.35, 0.85, normal.y + (n.x + n.y) * 0.25) * wet * (wood ? 0.8 : 1.0);
	vec3 moss = pow(texture(textures[PROP_MESHES * 2], mossUV).rgb, vec3(2.2)) * vec3(0.8, 0.95, 0.6);
	albedo = mix(albedo, moss, mossCover);

	float roughness = wood ? 0.85 : 0.95;
	// the desert: the stone sun baked to a warm sandstone, sand drifted up its foot and lying in the hollows on top
	if (sand > 0.0) {
		float luma = dot(albedo, vec3(0.3, 0.59, 0.11));
		albedo = mix(albedo, luma * vec3(1.35, 1.05, 0.72) * 1.15, sand * 0.7);
		vec3 sandColour = vec3(0.6, 0.47, 0.3) * mix(0.9, 1.08, fract(inSeed * 5.3));
		float relief = (n.x + n.y) * 0.3;
		float drift = 1.0 - smoothstep(0.0, 0.18 * inBase.y, inPosition.y - inBase.x + relief * 0.05 * inBase.y);
		float lying = smoothstep(0.6, 0.85, normal.y + relief);
		float sandCover = max(drift, lying * 0.85) * sand;
		albedo = mix(albedo, sandColour, sandCover);
		roughness = mix(roughness, 1.0, sandCover);
	}
	// snow settled on what faces up, a hard edge broken up by the surface's relief (a soft fade read as grey stone).
	// more snow lowers the threshold: a dusting in the hollows on top first, the whole top under deep snow
	if (snowAmount > 0.0) {
		float field = normal.y + (n.x + n.y) * 0.35;
		float threshold = mix(0.95, 0.25, snowAmount);
		float edge = fwidth(field) + 0.03;
		float snowCover = smoothstep(threshold - edge, threshold + edge, field) * 0.95;
		float luma = dot(albedo, vec3(0.3, 0.59, 0.11));
		vec3 snowColour = vec3(0.8, 0.83, 0.88) * mix(0.75, 1.0, smoothstep(0.0, 0.15, luma)); // the rock's detail shows a little
		albedo = mix(albedo, snowColour, snowCover);
		roughness = mix(roughness, 0.82, snowCover); // matte, the sparkle is lighting.comp's
	}
	writeGBuffer(albedo, inAO, normal, wood ? MAT_BARK : MAT_TERRAIN, roughness, mix(0.7, 1.0, inAO));
}
