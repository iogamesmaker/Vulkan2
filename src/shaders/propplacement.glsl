// which rock / boulder / log sits in a placement cell (world anchored, a pure function of the cell), shared by the
// scatter (props.comp) and the walking probe (probe.comp, collisions), so you collide with exactly what's drawn.
// include biometools.glsl first
#ifndef PROP_PLACEMENT_GLSL
#define PROP_PLACEMENT_GLSL

#define PROP_CELL 0.9 // world units (~5m) per placement cell

// mesh space collision shapes (times the scale), see Props::buildRock / buildLog
#define ROCK_RADII vec3(0.55, 0.36, 0.47) // an ellipsoid around the lumpy rock
#define ROCK_SINK 0.15					  // rocks sit this far (times the scale) into the ground
#define LOG_RADIUS 0.055 // ~0.6m thick at the average scale
#define LOG_SINK 0.05
#define STUMP_RADIUS 0.06 // Props::buildStump, before the root flare
#define STUMP_HEIGHT 0.1

struct PropPlacement {
	vec2 xz;	 // terrain space
	vec2 uv;	 // heightmap uv
	uint mesh;	 // 0..3 rocks, 4..5 logs, 6..7 stumps
	float scale; // world units
	float yaw, tilt, seed, moss;
	float height; // raw heightmap height (0..1), a rough idea of the altitude
};

vec4 propHash42(vec2 p) {
	vec4 p4 = fract(vec4(p.xyxy) * vec4(0.1031, 0.1030, 0.0973, 0.1099));
	p4 += dot(p4, p4.wzxy + 33.33);
	return fract((p4.xxyz + p4.yzzw) * p4.zywx);
}

bool placeProp(sampler2D hm, sampler2D climate, vec2 cellIndex, vec2 offsetWorld, ivec2 offset, float terrainSize, float factor, out PropPlacement p) {
	vec4 rnd = propHash42(cellIndex * 1.71 + 3.3);
	vec4 rnd2 = propHash42(cellIndex * 0.37 - 9.1);
	p.xz = (cellIndex + 0.1 + rnd.xy * 0.8) * PROP_CELL - offsetWorld;

	vec2 size = vec2(textureSize(hm, 0));
	p.uv = (p.xz + 0.5 * terrainSize) / terrainSize + vec2(offset) / size;
	vec4 data = vec4(textureLod(climate, p.uv, 0.0).rgb, textureLod(hm, p.uv, 0.0).a); // climate (half resolution), height
	float hR = textureLodOffset(hm, p.uv, 0.0, ivec2(1, 0)).a, hL = textureLodOffset(hm, p.uv, 0.0, ivec2(-1, 0)).a;
	float hT = textureLodOffset(hm, p.uv, 0.0, ivec2(0, 1)).a, hB = textureLodOffset(hm, p.uv, 0.0, ivec2(0, -1)).a;
	float slope = pow((hR - hL) * heightmapSlopeScale(size.x), 2.0) + pow((hT - hB) * heightmapSlopeScale(size.x), 2.0); // the same measure terrain.frag uses

	float weights[BIOME_COUNT];
	groundCover(data.rgb, true, weights);
	float mountains = mountainWeight(data.rgb);
	float rocky = max(smoothstep(0.3, 0.8, slope), mountains * smoothstep(0.5, 0.6, data.a));
	float forest = weights[BIOME_TAIGA] + weights[BIOME_PLAINS] * smoothstep(0.6, 0.78, data.g) * 0.6;

	// boulders on the mountains, rocks (scree on steep ground, a scattering in meadows / forests), logs in the
	// forests. the chances are per cell
	float boulder = rocky * 0.035 + mountains * 0.01;
	float rock = rocky * 0.35 + 0.02 + weights[BIOME_DESERT] * 0.04;
	float fallenLog = forest * 0.05 * (1.0 - rocky);
	float stump = forest * 0.035 * (1.0 - rocky);
	float roll = rnd.z;
	if (roll < boulder) { p.mesh = uint(rnd.w * 4.0); p.scale = mix(0.5, 1.4, rnd2.x); }					 // ~3-8m
	else if (roll < boulder + fallenLog) { p.mesh = 4u + uint(rnd.w * 2.0); p.scale = mix(0.9, 1.6, rnd2.x); } // logs, ~5-10m
	else if (roll < boulder + fallenLog + rock) { p.mesh = uint(rnd.w * 4.0); p.scale = mix(0.06, 0.32, rnd2.x * rnd2.x); } // ~0.4-2m
	else if (roll < boulder + fallenLog + rock + stump) { p.mesh = 6u + uint(rnd.w * 2.0); p.scale = mix(0.8, 1.6, rnd2.x); } // ~0.6-1m tall
	else return false;
	if (p.mesh < 4u && slope > 1.5 && p.scale > 0.4) return false; // nothing big sits on a cliff face
	// small rocks don't show on snow fields (buried): scattered dark pebbles all over the snow looked like pepper
	float snowline = mix(0.44, 0.68, data.r); // terrain.frag's, without its noise
	float snowy = smoothstep(snowline - 0.03, snowline + 0.06, data.a) * mountains * (1.0 - smoothstep(0.5, 1.0, slope));
	if (p.mesh < 4u && p.scale < 0.4 && snowy > 0.35) return false;

	p.yaw = rnd2.z * 6.2831853;
	// logs lie along the slope under their own axis (the yaw turns the mesh's x axis to (cos, -sin) in xz)
	vec2 gradient = vec2(hR - hL, hT - hB) * factor / (2.0 * terrainSize / size.x); // height per world unit
	float along = dot(gradient, vec2(cos(p.yaw), -sin(p.yaw)));
	p.tilt = p.mesh >= 6u ? 0.0 : (p.mesh >= 4u ? atan(along) : (rnd2.y - 0.5) * 0.4); // stumps stand upright
	p.seed = rnd2.w;
	p.moss = clamp(data.g * 1.3 - 0.3 + weights[BIOME_TAIGA] * 0.3, 0.0, 1.0) * (1.0 - weights[BIOME_DESERT]) * (1.0 - weights[BIOME_SNOW]);
	p.height = data.a;
	return true;
}

// the rotation props.vert applies (tilt about the mesh's z axis, then the yaw)
mat3 propRotation(float yaw, float tilt) {
	float ct = cos(tilt), st = sin(tilt), cy = cos(yaw), sy = sin(yaw);
	return mat3(cy, 0.0, -sy, 0.0, 1.0, 0.0, sy, 0.0, cy) * mat3(ct, st, 0.0, -st, ct, 0.0, 0.0, 0.0, 1.0);
}

#endif
