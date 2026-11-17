// shared between groundcover.comp / .vert / .frag: ferns, bushes, weeds, nettles, dead branches (Poly Haven CC0
// models in assets/plants) scattered around the camera between the grass and the trees
#ifndef GROUNDCOVER_COMMON_GLSL
#define GROUNDCOVER_COMMON_GLSL

#include "foliagecommon.glsl" // the same push constants as the grass (FoliagePushConstants), pc.interaction = density
#include "groundcoverspecies.glsl" // the species, grids and structs (no push constants: trees_place.comp shares them)

#endif
