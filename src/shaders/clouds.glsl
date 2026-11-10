// a layer of cumulus-ish clouds at ubo.cloudAltitude: 2D domain warped fbm with a coverage threshold, so it's
// cheap enough for every sky pixel (sky.frag) and for one cloud shadow lookup per lit pixel (lighting.comp).
// positions are continuous world kilometres (camera position + map offset), wind is added by the callers.
// needs atmosphere.glsl (ubo) included first
#ifndef CLOUDS_GLSL
#define CLOUDS_GLSL

float cloudHash(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float cloudNoise(vec2 p) {
	vec2 i = floor(p), f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(cloudHash(i), cloudHash(i + vec2(1.0, 0.0)), u.x), mix(cloudHash(i + vec2(0.0, 1.0)), cloudHash(i + vec2(1.0, 1.0)), u.x), u.y);
}

float cloudFbm(vec2 p, int octaves) {
	float sum = 0.0, amplitude = 0.5;
	const mat2 rotation = mat2(0.8, 0.6, -0.6, 0.8);
	for (int i = 0; i < octaves; i++) {
		sum += cloudNoise(p) * amplitude;
		p = rotation * p * 2.02 + 17.3;
		amplitude *= 0.5;
	}
	return sum;
}

// the coverage independent part of the pattern: a warped fbm (~0.1 .. 0.9). two sizes of cloud, fields of small
// puffs (~1km) and big heaps (~3km), mixed by a slow noise (~12km) so the sky isn't one size of cloud everywhere,
// and the coverage swells and thins regionally on top (~20km). p = the small pattern's warped position (billows)
float cloudShape(vec2 worldKm, int octaves, out vec2 p) {
	float sizeMix = smoothstep(0.3, 0.7, cloudNoise(worldKm * 0.08 + vec2(-33.1, 5.9)));
	float region = cloudNoise(worldKm * 0.045 + vec2(71.3, -12.7)) - 0.5;
	p = worldKm * 0.8;
	p += (vec2(cloudNoise(p * 0.35 + 3.1), cloudNoise(p * 0.35 - 8.7)) - 0.5) * 1.6; // billowy, irregular outlines
	float small = sizeMix < 0.999 ? cloudFbm(p, octaves) : 0.0;
	float large = 0.0;
	if (sizeMix > 0.001) {
		vec2 q = worldKm * 0.28 + vec2(57.1, -21.4);
		q += (vec2(cloudNoise(q * 0.35 + 3.1), cloudNoise(q * 0.35 - 8.7)) - 0.5) * 1.6;
		large = cloudFbm(q, octaves);
	}
	// two independent noises averaged lose contrast where they mix (fewer clouds in between), scaled back up
	float mean = 0.5 * (1.0 - pow(0.5, float(octaves)));
	float spread = inversesqrt(sizeMix * sizeMix + (1.0 - sizeMix) * (1.0 - sizeMix));
	float n = mean + (mix(small, large, sizeMix) - mean) * spread;
	return n + region * 0.14;
}

// ---- the weather field: where the weather is. a slow noise (~9km cells) in the same wind carried space as the
// cloud pattern, so it drifts with the clouds: showers and thunderstorms are cells of it, fronts roll in from upwind.
// raw 0..1, the weather's instability (ubo.weather3.x) decides how much of it shows. weather.cpp mirrors this
// exactly (the rain where you stand, where lightning strikes): keep the two in step
float weatherCells(vec2 worldKm) {
	vec2 p = worldKm * 0.09 + vec2(13.7, -41.2);
	return cloudNoise(p) * 0.65 + cloudNoise(p * 2.03 + vec2(7.1, 3.3)) * 0.35;
}
// how much a place is inside a cell (0 .. 1), the core of the storm at 1
float weatherCellCore(float cells) { return smoothstep(0.55, 0.78, cells); }

// the coverage at a place: the weather's own, thinner between the cells and full inside them as the air gets unstable
float localCoverage(float cells) {
	float instability = ubo.weather3.x;
	float core = weatherCellCore(cells);
	return clamp(ubo.cloudCoverage * (1.0 - instability * 0.45) + core * instability, 0.0, 1.0);
}

