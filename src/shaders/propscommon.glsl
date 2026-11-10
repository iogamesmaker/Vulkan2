// shared between props.comp / props.vert: rocks, boulders and fallen logs scattered around the camera
#ifndef PROPS_COMMON_GLSL
#define PROPS_COMMON_GLSL

#include "foliagecommon.glsl" // the same push constants as the grass (FoliagePushConstants)

// meshes (photoscans, Props::loadMesh): PROP_ROCKS rocks (also scaled up into boulders), the logs, the stumps
// (propplacement.glsl). every mesh has a detailed and a cheap LOD
#define PROP_MESHES 15 // Props::MESHES: 6 rocks, 2 logs, 2 stumps
// bins: mesh * 2 + lod for the camera, then PROP_MESHES shadow caster bins (cheap LOD), every bin PROP_CAPACITY
#define PROP_BINS (PROP_MESHES * 3)
#define PROP_CAPACITY 4096u

// PROP_CELL: propplacement.glsl
#define PROP_RADIUS 160.0  // ~1km
#define PROP_LOD_PIXELS 70.0 // the detailed LOD above this radius on screen (props.comp)
#define PROP_SHADOW_DISTANCE 90.0

struct PropInstance {
	vec4 posScale; // xyz terrain space, w = size in world units
	vec4 params;   // x = yaw, y = tilt (logs lie on the slope), z = seed, w = moss (how wet / shaded it is)
};

#endif
