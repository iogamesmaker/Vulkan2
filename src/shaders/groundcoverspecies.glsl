// the ground cover's species, grids and instance / species structs, without the push constants: groundcover.comp
// (through groundcovercommon.glsl) and trees_place.comp (the shrubs it places for the ground cover, groundcoverpick.glsl)
#ifndef GROUNDCOVER_SPECIES_GLSL
#define GROUNDCOVER_SPECIES_GLSL

// must match GroundCover in groundcover.hpp
#define GC_FILES 36	// the models (their textures), GroundCover::FILES
#define GC_SPECIES 40 // + the looks below, made from those models (GroundCover::SPECIES)
#define GC_MAX_MESHES 256
#define GC_TEXTURE_PAIRS 64 // GroundCover::MAX_TEXTURE_PAIRS
#define GC_CAPACITY 1792u // (more meshes, fewer per bin: the same ~38MB of instances)
#define GC_LOD2_PIXELS 18.0 // and the much simpler LOD 2 below this one
#define GC_LOD_PIXELS 60.0 // the full model above this radius on screen, the simplified one below (groundcover.comp)
#define GC_SMALL_LOD_SCALE 1.7 // the small plants (flowers, clover, weeds, moss, mushrooms...) switch at this times the size:
							   // their detail is a few leaves, the full model of a whole meadow of them was most of the cost
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
#define GC_SHRUB_FLOWER 16 // lilac bushes, 2-5m (LOLIPOP, CC-BY): drawn as trees (GC_TREE_KIND)
#define GC_SHRUB_GREEN 17	 // a dense green bush, ~1.5m (LOLIPOP's bush pack, CC-BY): drawn as trees
#define GC_GRASS_CLUMP 18	 // tall grass tussocks
#define GC_ROOTS 19		 // a tangle of exposed roots and old wood
// Poly Haven (CC0), more again
#define GC_DAISY 20		 // daisies, single and in patches, drifts in the meadows (LOLIPOP)
#define GC_GRASS_TALL 21	 // another tall grass, finer than the tussocks
#define GC_BERMUDA 22		 // low creeping grass, dry ground, the desert's edge
#define GC_ICEPLANT 23		 // a crystalline succulent mat, the desert and dry stony ground
#define GC_SUCCULENT 24		 // cheiridopsis: fat succulent clumps in the desert
#define GC_QUIVER_BRANCH 25	 // a dead, bleached branch lying in the desert
#define GC_MUSHROOMS 26		 // scanned mushrooms (Loic Norgeot, CC-BY): in patches on the damp forest floor
#define GC_CACTUS 27		 // single cacti, 1-5m, the desert (LOLIPOP): drawn as trees
#define GC_CACTUS_CLUMP 28	 // clusters of cacti, the desert
#define GC_QUIVER_BRANCH2 29 // another dead branch in the desert
#define GC_UNDERGROWTH 30	 // dandelion, wild lettuce and plantain rosettes in the meadows (LOLIPOP)
#define GC_GRASS_TUFT 31	 // low poly grass tufts, fresh / dry / tall (LOLIPOP's grass packs)
#define GC_ROOT_CLUSTER2 32	 // a cluster of exposed roots, forest
#define GC_LILAC_SAPLING 33 // young lilacs, ~2m, forest edges and meadows: drawn as trees
#define GC_BUSH_SMALL 34	 // small and medium bushes, 0.5-1m (LOLIPOP's bush pack)
#define GC_LILAC_TREE 35	 // small lilac trees, ~7m, forest edges: drawn as trees
// the same models again with a look of their own (GroundCover's VARIANT_OF: the mesh, its colouring in groundcover.frag)
#define GC_BRACKEN 36		 // the fern, big, in yellow green / rusty stands at forest edges and on open slopes
#define GC_AUTUMN_SHRUB 37	 // shrub_02 turned red / orange, the odd one at a forest edge
#define GC_DRY_TUSSOCK 38	 // the grass clump gone to straw, in dry meadows and between the rocks
#define GC_SEDGE 39			 // the grass clump dark blue green, in wet ground
// the look, in the instance's tint (+ 2 * look, groundcover.frag decodes it)
#define GC_LOOK_NONE 0
#define GC_LOOK_BRACKEN 1
#define GC_LOOK_AUTUMN 2
#define GC_LOOK_STRAW 3
#define GC_LOOK_SEDGE 4

// slots with no model (GroundCover's "" files that aren't drawn as trees either): never picked. bits by GC_* (x: 0-31,
// y: 32-). (root_cluster_01, GC_ROOTS, never loaded)
const uvec2 GC_EMPTY_SLOTS = uvec2(1u << GC_ROOTS, 0u);
bool gcEmptySlot(int kind) { return ((kind < 32 ? GC_EMPTY_SLOTS.x >> kind : GC_EMPTY_SLOTS.y >> (kind - 32)) & 1u) != 0u; }

// the big ones the tree pipeline draws (Trees, trees_place.comp: their LODs, impostors and shadows), tools/import_trees.py's
// shrub packs. the ground cover still picks them (groundcoverpick.glsl, so everything else grows exactly where it did)
// but places nothing there, trees_place.comp makes the same pick and places the tree. -1: the ground cover's own.
// the kind is treemeshes.glsl's SHRUB_*
int gcTreeKind(int kind) {
	if (kind == GC_LILAC_TREE) return 0;	// SHRUB_LILAC_TREE
	if (kind == GC_SHRUB_FLOWER) return 1;	// SHRUB_LILAC_BUSH
	if (kind == GC_LILAC_SAPLING) return 2; // SHRUB_LILAC_SAPLING
	if (kind == GC_SHRUB_GREEN) return 3;	// SHRUB_BUSH_BIG
	if (kind == GC_CACTUS) return 4;		// SHRUB_CACTUS
	return -1;
}

// three grids (consecutive thread ranges in groundcover.comp): small plants on a fine one close by, the medium ones,
// and the big shrubs on a coarse one far out. must match GroundCover::SIDE
#define GC_RADIUS_SMALL 3.5 // ~21m
#define GC_RADIUS_FAR 25.0	// ~150m
const float GC_CELL[3] = float[](0.05, 0.15, 0.3); // world units (~0.3m, ~0.9m, ~1.8m)
#define GC_MEDIUM_REACH 1.3 // the medium grid's plants (ferns, bushes, nettles, flowers...) reach this much further than
							// groundcover.comp's table says, ~78m instead of ~60m (they're on the simplified LODs out there)
const float GC_RADIUS[3] = float[](GC_RADIUS_SMALL, 13.0, GC_RADIUS_FAR);
const uint GC_SIDE[3] = uint[](140u, 174u, 166u); // 2 * radius / cell
#define GC_SHADOW_DISTANCE 3.0 // ~18m, the shrubs twice that (with their own ~12% LOD, GroundCover::init)

struct GroundCoverInstance {
	vec4 posScale; // xyz terrain space, w = scale (model metres -> world units, the variation included)
	vec4 params;   // x = yaw, y = tint, z = seed, w = how much it sways
};

struct GroundCoverSpecies {
	uvec4 meshes; // x = first mesh, y = mesh count
	vec4 size;	  // x = the tallest / widest variant's radius in metres
};

#endif
