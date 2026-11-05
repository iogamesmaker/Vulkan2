#version 450
// alpha tested grass / wheat cards, lit like the terrain (sun + moon through the atmosphere, sky ambient,
// cascaded shadows) plus light shining through the blades when they're backlit. outputs HDR

#define GBUFFER_WRITE
#include "gbuffer.glsl"
#include "foliagecommon.glsl"

layout(early_fragment_tests) in; // depth EQUAL after the prepass, the discard below never changes the depth

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUV;
layout(location = 2) in vec3 inNormal;
layout(location = 7) in vec3 inPrevPosition; // with last frame's wind
layout(location = 8) flat in vec3 inGroundNormal; // the terrain's normal under the clump
layout(location = 3) in float inHeight;
layout(location = 4) flat in int inKind;
layout(location = 5) flat in float inSeed;
layout(location = 6) flat in float inMoisture;


layout(set = 0, binding = 1) uniform sampler2D bladeAtlas;
layout(set = 0, binding = 2) uniform sampler2D flowerAtlas;
layout(set = 0, binding = 3) uniform sampler2D wheatAtlas;

layout(set = 1, binding = 0) uniform sampler2D transmittanceLUT;
layout(set = 1, binding = 2) uniform sampler2D skyviewLUT;

#define ATMOSPHERE_SET 1
#define ATMOSPHERE_BINDING 4
#include "atmosphere.glsl"
#include "foliagealpha.glsl"

const vec3 WHEAT_GOLD = vec3(0.46, 0.33, 0.11); // linear, ripe wheat (terrain.frag's canopy uses the same)


