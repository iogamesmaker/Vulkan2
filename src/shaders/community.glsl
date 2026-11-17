#ifndef COMMUNITY_GLSL
#define COMMUNITY_GLSL
#include "noise.glsl"

// how much one kind of plant thrives here (world = continuous world units, ~6m each): every kind has its own regions of
// a few hundred metres where it's common and others where it's (nearly) absent, so each meadow / wood has a few
// dominant plants of its own instead of the same even mix of everything. ~0.05 .. 2.4, about 1 on average
float community(vec2 world, float kind) {
	float n = latticeNoise(world / 34.0 + kind * 17.31) * 0.7 + latticeNoise(world / 12.0 - kind * 7.13) * 0.3;
	return mix(0.05, 2.4, smoothstep(0.32, 0.68, n));
}

// the wild oat prairie (the "wheat" biome, biometools.glsl's field mask: natural, not farmed): drifts of tall oat grass
// with plain grass between them, bare soil showing here and there, the oats green in some stretches and gold in
// others. foliage.comp / .frag grow and colour the plants by it, terrainmaterial.glsl the ground and the far canopy.
// world = continuous world units. x = tall oats (0..1), y = bare soil, z = ripeness (0 green .. 1 gold)
vec3 prairieMix(vec2 world) {
	float bare = smoothstep(0.66, 0.82, latticeNoise(world / 3.0 + 77.0) * 0.75 + latticeNoise(world / 0.9 + 3.0) * 0.25);
	float oats = smoothstep(0.32, 0.62, latticeNoise(world / 7.0 + 13.0) * 0.7 + latticeNoise(world / 2.2 - 5.0) * 0.3);
	float ripe = clamp(latticeNoise(world / 18.0 - 41.0) * 1.3 - 0.15 + (latticeNoise(world / 4.0 + 9.0) - 0.5) * 0.4, 0.0, 1.0);
	return vec3(oats * (1.0 - bare), bare, ripe);
}

#endif
