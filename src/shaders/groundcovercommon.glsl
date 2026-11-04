// shared between groundcover.comp / .vert / .frag: ferns, bushes, weeds, nettles, dead branches (Poly Haven CC0
// models in assets/plants) scattered around the camera between the grass and the trees
#ifndef GROUNDCOVER_COMMON_GLSL
#define GROUNDCOVER_COMMON_GLSL

#include "foliagecommon.glsl" // the same push constants as the grass (FoliagePushConstants), pc.interaction = density

// must match GroundCover in groundcover.hpp
#define GC_SPECIES 9
#define GC_MAX_MESHES 64
#define GC_CAPACITY 4096u
// species
#define GC_FERN 0
#define GC_BRANCHES 1 // dead branches lying on the forest floor (opaque bark)
#define GC_SHRUB 2	  // big leafy bushes, ~1.5m
#define GC_BUSH 3	  // small round bushes, ~30cm
#define GC_NETTLE 4
#define GC_PERIWINKLE 5 // low creeping ground cover
#define GC_SORREL 6		// tiny clover leaves
#define GC_WEED 7
#define GC_DANDELION 8

// two grids: the bigger plants on a coarse one out to GC_RADIUS_BIG, the small ones on a fine one close by
#define GC_CELL_BIG 0.15	// world units (~0.9m)
#define GC_RADIUS_BIG 14.0	// ~85m (the big shrubs, everything else stops closer)
#define GC_CELL_SMALL 0.05	// ~0.3m
#define GC_RADIUS_SMALL 3.5 // ~21m
#define GC_SHADOW_DISTANCE 3.5 // ~20m, the shrubs twice that

struct GroundCoverInstance {
	vec4 posScale; // xyz terrain space, w = scale (model metres -> world units, the variation included)
	vec4 params;   // x = yaw, y = tint, z = seed, w = how much it sways
};

struct GroundCoverSpecies {
	uvec4 meshes; // x = first mesh, y = mesh count
	vec4 size;	  // x = the tallest / widest variant's radius in metres
};

#endif
