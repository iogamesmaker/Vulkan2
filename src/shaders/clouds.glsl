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

// fine noise eating into the thin parts of a cloud (the edges turn cauliflower instead of airbrushed)
float cloudBillowNoise(vec2 p) { return cloudNoise(p * 7.3 + 41.0); }

// the shape into a density at the current coverage: 0 = nothing, 1 = overcast moves the threshold. a ramp, not a
// clamp, so the inside of a cloud still has structure for the lighting to pick up. billow < 0: none
float cloudShapeDensity(float n, float billow) {
	float threshold = mix(0.58, 0.2, ubo.cloudCoverage);
	float density = smoothstep(threshold, threshold + 0.32, n);
	if (billow >= 0.0 && density > 0.0) density = clamp(density - (1.0 - density) * billow * 0.7, 0.0, 1.0);
	return density;
}

// 0 = clear sky, ~1 = the thickest part of a cloud. octaves: 5 for the sky, fewer for shadows / light samples
float cloudDensity(vec2 worldKm, int octaves) {
	vec2 p;
	float n = cloudShape(worldKm, octaves, p);
	return cloudShapeDensity(n, octaves > 3 ? cloudBillowNoise(p) : -1.0);
}

// the coverage through the lookup table around the camera where there is one (cloudmap.comp, define
// CLOUD_COVERAGE_MAP with cloudCoverageMap + the CloudMapInfo block cloudMapInfo: xy centre, z extent km, w valid),
// the exact formula outside it (far clouds) and in the shaders without it. the map wraps (a torus, repeat sampled):
// texel i holds world km [i, i + 1) * texel modulo its size, only the strips the camera moved into get redone. it
// stores the shape, not the density: the weather's coverage changing doesn't need it redone
#ifdef CLOUD_COVERAGE_MAP
vec2 cloudMapUV(vec2 worldKm) { return worldKm / cloudMapInfo.z; }
bool cloudMapCovers(vec2 worldKm) { return cloudMapInfo.w > 0.5 && all(lessThan(abs(worldKm - cloudMapInfo.xy), vec2(cloudMapInfo.z * 0.495))); }
#endif
float cloudCoverage3(vec2 worldKm) {
#ifdef CLOUD_COVERAGE_MAP
	if (cloudMapCovers(worldKm)) return cloudShapeDensity(textureLod(cloudCoverageMap, cloudMapUV(worldKm), 0.0).r, -1.0);
#endif
	return cloudDensity(worldKm, 3);
}
float cloudCoverage4(vec2 worldKm) {
#ifdef CLOUD_COVERAGE_MAP
	if (cloudMapCovers(worldKm)) {
		vec4 t = textureLod(cloudCoverageMap, cloudMapUV(worldKm), 0.0);
		return cloudShapeDensity(t.g, t.a);
	}
#endif
	return cloudDensity(worldKm, 4);
}
float cloudBaseNoise(vec2 worldKm) { // the cumulus' uneven bases (volclouds.glsl)
#ifdef CLOUD_COVERAGE_MAP
	if (cloudMapCovers(worldKm)) return textureLod(cloudCoverageMap, cloudMapUV(worldKm), 0.0).b;
#endif
	return cloudFbm(worldKm * 0.7 + 23.0, 2);
}

// how much sunlight gets through the layer above a point (1 = none in the way), for the cloud shadows
float cloudShadow(vec2 worldKm, float altitudeKm, vec3 sunDir) {
	if (ubo.clouds < 0.5 || ubo.cloudCoverage <= 0.0 || sunDir.y <= 0.02) return 1.0;
	float up = max(ubo.cloudAltitude - altitudeKm, 0.0);
	// where the sun ray from here crosses the layer. towards sunset that point runs off to infinity (1 / sun height):
	// at a few degrees the shadows were cast from 10-30km away and swept across the land as fast as the sun sank, a
	// thousand clouds a minute. capped at ~3x the cloud height sideways (the sun at ~17 degrees), and faded out as the
	// sun gets low: at sunset the light comes through so much air that it's soft anyway
	float slant = max(sunDir.y, 0.3);
	vec2 under = worldKm + sunDir.xz / slant * up;
	float density = cloudCoverage4(under); // 4 octaves: crisp patches, 3 smeared them into broad gradients
	return mix(1.0, exp(-density * ubo.cloudDensity * 4.0), smoothstep(0.04, 0.3, sunDir.y));
}

#endif
