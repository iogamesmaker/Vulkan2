// shared between groundcover.comp / .vert / .frag: ferns, bushes, weeds, nettles, dead branches (Poly Haven CC0
// models in assets/plants) scattered around the camera between the grass and the trees
#ifndef GROUNDCOVER_COMMON_GLSL
#define GROUNDCOVER_COMMON_GLSL

#include "foliagecommon.glsl" // the same push constants as the grass (FoliagePushConstants), pc.interaction = density

// must match GroundCover in groundcover.hpp
#define GC_SPECIES 20
#define GC_MAX_MESHES 128
#define GC_CAPACITY 3072u // (more meshes, fewer per bin: the same ~38MB of instances)
#define GC_LOD2_PIXELS 12.0 // and the much simpler LOD 2 below this one
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
// Poly Haven (CC0), more variety in the woods and meadows
#define GC_MOSS 14		 // cushions of moss on the forest floor
#define GC_CELANDINE 15	 // lesser celandine: glossy leaves, yellow star flowers, damp shade and forest edges
#define GC_SHRUB_FLOWER 16 // a leafy flowering shrub
#define GC_SHRUB_GREEN 17	 // a dense green bush
#define GC_GRASS_CLUMP 18	 // tall grass tussocks
#define GC_ROOTS 19		 // a tangle of exposed roots and old wood

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
