// shared between props.comp / props.vert: rocks, boulders and fallen logs scattered around the camera
#ifndef PROPS_COMMON_GLSL
#define PROPS_COMMON_GLSL

#include "foliagecommon.glsl" // the same push constants as the grass (FoliagePushConstants)

// meshes (photoscans, Props::loadMesh): PROP_ROCKS rocks (also scaled up into boulders), the logs, the stumps
// (propplacement.glsl). every mesh has a detailed and a cheap LOD
#define PROP_MESHES 15 // Props::MESHES: 6 rocks, 2 logs, 2 stumps
// bins: mesh * 2 + lod for the camera (LOD 0 / 1), then PROP_MESHES shadow caster bins (cheap LOD), then
// PROP_MESHES bins for the camera's LOD 2 (the speck), every bin PROP_CAPACITY
#define PROP_BINS (PROP_MESHES * 4)
#define PROP_CAPACITY 4096u

// PROP_CELL: propplacement.glsl
#define PROP_RADIUS 160.0  // ~1km
#define PROP_LOD_PIXELS 70.0 // the detailed LOD above this radius on screen (props.comp)
#define PROP_LOD2_PIXELS 16.0 // the cheapest below this one
#define PROP_SHADOW_DISTANCE 90.0

struct PropInstance {
	vec4 posScale; // xyz terrain space, w = size in world units
	vec4 params;   // x = yaw, y = tilt (logs lie on the slope), z = seed + 1 + its dither range (propDitherPack), w = moss (how wet / shaded it is)
};

// the LOD hand over: across a band around each switch both LODs are drawn, each keeping the pixels whose dither
// value (props.frag, different every frame, TAA averages it) falls in its part of [0, 1), so one fades into the other
// instead of popping. the far edge (a speck, a small rock's reach) fades out the same way. the range [a, b) goes
// into the seed's integer part in PROP_DITHER_LEVELS steps (the seed is a fraction, it gets the fraction back)
#define PROP_DITHER_LEVELS 16.0
float propDitherPack(float seed, float a, float b) {
	float qa = floor(clamp(a, 0.0, 1.0) * PROP_DITHER_LEVELS + 0.5), qb = floor(clamp(b, 0.0, 1.0) * PROP_DITHER_LEVELS + 0.5);
	return fract(seed) + 1.0 + qa * (PROP_DITHER_LEVELS + 1.0) + qb;
}
// -> xy = [a, b), z = the seed
vec3 propDitherUnpack(float packed) {
	float code = floor(packed) - 1.0;
	float qa = floor(code / (PROP_DITHER_LEVELS + 1.0));
	return vec3(qa, code - qa * (PROP_DITHER_LEVELS + 1.0), PROP_DITHER_LEVELS * fract(packed)) / PROP_DITHER_LEVELS;
}

#endif