// how hard it rains (or snows) at a place, 0..1: everywhere under a thick deck, and under the cells
float localPrecipitation(float cells) {
	float core = weatherCellCore(cells);
	return clamp(ubo.weather3.y * smoothstep(0.75, 0.95, localCoverage(cells)) + ubo.weather3.z * smoothstep(0.2, 0.8, core), 0.0, 1.0);
}

// fine noise eating into the thin parts of a cloud (the edges turn cauliflower instead of airbrushed)
float cloudBillowNoise(vec2 p) { return cloudNoise(p * 7.3 + 41.0); }

// the shape into a density at the current coverage: 0 = nothing, 1 = overcast moves the threshold. a ramp, not a
// clamp, so the inside of a cloud still has structure for the lighting to pick up. billow < 0: none
float cloudShapeDensity(float n, float billow, float coverage) {
	float threshold = mix(0.58, 0.2, coverage);
	float density = smoothstep(threshold, threshold + 0.32, n);
	if (billow >= 0.0 && density > 0.0) density = clamp(density - (1.0 - density) * billow * 0.7, 0.0, 1.0);
	return density;
}

// 0 = clear sky, ~1 = the thickest part of a cloud. octaves: 5 for the sky, fewer for shadows / light samples
float cloudDensity(vec2 worldKm, int octaves) {
	vec2 p;
	float n = cloudShape(worldKm, octaves, p);
	return cloudShapeDensity(n, octaves > 3 ? cloudBillowNoise(p) : -1.0, localCoverage(weatherCells(worldKm)));
}

// the coverage through the lookup table around the camera where there is one (cloudmap.comp, define
// CLOUD_COVERAGE_MAP with cloudCoverageMap + the CloudMapInfo block cloudMapInfo: xy centre, z extent km, w valid),
// the exact formula outside it (far clouds) and in the shaders without it. the map wraps (a torus, repeat sampled):
// texel i holds world km [i, i + 1) * texel modulo its size, only the strips the camera moved into get redone. it
// stores the shape, not the density: the weather's coverage changing doesn't need it redone. b = the weather field
#ifdef CLOUD_COVERAGE_MAP
vec2 cloudMapUV(vec2 worldKm) { return worldKm / cloudMapInfo.z; }
bool cloudMapCovers(vec2 worldKm) { return cloudMapInfo.w > 0.5 && all(lessThan(abs(worldKm - cloudMapInfo.xy), vec2(cloudMapInfo.z * 0.495))); }
#endif
float cloudCoverage3(vec2 worldKm) {
#ifdef CLOUD_COVERAGE_MAP
	if (cloudMapCovers(worldKm)) {
		vec4 t = textureLod(cloudCoverageMap, cloudMapUV(worldKm), 0.0);
		return cloudShapeDensity(t.r, -1.0, localCoverage(t.b));
	}
#endif
	return cloudDensity(worldKm, 3);
}
float cloudCoverage4(vec2 worldKm) {
#ifdef CLOUD_COVERAGE_MAP
	if (cloudMapCovers(worldKm)) {
		vec4 t = textureLod(cloudCoverageMap, cloudMapUV(worldKm), 0.0);
		return cloudShapeDensity(t.g, t.a, localCoverage(t.b));
	}
#endif
	return cloudDensity(worldKm, 4);
}
// the weather field at a place (the map's b channel where there is one)
float cloudWeatherCells(vec2 worldKm) {
#ifdef CLOUD_COVERAGE_MAP
	if (cloudMapCovers(worldKm)) return textureLod(cloudCoverageMap, cloudMapUV(worldKm), 0.0).b;
#endif
	return weatherCells(worldKm);
}
float cloudBaseNoise(vec2 worldKm) { // the cumulus' uneven bases (volclouds.glsl), from the billows (their map channel is shared)
#ifdef CLOUD_COVERAGE_MAP
	if (cloudMapCovers(worldKm)) return textureLod(cloudCoverageMap, cloudMapUV(worldKm), 0.0).a;
#endif
	vec2 p = worldKm * 0.8;
	p += (vec2(cloudNoise(p * 0.35 + 3.1), cloudNoise(p * 0.35 - 8.7)) - 0.5) * 1.6;
	return cloudBillowNoise(p);
}

#endif
