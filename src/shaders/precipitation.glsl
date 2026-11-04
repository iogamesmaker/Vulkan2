// shared by precipitation.vert / .frag: what's falling (weather.cpp), drawn after the TAA onto the window resolution
// output (no ghosting of the streaks), depth tested by hand against the render resolution depth buffer
#ifndef PRECIPITATION_GLSL
#define PRECIPITATION_GLSL

#define KIND_RAIN 0
#define KIND_SNOW 1
#define KIND_SAND 2

layout(push_constant) uniform constants {
	mat4 viewProj;	 // unjittered, terrain space
	vec3 campos;
	float time;
	vec2 offsetWorld; // terrain space -> continuous world (the particles are world anchored)
	vec2 wind;		 // world units / s, horizontal
	ivec4 counts;	 // instances: x rain, y snow, z sand (in that order)
	vec4 view;		 // xy = output size (pixels), z = projScale (pixels per world unit at distance 1), w = 0
	vec3 sundir;
	float pad;
	vec4 motion;	 // xyz = the camera's velocity (world units / s): streaks are what the drops do relative to you
} pc;

#endif
