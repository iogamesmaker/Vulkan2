#version 450
#extension GL_EXT_nonuniform_qualifier : require // texture index changes per pixel
#define GBUFFER_WRITE
#include "gbuffer.glsl"
#include "biometools.glsl"
#include "terrainheight.glsl"

// depth EQUAL after the prepass (terrain_depth.frag discards the same edges): test before shading. without it the
// discards below made the depth test run after the shader, and the whole material ran for every terrain fragment,
// the ground behind the trees, the rocks, the ground cover and the nearer hills included
layout(early_fragment_tests) in;

layout(location = 0) in vec3 fragPos;
layout(location = 1) in vec2 fragUV;
layout(location = 5) flat in int LOD;

layout(set = 0, binding = 0) uniform sampler2D heightmap;
layout(set = 0, binding = 4) uniform sampler2D climateMap; // half resolution

layout(set = 0, binding = 1) uniform sampler2D albedo[];
layout(set = 0, binding = 2) uniform sampler2D normalmap[];
layout(set = 0, binding = 3) uniform sampler2D pom[];
layout(set = 0, binding = 5) uniform sampler2D skyVisibility; // horizon AO from the heightmap (horizonao.comp)

// the far field cache (terraincache.hpp): the finished surface baked around the camera in levels of doubling size
#define TERRAIN_CACHE_LEVELS 6
layout(set = 0, binding = 6) uniform TerrainCacheInfo {
	vec4 levels[TERRAIN_CACHE_LEVELS]; // xy = the baked square's first texel (world texel coordinates), z = texel size (world units), w = 1 when baked
	vec4 params;					   // x = distance the cache takes over at, y = 1 when on, z = texels per side
} cache;
layout(set = 0, binding = 7) uniform sampler2DArray cacheAlbedo; // sRGB, a = wheat canopy share, wraps (toroidal)
layout(set = 0, binding = 8) uniform sampler2DArray cacheNormal; // rg = world normal xz * 0.5 + 0.5

layout(set = 1, binding = 0) uniform sampler2D transmittanceLUT;
layout(set = 1, binding = 1) uniform sampler2D multiscatteringLUT;
layout(set = 1, binding = 2) uniform sampler2D skyviewLUT;
layout(set = 1, binding = 3) uniform sampler3D aerialLUT;

#define ATMOSPHERE_SET 1
#define ATMOSPHERE_BINDING 4
#include "atmosphere.glsl"


layout(push_constant) uniform constants {
	mat4 viewproj;
	vec3 campos;
	float tessellationFactor;
	vec3 sundir;
	float factor;
	ivec2 offset;
	float projScale;
	float worldRadius;
	float fixedTessLevel; // > 0: shadow pass, ignore the camera based LOD
	float time;			  // seconds, wind over the wheat canopy
	vec2 detailOffset; // map offset in world units, modulo TERRAIN_DETAIL_PERIOD
} pc;

#include "terrainmaterial.glsl"
#define SNOW_SET 0
#define SNOW_BINDING 9
#include "snow.glsl"

// one cache level at this world position: false where it isn't baked (yet), the caller shades it directly
bool sampleCache(int level, vec2 worldXZ, vec2 dx, vec2 dy, out vec4 albedoCanopy, out vec2 normalXZ) {
	vec4 info = cache.levels[level];
	vec2 inside = worldXZ / info.z - info.xy; // texels into the baked square
	const float MARGIN = 8.0; // the filter footprint at its edge
	if (info.w < 0.5 || any(lessThan(inside, vec2(MARGIN))) || any(greaterThan(inside, vec2(cache.params.z - MARGIN)))) return false;
	float scale = 1.0 / (info.z * cache.params.z); // world units -> uv, the texture wraps
	vec3 uv = vec3(worldXZ * scale, float(level));
	albedoCanopy = textureGrad(cacheAlbedo, uv, dx * scale, dy * scale);
	normalXZ = textureGrad(cacheNormal, uv, dx * scale, dy * scale).rg * 2.0 - 1.0;
	return true;
}

