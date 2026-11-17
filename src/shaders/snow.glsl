// deep snow (SnowTrail, snowtrail.hpp): the mountains' closed snow cover lies ~45cm deep on the ground and the
// tessellated terrain mesh is raised by it (terrain.tese), softening the ground's small bumps under it. where you
// walk your feet press prints into it and your legs plough a shallow furrow (snowstamp.comp records them in a
// world anchored, toroidal texture around you), each with a little rim of pushed up snow. terrain.frag bends the
// normals into them and greys the packed snow a little
// the includer declares the heightmap (binding 0) and the climate map (binding 4) of the terrain set
// define SNOW_SET / SNOW_BINDING (the info, the trail texture at SNOW_BINDING + 1) before including this
#ifndef SNOW_GLSL
#define SNOW_GLSL

#include "biometools.glsl"
#include "heightmapscale.glsl"

layout(std140, set = SNOW_SET, binding = SNOW_BINDING) uniform SnowInfo {
	vec4 camera;   // xy = the camera in trail texels, modulo the texture's size (fraction included), z = texels per world unit, w = 1 when on
	vec4 window;   // xy = the camera's texel minus the window's centre (the texels around it that hold prints), z = the texture's size, w = world units per metre
} snowInfo;
layout(set = SNOW_SET, binding = SNOW_BINDING + 1) uniform sampler2D snowTrail; // r = pressed down (prints, furrow), g = the rim's reach, repeats

#define SNOW_DEPTH_METRES 0.45 // knee deep on the closed cover
#define SNOW_PRINT_DEPTH 0.8   // how much of the snow a print presses down (to near the ground)
#define SNOW_RIM_HEIGHT 0.18   // the rim around a print, of the snow's depth

// the snow's depth (world units) on the ground at heightmap uv: the closed cover above the snowline on the mountains
// (terrainmaterial.glsl's snowline, the steep faces shed it), nothing on its patchy fringe, where the texture still
// shows rock and grass between the snow. climate = the climate map's rgb there, height = the heightmap's (0..1)
float snowDepth(vec2 uv, vec3 climate, float height) {
	if (snowInfo.camera.w < 0.5) return 0.0;
	float mountains = mountainWeight(climate);
	if (mountains <= 0.01) return 0.0;
	// terrainmaterial.glsl's snowline: its noise, and lower on the shaded north faces
	float lineNoise = latticeNoise(uv * 180.0) * 0.65 + latticeNoise(uv * 900.0) * 0.35;
	float snowline = snowlineHeight(climate) + (lineNoise - 0.5) * 0.08;
	if (height < snowline - 0.08) return 0.0; // (the north faces' shift can't reach this far)
	// the slope (tan^2) and which way it faces, from the heightmap's neighbours
	vec2 size = vec2(textureSize(heightmap, 0));
	float hR = textureLod(heightmap, uv + vec2(1.0, 0.0) / size, 0.0).a, hL = textureLod(heightmap, uv - vec2(1.0, 0.0) / size, 0.0).a;
	float hT = textureLod(heightmap, uv + vec2(0.0, 1.0) / size, 0.0).a, hB = textureLod(heightmap, uv - vec2(0.0, 1.0) / size, 0.0).a;
	float dzdx = (hR - hL) * heightmapSlopeScale(size.x), dzdy = (hT - hB) * heightmapSlopeScale(size.x);
	float slope = dzdx * dzdx + dzdy * dzdy;
	snowline -= -dzdy / sqrt(1.0 + slope) * 0.035;
	// how the texture's snow lies (its snowHeight, deepSnow and snowShed), deep where it's closed, none where it's patchy
	float snowHeight = smoothstep(snowline - 0.03, snowline + 0.06, height);
	float deep = smoothstep(snowline + 0.04, snowline + 0.16, height);
	float shed = smoothstep(mix(0.7, 1.2, deep), mix(1.4, 2.4, deep), slope + (lineNoise - 0.5) * 0.25);
	float cover = smoothstep(0.35, 0.75, snowHeight * (1.0 - shed * 0.85));
	return cover * smoothstep(0.3, 0.7, mountains) * SNOW_DEPTH_METRES * snowInfo.window.w;
}
float snowDepthAt(vec2 uv) {
	return snowDepth(uv, textureLod(climateMap, uv, 0.0).rgb, textureLod(heightmap, uv, 0.0).a);
}

// the trail texture at a point rel (terrain space, world units) from the camera: r = pressed, g = rim. 0 outside the
// window it covers (texelOffset: in trail texels, for the normal's differences)
vec2 snowTrailAt(vec2 rel, vec2 texelOffset) {
	vec2 texel = rel * snowInfo.camera.z + texelOffset;
	vec2 fromCentre = snowInfo.window.xy + texel;
	float half_ = snowInfo.window.z * 0.5;
	// (fades out over the last texels of the window: the strips there are cleared as it moves on)
	float inside = 1.0 - smoothstep(half_ - 24.0, half_ - 8.0, max(abs(fromCentre.x), abs(fromCentre.y)));
	if (inside <= 0.0) return vec2(0.0);
	return textureLod(snowTrail, (snowInfo.camera.xy + texel) / snowInfo.window.z, 0.0).rg * inside;
}

// the surface's offset from the snow's own depth for what the trail holds (world units, <= 0 in a print, a little
// above it on its rim)
float snowTrailOffset(vec2 trail, float depth) {
	return depth * (SNOW_RIM_HEIGHT * clamp(trail.g - trail.r, 0.0, 1.0) - SNOW_PRINT_DEPTH * trail.r);
}

#endif
