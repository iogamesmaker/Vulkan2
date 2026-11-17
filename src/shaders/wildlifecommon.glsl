// shared between wildlife.vert / wildlife.frag (wildlife.hpp)
#ifndef WILDLIFE_COMMON_GLSL
#define WILDLIFE_COMMON_GLSL

// WildlifePushConstants
layout(push_constant) uniform constants {
	mat4 viewproj;
	vec3 campos; // terrain space
	float time;
	vec2 offsetWorld; // world units between terrain space and the continuous world
	ivec2 offset;	  // heightmap texel offset
	float terrainSize;
	float factor;		 // terrain height scale
	float unitsPerMeter; // world units per metre
	float projScale;	 // pixels per world unit at distance 1
	float previousTime;
	float sunHeight; // the sun direction's y
	float calm;		 // 1 fine weather .. 0 storm
	float pad;
} pc;

// what the fragment shader paints
#define CRITTER_BIRD 0		// flat colours: outColour on top, outColour2 underneath
#define CRITTER_BUTTERFLY 1 // a wing outline + pattern, outColour / outColour2 its two colours, outPattern the species

// must match Wildlife
#define FLOCK_GRID 9
#define BIRDS_PER_FLOCK 24
#define BIRD_VERTICES 39 // the butterflies use the first 18
#define BUTTERFLY_GRID 14
#define BIRDS (FLOCK_GRID * FLOCK_GRID * BIRDS_PER_FLOCK)

// one bird / bat / butterfly this frame (wildlife.comp -> wildlife.vert)
struct Critter {
	vec4 position; // xyz terrain space, w = scale (world units per mesh unit, 0 = not there)
	vec4 previous; // xyz last frame, w = CRITTER_BIRD / CRITTER_BUTTERFLY
	vec4 right;	   // xyz the body frame's right axis, w = wing chord (a bird's wing depth)
	vec4 up;	   // xyz its up axis (forward = cross(right, up)), w = neck length
	vec4 wings;	   // xy the wing joints' angles now, zw last frame
	vec4 colour;   // rgb top / the wings, w = butterfly pattern
	vec4 colour2;  // rgb underside / the pattern's second colour, w = how much thicker it's drawn (far birds)
};

#endif