void main() {
	if(fragUV.x > 0.984 || fragUV.y > 0.984 || fragUV.x < 0.016 || fragUV.y < 0.016) discard;
	float horizontalDistance = distance(fragPos.xz, pc.campos.xz);
	if(horizontalDistance > pc.worldRadius) discard; // the loaded world is a circle around the camera

	vec2 size = vec2(textureSize(heightmap, 0));
	vec2 readCoord = fragUV + (vec2(pc.offset) / size);
	// (the texture gradients come from these, taken while every pixel of the quad is still running)
	// per pixel derivatives: the coarse ones (one per 2x2 quad on Intel) gave each quad its own mip and parallax
	// depth, small squares fixed to the screen, worst far away and zoomed in
	vec3 dPdx = dFdxFine(fragPos), dPdy = dFdyFine(fragPos);
	float viewDistance = distance(fragPos, pc.campos);
	vec2 worldXZ = fragPos.xz + vec2(pc.offset) * (ubo.terrainSize / size.x); // continuous world units
	float bumpFade = detailFade(horizontalDistance) * ubo.terrainBumps;
	// deep snow (snow.glsl): terrain.tese smooths most of the ground's bumps over under it, their shading too
	float snowHere = horizontalDistance < DETAIL_FADE_END ? snowDepthAt(readCoord) : 0.0;
	float snowShare = snowHere / (SNOW_DEPTH_METRES * snowInfo.window.w);
	bumpFade *= 1.0 - 0.6 * snowShare;

	// far away the surface comes from the cache: one or two filtered reads instead of the whole material. the
	// level by distance (each one's texels about a pixel big over its range), the next level fading in over the
	// last quarter of it, the direct shading fading out over the quarter before the first
	float cacheWeight = 0.0;
	vec4 cachedAlbedo = vec4(0.0);
	vec2 cachedNormal = vec2(0.0);
	if (cache.params.y > 0.5 && ubo.terrainCache > 0.5) { // (settings: off shades every pixel in full)
		float levelF = log2(max(viewDistance, 1e-3) / cache.params.x);
		// the cache is baked from above: on a steep face its texels stretch down the slope by 1 / normal.y and the
		// cliffs' cracks smeared. there it takes over later, at up to ~1.4x the distance (twice cost back half of
		// what the earlier start saves: most of a mountain view is steep), the level itself still goes by the distance
		float steepness = abs(normalize(cross(dPdx, dPdy)).y);
		float startF = levelF + log2(clamp(steepness, 0.7, 1.0));
		cacheWeight = smoothstep(-0.25, 0.0, startF) *(1.0 - smoothstep(float(TERRAIN_CACHE_LEVELS) - 0.25, float(TERRAIN_CACHE_LEVELS), levelF));
		if (cacheWeight > 0.0) {
			int level = clamp(int(floor(levelF)), 0, TERRAIN_CACHE_LEVELS - 1);
			float toNext = level + 1 < TERRAIN_CACHE_LEVELS ? smoothstep(0.75, 1.0, levelF - float(level)) : 0.0;
			bool baked = sampleCache(level, worldXZ, dPdx.xz, dPdy.xz, cachedAlbedo, cachedNormal);
			if (toNext > 0.0) {
				vec4 nextAlbedo; vec2 nextNormal;
				baked = baked && sampleCache(level + 1, worldXZ, dPdx.xz, dPdy.xz, nextAlbedo, nextNormal);
				cachedAlbedo = mix(cachedAlbedo, nextAlbedo, toNext);
				cachedNormal = mix(cachedNormal, nextNormal, toNext);
			}
			if (!baked) cacheWeight = 0.0;
		}
	}

	TerrainSurface surface;
	surface.albedo = vec3(0.0); surface.normal = vec3(0.0, 1.0, 0.0); surface.canopy = 0.0; surface.directScale = 1.0;
	if (cacheWeight < 1.0) {
		TerrainShading s;
		s.position = fragPos;
		s.dPdx = dPdx; s.dPdy = dPdy;
		s.readCoord = readCoord;
		s.detailPos = fragPos.xz + pc.detailOffset; // (fragPos is small near the camera so it's precise where it matters)
		s.detailLocal = fragPos.xz;
		s.detailOffset = pc.detailOffset;
		s.worldXZ = worldXZ;
		s.factor = pc.factor;
		s.viewDistance = viewDistance;
		s.campos = pc.campos;
		s.sundir = pc.sundir;
		// a shorter handover between the near and the far texture scale: inside it both are sampled (twice the cost)
		// (complete where the cache takes over, TerrainCache::START_DISTANCE: it's baked at the far scale)
		s.farWeight = smoothstep(15.0, 30.0, viewDistance);
		s.macroWeight = smoothstep(180.0, 260.0, viewDistance) * ubo.terrainMacro; // a short handover, both layers cost double
		s.bumpFade = bumpFade;
		s.canopyNear = mix(0.45, 0.9, smoothstep(6.0, 11.0, viewDistance));
		surface = shadeTerrain(s);
	}
	if (cacheWeight > 0.0) {
		vec3 normal = vec3(cachedNormal.x, sqrt(max(1.0 - dot(cachedNormal, cachedNormal), 0.0)), cachedNormal.y);
		// the procedural bumps reach further than the cache's start: on top of the baked normal (they're too fine for it)
		if (bumpFade > 0.0) {
			vec2 bumpSlope = terrainDetailGrad(readCoord * HEIGHTMAP_REFERENCE_SIZE);
			normal = normalize(normal - vec3(bumpSlope.x, 0.0, bumpSlope.y) / (ubo.terrainSize / HEIGHTMAP_REFERENCE_SIZE) * bumpFade);
		}
		surface.albedo = mix(surface.albedo, cachedAlbedo.rgb, cacheWeight);
		surface.normal = normalize(mix(surface.normal, normal, cacheWeight));
		surface.canopy = mix(surface.canopy, cachedAlbedo.a, cacheWeight);
		surface.directScale = mix(surface.directScale, 1.0, cacheWeight); // (the far field has no parallax)
	}

	// the prints and the furrow in the snow: the normal follows the pressed surface terrain.tese built (its triangles
	// are too coarse to shade a print by), the packed snow at their bottom a little greyer, the light dimmer in there
	if (snowHere > 0.0) {
		vec2 rel = fragPos.xz - pc.campos.xz;
		vec2 trail = snowTrailAt(rel, vec2(0.0));
		if (trail.g > 0.0) {
			float hx = snowTrailOffset(snowTrailAt(rel, vec2(1.0, 0.0)), snowHere) - snowTrailOffset(snowTrailAt(rel, vec2(-1.0, 0.0)), snowHere);
			float hz = snowTrailOffset(snowTrailAt(rel, vec2(0.0, 1.0)), snowHere) - snowTrailOffset(snowTrailAt(rel, vec2(0.0, -1.0)), snowHere);
			vec2 slope = vec2(hx, hz) * snowInfo.camera.z * 0.5; // height per world unit (the differences span two texels)
			surface.normal = normalize(surface.normal - vec3(slope.x, 0.0, slope.y));
			surface.albedo *= mix(vec3(1.0), vec3(0.84, 0.87, 0.92), trail.r * snowShare);
		}
	}

	// the wind gusts rolling over the wheat canopy
	if (surface.canopy > 0.0) surface.albedo *= mix(1.0, canopyGust(worldXZ, pc.time), surface.canopy);


	// lit in lighting.comp
	writeGBuffer(surface.albedo, 1.0, surface.normal, MAT_TERRAIN, 1.0, surface.directScale);
}