void main() {
	vec4 texel;
	if (!foliageCovered(inKind, inUV, texel)) discard;

	vec3 albedo;
	if (inKind == FOLIAGE_WHEAT) {
		// ripe grain: the atlas' light / dark detail, regraded towards a golden straw (the photo is a dull olive
		// brown, a field of it read as mud). WHEAT_GOLD is shared with terrain.frag's canopy so they match.
		// ripeness varies in big patches across the field (a little greener / paler), and a bit per plant
		vec3 photo = pow(texel.rgb, vec3(2.2));
		float detail = dot(photo, vec3(0.3, 0.59, 0.11)) / 0.152; // relative to the atlas' mean luminance
		vec2 fieldXZ = (inPosition.xz + pc.offsetWorld) * 0.25;
		float ripeness = fract(sin(dot(floor(fieldXZ), vec2(12.9898, 78.233))) * 43758.5453) * 0.5 + inMoisture * 0.5;
		vec3 gold = mix(WHEAT_GOLD, WHEAT_GOLD * vec3(0.85, 1.05, 0.8), smoothstep(0.55, 0.9, ripeness)); // a little greener where it's wetter
		albedo = mix(photo, gold * detail, 0.85) * mix(0.88, 1.1, inSeed);
	} else if (inKind == 3) {
		// mostly white daisies, some tinted petals for a bit of colour in the meadow
		vec3 tint = inSeed < 0.6 ? vec3(1.0) : (inSeed < 0.75 ? vec3(1.0, 0.85, 0.35) : (inSeed < 0.9 ? vec3(0.75, 0.55, 1.0) : vec3(1.0, 0.55, 0.6)));
		float petal = smoothstep(0.55, 0.8, dot(texel.rgb, vec3(0.33))); // the white parts, not the yellow middle
		albedo = pow(texel.rgb, vec3(2.2)) * mix(vec3(1.0), tint, petal) * 0.85;
	} else {
		// greener at the root, lighter and a bit yellow at the tip, drier grass goes straw coloured. big patches of
		// lusher / paler grass across the meadow so it isn't one flat colour
		// procedural blade colour (linear): a dark base, a fresh green tip, a slightly lighter midrib
		float midrib = 1.0 - smoothstep(0.0, 0.12, abs(inUV.x - 0.5));
		// (a bit brighter and less saturated than before: next to the meadow's ground texture the darker blades made
		// every place the grass thins out read as a seam)
		// species: meadows are a mix, in patches (~15m): fresh green, blue green fescue, warm yellow green, and a seeded
		// kind with tan / reddish tips. one green everywhere looked painted
		vec2 speciesXZ = (inPosition.xz + pc.offsetWorld) * 0.4;
		vec2 speciesCell = floor(speciesXZ + vec2(hash42(floor(speciesXZ * 0.5)).x) * 0.8);
		float species = hash42(speciesCell + 7.0).y;
		vec3 base = vec3(0.04, 0.075, 0.02), tip = vec3(0.14, 0.22, 0.05);
		if (species > 0.75) { base = vec3(0.035, 0.07, 0.03); tip = vec3(0.11, 0.2, 0.085); }	// fescue (blue green, not teal)
		else if (species > 0.5) { base = vec3(0.06, 0.08, 0.02); tip = vec3(0.2, 0.24, 0.06); }	// yellow green
		float t = smoothstep(0.0, 0.9, inUV.y);
		albedo = mix(base, tip, t) * (1.0 + midrib * 0.25);
		if (species < 0.18) albedo = mix(albedo, vec3(0.15, 0.12, 0.06), smoothstep(0.78, 0.97, inUV.y) * 0.6); // seeded tips (darker: in full sun they read as white)
		vec2 worldXZ = (inPosition.xz + pc.offsetWorld) * 0.35;
		vec2 cell = floor(worldXZ), f = fract(worldXZ);
		f = f * f * (3.0 - 2.0 * f);
		float patches = mix(mix(hash42(cell).x, hash42(cell + vec2(1, 0)).x, f.x), mix(hash42(cell + vec2(0, 1)).x, hash42(cell + vec2(1, 1)).x, f.x), f.y);
		albedo *= mix(vec3(0.75, 0.95, 0.7), vec3(1.15, 1.05, 0.75), patches);
		albedo *= mix(vec3(0.9, 0.97, 0.85), vec3(1.1, 1.03, 0.9), inSeed);
		// every blade a little lighter / darker (they face the light at their own angles), some tips yellowed, and a
		// few dead straw blades among the green: one clean green per patch read as a painted lawn
		albedo *= 0.78 + 0.44 * fract(inSeed * 11.7);
		albedo = mix(albedo, albedo * vec3(1.35, 1.1, 0.55), smoothstep(0.65, 1.0, inUV.y) * 0.4 * step(0.6, fract(inSeed * 5.3)));
		albedo = mix(albedo, vec3(0.15, 0.115, 0.06) * (0.8 + 0.4 * fract(inSeed * 3.7)), step(0.93, fract(inSeed * 7.13)));
		float grey = dot(albedo, vec3(0.3, 0.59, 0.11));
		albedo = mix(vec3(grey), albedo, 0.85);
		float dry = smoothstep(0.3, 0.15, inMoisture); // only the driest, the ground under it stays lush green otherwise
		float luma = dot(albedo, vec3(0.3, 0.59, 0.11));
		albedo = mix(albedo, luma * vec3(1.55, 1.2, 0.55), dry * 0.6); // golden tan straw (a paler one went cream white in the sun)
	}

	// darker near the roots, the clump shadows itself
	// (gentler than it was: dark bases on every blade made the grass far darker than the ground it grows on)
	float ao = mix(0.55, 1.0, smoothstep(0.0, 0.7, inHeight));

	// the visible side of a blade is the one facing the camera. gl_FrontFacing can't be trusted for that here (the
	// cards turn towards the camera and the projection flips y, so the camera side came out as the back face and
	// blades looked lit from behind: bright when looking towards the light, dark when looking away)
	vec3 viewDir = normalize(inPosition - pc.campos);
	vec3 normal = normalize(inNormal);
	if (dot(normal.xz, viewDir.xz) > 0.0) normal.xz = -normal.xz;
	// mostly the ground's normal: a meadow on a slope facing away from the sun is as dark as the slope, not lit like
	// flat ground. the card's own normal keeps a little shape in the blades
	// (0.35 of the blade's own made every blade in a patch the same flat shade)
	normal = normalize(mix(inGroundNormal, normal, 0.55));

	writeGBuffer(albedo, ao, normal, MAT_GRASS, 1.0, sqrt(ao)); // the direct light darkens less than the ambient // lit in lighting.comp
	writeVelocity(pc.viewproj, inPosition, inPrevPosition);
}
