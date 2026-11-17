// shared by precipitation.vert / .frag: what's falling (weather.cpp), drawn after the TAA onto the window resolution
// output (no ghosting of the streaks), depth tested by hand against the render resolution depth buffer
#ifndef PRECIPITATION_GLSL
#define PRECIPITATION_GLSL

#define KIND_RAIN 0
#define KIND_SNOW 1
#define KIND_SAND 2
#define KIND_LEAF 3
#define KIND_MOTE 4	 // pollen, seed fluff, dust in the air: drifting with the wind, glinting against the sun
// (5 was dust puffs: they popped in and out with the gusts and had no weight to them, removed)
#define KIND_DRIFT 6 // spindrift: snow streaming low off the ground in the wind

layout(push_constant) uniform constants {
	mat4 viewProj;	 // unjittered, terrain space
	vec3 campos;
	float time;
	vec2 offsetWorld; // terrain space -> continuous world (the particles are world anchored)
	vec2 wind;		 // world units / s, horizontal
	ivec4 counts;	 // instances: x rain, y snow, z sand, w leaves (in that order)
	vec4 view;		 // xy = output size (pixels), z = projScale (pixels per world unit at distance 1), w = 0
	vec3 sundir;
	float standingEye; // where your eyes are standing on the ground under you: the height the near-ground ones go by
	vec4 motion;	 // xyz = the camera's velocity (world units / s): streaks are what the drops do relative to you
	ivec4 counts2;	 // after counts' kinds: x motes, y unused (0), z spindrift
} pc;

#endif
