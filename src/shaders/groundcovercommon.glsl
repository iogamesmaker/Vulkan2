// shared between groundcover.comp / .vert / .frag: ferns, bushes, weeds, nettles, dead branches (Poly Haven CC0
// models in assets/plants) scattered around the camera between the grass and the trees
#ifndef GROUNDCOVER_COMMON_GLSL
#define GROUNDCOVER_COMMON_GLSL

#include "foliagecommon.glsl" // the same push constants as the grass (FoliagePushConstants), pc.interaction = density

// must match GroundCover in groundcover.hpp
#define GC_SPECIES 14
#define GC_MAX_MESHES 96
#define GC_CAPACITY 4096u
#define GC_LOD_PIXELS 45.0 // the full model above this radius on screen, the simplified one below (groundcover.comp)
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
#define GC_GAZANIA 9	 // orange / yellow flower clumps
#define GC_URSINIA 10	 // orange daisies
#define GC_EMPODIUM 11	 // little yellow star flowers
#define GC_STINKKRUID 12 // yellow flowering tufts
#define GC_ROOIBOS 13	 // a medium, twiggy bush

// three grids (consecutive thread ranges in groundcover.comp): small plants on a fine one close by, the medium ones,
// and the big shrubs on a coarse one far out. must match GroundCover::SIDE
#define GC_RADIUS_SMALL 3.5 // ~21m
#define GC_RADIUS_FAR 25.0	// ~150m
const float GC_CELL[3] = float[](0.05, 0.15, 0.3); // world units (~0.3m, ~0.9m, ~1.8m)
const float GC_RADIUS[3] = float[](GC_RADIUS_SMALL, 10.0, GC_RADIUS_FAR);
const uint GC_SIDE[3] = uint[](140u, 133u, 166u); // 2 * radius / cell
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
