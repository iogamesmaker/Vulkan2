#ifndef BIOME_TOOLS_GLSL
#define BIOME_TOOLS_GLSL
#include "heightmapscale.glsl"
// biomes come from three slowly varying climate fields, written into the heightmap's rgb by heightmap.comp:
//   r = temperature, g = moisture, b = ruggedness (all 0..1)
// every biome is a soft region in that space, so the weights (and with them the heights and textures) blend
// smoothly. taiga needs it cold and desert needs it hot, and temperature only changes slowly, so there's
// always a temperate band (plains / wheat) between them, they can never touch.
// used by both heightmap.comp (heights) and terrain.frag (textures) so they always agree.

#define BIOME_COUNT 6

// these double as the texture indices (albedo[] / normalmap[] / pom[] in terrain.frag)
#define BIOME_PLAINS	0 // grass
#define BIOME_WHEAT		1 // soil for now, wheat later
#define BIOME_MOUNTAINS	2 // rock
#define BIOME_DESERT	3 // sand
#define BIOME_TAIGA		4 // forest floor
#define BIOME_SNOW		5 // snow

// temperature thresholds, the gap between taiga and desert is what keeps them apart
#define TAIGA_MAX_TEMP	0.38
#define DESERT_MIN_TEMP	0.62

// what the ground is covered with given the climate, ignoring mountains (mountains only change the shape,
// their slopes keep the local cover and get rock on cliffs / up high, see terrain.frag)
void groundCover(vec3 climate, bool allowFarmland, out float weights[BIOME_COUNT]) {
	float temperature = climate.r;
	float moisture	  = climate.g;

	float snow	 = smoothstep(0.22, 0.12, temperature);
	float taiga	 = smoothstep(TAIGA_MAX_TEMP, TAIGA_MAX_TEMP - 0.1, temperature) * (1.0 - snow);
	float desert = smoothstep(DESERT_MIN_TEMP, DESERT_MIN_TEMP + 0.1, temperature) * smoothstep(0.45, 0.33, moisture);

	// whatever's left is temperate: wheat in a fairly dry band of it, grassy plains everywhere else
	float temperate = max(1.0 - snow - taiga - desert, 0.0);
	float wheat	 = allowFarmland ? temperate * smoothstep(0.30, 0.35, moisture) * smoothstep(0.45, 0.40, moisture) : 0.0;

	weights[BIOME_PLAINS]	 = temperate - wheat;
	weights[BIOME_WHEAT]	 = wheat;
	weights[BIOME_MOUNTAINS] = 0.0;
	weights[BIOME_DESERT]	 = desert;
	weights[BIOME_TAIGA]	 = taiga;
	weights[BIOME_SNOW]		 = snow;
}

float mountainWeight(vec3 climate) {
	return smoothstep(0.55, 0.72, climate.b);
}

// how much of the mountains' relief the terrain shape gets: a wider band than mountainWeight (the texturing,
// trees, snow), so the land rises into them over ~twice the distance instead of a steep ramp. starts a bit before
// the rock and snow do, the foothills are still green
float mountainShapeWeight(vec3 climate) {
	return smoothstep(0.45, 0.8, climate.b);
}

// biome weights for the terrain shape (heightmap.comp)
void biomeWeights(vec3 climate, out float weights[BIOME_COUNT]) {
	groundCover(climate, true, weights);
	float mountains = mountainShapeWeight(climate);
	for (int i = 0; i < BIOME_COUNT; i++) weights[i] *= 1.0 - mountains;
	weights[BIOME_MOUNTAINS] = mountains;
}

#endif
