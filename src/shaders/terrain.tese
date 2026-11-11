#version 450
// tessellation evaluation

layout (quads, fractional_odd_spacing, cw) in; // fractional so LOD changes morph instead of popping

layout(location = 0) in vec3 inPosition[];
layout(location = 1) in vec2 inUV[];
layout(location = 5) flat in int inLOD[];

// the depth prepass and the shading pass (depth EQUAL) must produce bit identical depths
invariant gl_Position;

layout(location = 0) out vec3 outPosition;
layout(location = 1) out vec2 outUV;
layout(location = 5) flat out int outLOD;

layout(set = 0, binding = 0) uniform sampler2D heightmap;

#include "terrainheight.glsl"

layout(push_constant) uniform constants {
	mat4 viewproj;
	vec3 campos;
	float tessellationFactor;
	vec3 sundir;
	float factor;
	ivec2 offset;
	float projScale;
	float worldRadius;
	float fixedTessLevel; // > 0: shadow pass, ignore the camera based LOD
	float time;
	vec2 pad1;
} pc;

void main() {
	float u = gl_TessCoord.x;
	float v = gl_TessCoord.y;

	vec4 p0 = mix(gl_in[0].gl_Position, gl_in[1].gl_Position, u);
	vec4 p1 = mix(gl_in[2].gl_Position, gl_in[3].gl_Position, u);
	vec4 pos = mix(p0, p1, v);

	vec2 uv0 = mix(inUV[0], inUV[1], u);
	vec2 uv1 = mix(inUV[2], inUV[3], u);
	outUV = mix(uv0, uv1, v);

	outLOD = int(round(mix(mix(inLOD[0],inLOD[1],u),mix(inLOD[2], inLOD[3],u), v)));

	vec2 size = vec2(textureSize(heightmap, 0));
	vec2 readCoord = outUV + vec2(pc.offset) / size;

	// prefilter to roughly the triangle size at this distance, otherwise coarse LODs point sample tiny details and
	// ridges zigzag. only depends on the vertex position so shared edges match. foliage / trees use the same function
	float dist = distance(pos.xz, pc.campos.xz);
	float patchWorld = distance(gl_in[0].gl_Position.xz, gl_in[1].gl_Position.xz);
	float terrainSize = patchWorld / distance(inUV[0], inUV[1]);
	// shadow pass: the triangle size from terrain.tesc's shadowTessFactor at this vertex, not outLOD. outLOD is
	// blended across the patch from its four edges' levels, so two patches worked out different spacings (and so
	// different prefiltered heights) along the edge they share: cracks in the shadow maps, light through the ground
	bool shadowPass = pc.fixedTessLevel > 0.0;
	float shadowLevel = clamp(pc.fixedTessLevel * pc.sundir.y / max(distance(pos.xz, pc.sundir.xz), pc.sundir.y), 1.0, 64.0);
	float spacing = shadowPass ? patchWorld / shadowLevel : terrainSpacing(dist, 40.0 / max(pc.tessellationFactor, 0.1), pc.projScale);
	pos.y = terrainHeight(heightmap, readCoord, dist, spacing, pc.factor, terrainSize);
	// the caster mesh is coarser than what's on screen: across a crevice or a dip its flat triangles sit above the
	// real surface (and the prefilter fills it in), so the floor shadowed itself with the sun shining right in.
	// lowered a little with the triangle size. a heightfield has no overhangs, this only trims a shadow's very start
	// in a concave place (a gully, a bowl, the foot of a slope) the flat caster triangles bow above the real surface
	// by their sag, and with the sun grazing the slope each one shaded a long streak down it: stair stepped bands. the
	// sag from the curvature (the height's second difference one triangle either way, along x, z and both
	// diagonals): a chord over a curve k sits k s^2 / 8 above its middle, the second difference is k s^2 / 2
	if (shadowPass) {
		vec2 du = vec2(spacing / terrainSize, 0.0), dv = vec2(0.0, spacing / terrainSize);
		float hx = terrainHeight(heightmap, readCoord + du, dist, spacing, pc.factor, terrainSize) + terrainHeight(heightmap, readCoord - du, dist, spacing, pc.factor, terrainSize);
		float hz = terrainHeight(heightmap, readCoord + dv, dist, spacing, pc.factor, terrainSize) + terrainHeight(heightmap, readCoord - dv, dist, spacing, pc.factor, terrainSize);
		float hd = terrainHeight(heightmap, readCoord + du + dv, dist, spacing, pc.factor, terrainSize) + terrainHeight(heightmap, readCoord - du - dv, dist, spacing, pc.factor, terrainSize);
		float he = terrainHeight(heightmap, readCoord + du - dv, dist, spacing, pc.factor, terrainSize) + terrainHeight(heightmap, readCoord - du + dv, dist, spacing, pc.factor, terrainSize);
		float concave = max(max(hx, hz), max(hd, he)) * 0.5 - pos.y; // (the diagonals are a longer edge: their sag counts double, as it should)
		pos.y -= max(concave, 0.0) * 0.4 + min(spacing * 0.03, 0.06);
	}

	outPosition = pos.xyz;

	gl_Position = pc.viewproj * pos;
}
