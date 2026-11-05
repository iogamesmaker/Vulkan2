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

// the chances of a conifer, a broadleaf and a young tree (either kind, smaller) in a cell (before its roll), from the ground there (the heightmap's
// rgba: climate + height). treelineJitter moves the treeline (-0.5..0.5 per tree, 0 = the average), density = the
// setting. placeTree rolls against these; horizonao.comp bakes their average into the forest canopy map
vec3 treeChances(vec4 data, float treelineJitter, float density) {
	float temperature = data.r, moisture = data.g, height = data.a;
	float weights[BIOME_COUNT];
	groundCover(data.rgb, true, weights);
	float mountains = mountainWeight(data.rgb);

	// treeline on mountains, a bit lower where it's cold
	float treeline = mix(0.42, 0.56, temperature) + treelineJitter * 0.05;
	float belowTreeline = mountains * (1.0 - smoothstep(treeline - 0.04, treeline, height));
	float lowland = 1.0 - mountains;

	float conifer = (weights[BIOME_TAIGA] * 0.75 + weights[BIOME_SNOW] * 0.1) * lowland
				  + belowTreeline * (temperature < 0.5 ? 0.45 : 0.1);
	float broadleaf = weights[BIOME_PLAINS] * (0.03 + 0.55 * smoothstep(0.6, 0.78, moisture)) * lowland
					+ belowTreeline * (temperature < 0.5 ? 0.05 : 0.35);
	float young = (weights[BIOME_PLAINS] * 0.08 + weights[BIOME_TAIGA] * 0.06 + weights[BIOME_DESERT] * 0.04) * lowland
			   + belowTreeline * 0.05;
	// no trees in wheat fields (the same field mask as foliage.comp)
	float field = smoothstep(0.35, 0.5, weights[BIOME_WHEAT] * lowland);
	return vec3(conifer, broadleaf, young) * density * (1.0 - field);
}

// ---- where the ground shows bare stone / snow (the scatter passes: foliage.comp, groundcover.comp)

// terrain.frag's rock / snow line noise, the same function at the same heightmap coordinate
float lineHash(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float lineValueNoise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(lineHash(i), lineHash(i + vec2(1.0, 0.0)), u.x),
			   mix(lineHash(i + vec2(0.0, 1.0)), lineHash(i + vec2(1.0, 1.0)), u.x), u.y);
}

struct GroundBareness {
	float stone;   // bare rock (cliffs, above the rock line): the rock texture shows from ~0.2
	float snowCap; // snow cover
	float bare;	   // both, as one "nothing grows here" measure (stone counts 4x, it shows early)
};

// terrain.frag's own rock and snow masks, noise included, so plants grow right up to where the ground turns to
// rock / snow and never on the stone. (a fixed slope cut left grass texture with nothing on it; the masks without
// the noise put plants on the rock wherever the noise had pulled the rock line down.) data = climate rgb + height,
// slope = the squared heightmap gradient (terrain.frag's measure)
GroundBareness groundBareness(vec2 uv, vec4 data, float slope) {
	float mountains = mountainWeight(data.rgb);
	float lineNoise = lineValueNoise(uv * 180.0) * 0.65 + lineValueNoise(uv * 900.0) * 0.35;
	// bare rock on cliffs: in the lowlands only where it's really steep (the erosion's small scarps on the grassy
	// hills were all rock streaks), the mountains from a gentler slope
	float steepRock = smoothstep(mix(0.6, 0.35, mountains), mix(1.0, 0.75, mountains), slope + (lineNoise - 0.5) * 0.3);
	float rockline = 0.58 + (lineNoise - 0.5) * 0.12;
	float highRock = smoothstep(rockline, rockline + 0.06, data.a) * mountains;
	float snowline = mix(0.44, 0.68, data.r);
	// the snow as terrain.frag maps it (its cover goes through a 0.3..0.7 edge, plants stop inside it, a margin
	// for the noise it frays its edge with)
	float snowCover = smoothstep(snowline - 0.03, snowline + 0.06, data.a) * (1.0 - steepRock * 0.8);
	GroundBareness g;
	g.snowCap = smoothstep(0.1, 0.3, snowCover) * mountains;
	g.stone = max(steepRock, highRock);
	g.bare = max(g.stone * 4.0, g.snowCap);
	return g;
}

#endif
