// which ground cover plant grows in a cell of one of the ground cover's world anchored grids: groundcover.comp places
// it, trees_place.comp makes the same pick for the big ones the tree pipeline draws (gcTreeKind) and places those, so
// every plant grows exactly where it did and nothing is drawn twice. no push constants: the callers pass what it needs
#ifndef GROUNDCOVER_PICK_GLSL
#define GROUNDCOVER_PICK_GLSL

#include "groundcoverspecies.glsl"
#include "biometools.glsl"
#include "community.glsl"
#include "noise.glsl"

// a cell's own random numbers and where in it the plant stands (world = continuous world units)
struct GroundCoverCell {
	vec4 rnd, rnd2;
	vec2 world;
};

GroundCoverCell groundCoverCell(uint grid, vec2 cellIndex) {
	GroundCoverCell c;
	c.rnd = hash42(cellIndex * 1.37 + float(grid) * 41.0 + 13.0);
	c.rnd2 = hash42(cellIndex * 0.61 - float(grid) * 23.0 - 47.0);
	c.world = (cellIndex + 0.1 + c.rnd.xy * 0.8) * GC_CELL[grid];
	return c;
}

// slow patches (world units), so the plants grow in clusters and clearings instead of an even sprinkle
float patches(vec2 world, float scale, float seed) {
	return latticeNoise(world / scale + seed * 37.0) * 0.7 + latticeNoise(world / (scale * 0.37) - seed * 11.0) * 0.3;
}

// the plant (GC_*) picked for the cell, -1 for none. uv: the heightmap's at the plant, density: the ground cover's
// density setting. data = climate + height there, snowClimate: how much of the snow climate (the snowy look)
int groundCoverKind(sampler2D heightmap, sampler2D climateMap, uint grid, GroundCoverCell c, vec2 uv, float density, out vec4 data, out float snowClimate) {
	vec2 world = c.world;
	vec2 size = vec2(textureSize(heightmap, 0));
	data = vec4(textureLod(climateMap, uv, 0.0).rgb, textureLod(heightmap, uv, 0.0).a); // climate, height
	snowClimate = 0.0;

	// nothing on stone, thinning towards it and the snow (the same masks as the grass, biometools.glsl)
	float hR = textureLodOffset(heightmap, uv, 0.0, ivec2(1, 0)).a, hL = textureLodOffset(heightmap, uv, 0.0, ivec2(-1, 0)).a;
	float hT = textureLodOffset(heightmap, uv, 0.0, ivec2(0, 1)).a, hB = textureLodOffset(heightmap, uv, 0.0, ivec2(0, -1)).a;
	float slope = pow((hR - hL) * heightmapSlopeScale(size.x), 2.0) + pow((hT - hB) * heightmapSlopeScale(size.x), 2.0);
	float mountains = mountainWeight(data.rgb);
	GroundBareness ground = groundBareness(uv, data, slope);
	if (ground.stone > 0.2 || ground.bare > 0.85) return -1;
	float bare = ground.bare;

	float weights[BIOME_COUNT];
	groundCover(data.rgb, true, weights);
	float barren = smoothstep(0.1, 0.3, weights[BIOME_DESERT] + weights[BIOME_SNOW]);
	float field = smoothstep(0.35, 0.5, weights[BIOME_WHEAT] * (1.0 - mountains));
	// the wild oat prairie: a meadow's plants grow in it too, thinner where the tall oats stand (community.glsl)
	if (field > 0.0) field *= 0.35 + 0.5 * prairieMix(world).x;
	float alive = (1.0 - barren) * (1.0 - field) * (1.0 - smoothstep(0.5, 0.9, bare)) * density;
	// the desert's own plants (succulents, dead wood, the creeping grass) grow where the others can't
	float desert = smoothstep(0.1, 0.4, weights[BIOME_DESERT]) * (1.0 - smoothstep(0.1, 0.3, weights[BIOME_SNOW])) * (1.0 - field)
				 * (1.0 - smoothstep(0.5, 0.9, bare)) * density;
	// the snowy forest: the snow climate kills the rest, a few hardy things poke through its snow under the trees
	snowClimate = smoothstep(0.1, 0.3, weights[BIOME_SNOW]) * (1.0 - field) * density;
	if (alive <= 0.0 && desert <= 0.0 && snowClimate <= 0.0) return -1;
	// forest floor: where the trees actually grow (the same chances they're placed by, biometools.glsl). the rest
	// is open grassland (alpine meadows too): rock, snow, sand and fields are already masked out by alive
	vec3 trees = treeChances(data, 0.0, 1.0);
	float forest = smoothstep(0.08, 0.45, trees.x + trees.y);
	float meadow = 1.0 - forest;
	float damp = smoothstep(0.45, 0.75, data.g);
	// forest edges: where the forest thins into the meadow the bushes and nettles crowd
	float edge = forest * meadow * 4.0;
	float conifer = clamp(trees.x / max(trees.x + trees.y, 1e-4), 0.0, 1.0); // how much of the forest here is conifers
	// (the snow climate's own tree chance is only ~0.1, biometools.glsl: a 0.25 ramp left the snowy forests at a quarter)
	float snowForest = snowClimate * smoothstep(0.02, 0.09, trees.x + trees.y);

	// chances per cell (small cells ~0.1 m^2, medium ~0.8 m^2, far ~3.2 m^2)
	float chance[GC_SPECIES];
	for (int i = 0; i < GC_SPECIES; i++) chance[i] = 0.0;
	if (grid == 0u) {
		chance[GC_SORREL] = (forest * 0.3 + meadow * 0.15) * mix(0.2, 1.8, patches(world, 0.6, 6.0));
		chance[GC_WEED] = (meadow * 0.15 + forest * 0.05) * mix(0.3, 1.7, patches(world, 0.8, 7.0));
		chance[GC_DANDELION] = meadow * 0.07 * smoothstep(0.4, 0.75, patches(world, 1.0, 8.0)) * 2.5;
		chance[GC_MOSS] = forest * (0.12 + damp * 0.2) * smoothstep(0.35, 0.65, patches(world, 0.9, 14.0)) * 2.0;
		chance[GC_GRASS_TUFT] = (meadow * 0.12 + edge * 0.05) * mix(0.2, 1.8, patches(world, 0.8, 42.0));
		chance[GC_UNDERGROWTH] = (meadow * 0.07 + edge * 0.04) * smoothstep(0.35, 0.65, patches(world, 1.0, 51.0)) * 2.0;
		// mushrooms: rare, but where they are there's a crowd: a few patches of ~10-20m on the forest floor (more of
		// them where it's damp), clusters inside a patch, nothing in between
		float mushroomPatch = smoothstep(mix(0.8, 0.74, damp), 0.88, patches(world, 3.0, 34.0));
		chance[GC_MUSHROOMS] = forest * mushroomPatch * smoothstep(0.45, 0.7, patches(world, 0.15, 41.0)) * 0.4;
		chance[GC_BERMUDA] = meadow * (1.0 - damp) * 0.08 * mix(0.2, 1.8, patches(world, 0.9, 24.0));
		chance[GC_ICEPLANT] = meadow * (1.0 - damp) * bare * 0.1 * smoothstep(0.4, 0.7, patches(world, 0.8, 25.0));
	} else if (grid == 1u) {
		chance[GC_CELANDINE] = (forest * damp * 0.12 + edge * 0.08 + meadow * damp * 0.03) * smoothstep(0.4, 0.7, patches(world, 1.1, 15.0)) * 2.0;
		chance[GC_GRASS_CLUMP] = (meadow * 0.12 + edge * 0.08) * mix(0.2, 1.8, patches(world, 1.3, 18.0));
		chance[GC_ROOTS] = forest * 0.025;
		chance[GC_FERN] = (forest * 0.55 + meadow * damp * 0.08 + edge * 0.15) * mix(0.15, 1.8, smoothstep(0.3, 0.7, patches(world, 2.0, 1.0)));
		chance[GC_BRANCHES] = forest * 0.04;
		chance[GC_BUSH] = (forest * 0.1 + meadow * 0.08 + edge * 0.1) * mix(0.2, 1.8, patches(world, 1.5, 3.0));
		chance[GC_NETTLE] = (meadow * damp * 0.1 + forest * damp * 0.05 + edge * 0.1) * smoothstep(0.35, 0.65, patches(world, 1.2, 4.0)) * 2.0;
		chance[GC_PERIWINKLE] = forest * 0.16 * smoothstep(0.35, 0.65, patches(world, 1.0, 5.0)) * 2.0;
		// the flowers: drifts of one colour each in the meadows (a few at the forest edges), ~10m patches
		chance[GC_GAZANIA] = meadow * 0.3 * smoothstep(0.4, 0.65, patches(world, 1.6, 9.0)) * (1.0 - damp * 0.5);
		chance[GC_URSINIA] = (meadow * 0.28 + edge * 0.05) * smoothstep(0.4, 0.65, patches(world, 1.4, 10.0));
		chance[GC_EMPODIUM] = (meadow * 0.22 + edge * 0.1) * smoothstep(0.4, 0.65, patches(world, 1.2, 11.0));
		chance[GC_STINKKRUID] = meadow * 0.2 * smoothstep(0.45, 0.7, patches(world, 1.8, 12.0)) * (1.0 - damp * 0.6);
		chance[GC_DAISY] = (meadow * 0.26 + edge * 0.04) * smoothstep(0.4, 0.65, patches(world, 1.5, 26.0)) * (1.0 - damp * 0.4);
		chance[GC_GRASS_TALL] = (meadow * 0.1 + edge * 0.08) * mix(0.2, 1.8, patches(world, 1.4, 27.0));
		chance[GC_ROOT_CLUSTER2] = forest * 0.012;
		chance[GC_BUSH_SMALL] = (forest * 0.08 + meadow * 0.05 + edge * 0.12) * mix(0.2, 1.8, patches(world, 1.9, 46.0));
		// straw tussocks where it's dry and stony, dark sedges in the wet
		chance[GC_DRY_TUSSOCK] = (meadow * (1.0 - damp) * 0.1 + bare * 0.25) * smoothstep(0.35, 0.65, patches(world, 1.5, 22.0)) * 2.0;
		chance[GC_SEDGE] = (meadow + edge * 0.5) * smoothstep(0.55, 0.8, data.g) * 0.3 * mix(0.2, 1.8, patches(world, 1.1, 23.0));
	} else {
		// the big shrubs, far out: thickets at the forest edges, a few in the woods and out in the meadows
		chance[GC_SHRUB] = (forest * 0.1 + meadow * 0.06 + edge * 0.3) * mix(0.2, 1.8, patches(world, 3.0, 2.0));
		chance[GC_ROOIBOS] = (meadow * 0.15 + edge * 0.2 + forest * 0.03) * mix(0.2, 1.8, patches(world, 2.5, 13.0));
		// the real bushes: dense green ones at forest edges, in open woods and dotted over the meadows, the flowering
		// ones in the drier open land
		chance[GC_SHRUB_FLOWER] = (edge * 0.15 + meadow * (1.0 - damp) * 0.08 + forest * 0.02) * mix(0.2, 1.8, patches(world, 2.8, 16.0));
		chance[GC_SHRUB_GREEN] = (forest * 0.12 + edge * 0.3 + meadow * 0.09) * mix(0.2, 1.8, patches(world, 2.2, 17.0));
		// bracken: dense stands at the forest edges and over open, not too wet slopes
		chance[GC_BRACKEN] = (edge * 0.35 + meadow * (1.0 - damp) * 0.08 + forest * 0.04) * smoothstep(0.4, 0.7, patches(world, 2.6, 20.0)) * 2.5;
		// the sprawling dry shrub in the dry open land, young conifers in and at the edges of conifer forests
		// lilacs at the forest edges (saplings out into the meadows, small trees along the edge), not in conifer woods
		chance[GC_LILAC_SAPLING] = (edge * 0.07 + meadow * 0.012 + forest * 0.01) * (1.0 - conifer * 0.8) * mix(0.2, 1.8, patches(world, 2.4, 45.0));
		chance[GC_LILAC_TREE] = (edge * 0.035 + meadow * 0.004) * (1.0 - conifer * 0.8) * mix(0.1, 1.9, patches(world, 3.2, 52.0));
		// a splash of red / orange at a forest edge now and then
		chance[GC_AUTUMN_SHRUB] = (edge * 0.06 + forest * 0.01) * mix(0.2, 1.8, patches(world, 3.5, 21.0));
	}

	// the desert's plants, scaled to the desert's own life below (desertFrom: their chances are * desert, not * alive)
	float desertChance[GC_SPECIES];
	for (int i = 0; i < GC_SPECIES; i++) desertChance[i] = 0.0;
	if (grid == 0u) {
		desertChance[GC_BERMUDA] = 0.006 * mix(0.1, 1.9, patches(world, 1.2, 28.0));
		desertChance[GC_ICEPLANT] = 0.008 * smoothstep(0.45, 0.7, patches(world, 1.0, 29.0)) * 2.0;
	} else if (grid == 1u) {
		desertChance[GC_SUCCULENT] = 0.01 * mix(0.1, 1.9, patches(world, 1.6, 30.0));
		desertChance[GC_QUIVER_BRANCH] = 0.003;
		desertChance[GC_QUIVER_BRANCH2] = 0.003;
		desertChance[GC_CACTUS_CLUMP] = 0.0015 * smoothstep(0.45, 0.7, patches(world, 3.0, 47.0)) * 2.0;
	} else {
		desertChance[GC_CACTUS] = 0.008 * mix(0.1, 1.9, patches(world, 2.6, 48.0));
		desertChance[GC_CACTUS_CLUMP] = 0.002 * smoothstep(0.45, 0.7, patches(world, 3.0, 53.0)) * 2.0;
	}

	// the snowy forest's: sparse, dead branches, roots, straw and the odd dark bush through the snow
	float snowChance[GC_SPECIES];
	for (int i = 0; i < GC_SPECIES; i++) snowChance[i] = 0.0;
	// (dead stalks, branches and roots through the snow, straw tussocks and rusty dead bracken in the gaps, low dark
	// bushes in patches; groundcover.frag dusts them all with snow)
	if (grid == 0u) {
		snowChance[GC_WEED] = 0.05 * mix(0.2, 1.8, patches(world, 0.9, 35.0));
		snowChance[GC_MOSS] = 0.03 * smoothstep(0.45, 0.7, patches(world, 1.0, 36.0)) * 2.0;
	} else if (grid == 1u) {
		snowChance[GC_BRANCHES] = 0.07;
		snowChance[GC_ROOTS] = 0.03;
		snowChance[GC_DRY_TUSSOCK] = 0.14 * smoothstep(0.35, 0.65, patches(world, 1.3, 31.0)) * 2.0;
		snowChance[GC_BUSH] = 0.1 * mix(0.1, 1.9, patches(world, 1.7, 32.0));
		snowChance[GC_FERN] = 0.06 * smoothstep(0.4, 0.7, patches(world, 2.0, 37.0)) * 2.0;
	} else if (grid == 2u) {
		snowChance[GC_SHRUB_GREEN] = 0.07 * mix(0.1, 1.9, patches(world, 2.5, 33.0));

		snowChance[GC_BRACKEN] = 0.08 * smoothstep(0.4, 0.7, patches(world, 3.0, 38.0)) * 2.0;
		snowChance[GC_ROOIBOS] = 0.03 * mix(0.1, 1.9, patches(world, 2.7, 39.0));
	}

	// plant communities (foliagecommon.glsl): each kind has its own regions of a few hundred metres where it's common
	// and others where it's (nearly) gone, so one meadow is a drift of orange daisies between tussocks, the next scrub
	// with rooibos and nettles, the next nearly bare grass, and the woods differ the same way (ferns, moss, periwinkle)
	for (int i = 0; i < GC_SPECIES; i++) {
		if (chance[i] <= 0.0 && desertChance[i] <= 0.0 && snowChance[i] <= 0.0) continue;
		float c = community(world, float(i));
		chance[i] *= c;
		desertChance[i] *= c;
		snowChance[i] *= c;
	}
	// and how much grows at all: lush undergrowth in some places, open grass with little in it in others
	float lush = mix(0.35, 1.35, smoothstep(0.3, 0.7, latticeNoise(world / 40.0 + 91.7)));
	alive *= lush;
	desert *= lush;
	snowForest *= lush;
	for (int i = 0; i < GC_SPECIES; i++) chance[i] = chance[i] * alive + desertChance[i] * desert + snowChance[i] * snowForest;

	// (no chance for an empty slot: a kind with no model at all)
	for (int i = 0; i < GC_SPECIES; i++) if (gcEmptySlot(i)) chance[i] = 0.0;
	float total = 0.0;
	for (int i = 0; i < GC_SPECIES; i++) total += chance[i];
	float roll = c.rnd.z * max(total, 1.0);
	float sum = 0.0;
	for (int i = 0; i < GC_SPECIES; i++) {
		sum += chance[i];
		if (roll < sum) return i;
	}
	return -1;
}

#endif
