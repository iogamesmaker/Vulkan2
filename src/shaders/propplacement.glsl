// which rock / boulder / log sits in a placement cell (world anchored, a pure function of the cell), shared by the
// scatter (props.comp) and the walking probe (probe.comp, collisions), so you collide with exactly what's drawn.
// include biometools.glsl first
#ifndef PROP_PLACEMENT_GLSL
#define PROP_PLACEMENT_GLSL

#define PROP_CELL 0.9 // world units (~5m) per placement cell

#include "propmeshes.glsl" // rocks, then logs, then stumps
#include "proprotation.glsl"

// mesh space collision shapes (times the scale), measured from the scans in the space Props::loadMesh puts them in.
// rocks: an ellipsoid per rock, x / z half extents and the height above their base (they stand on y = 0, the
// ellipsoid's centre is the base, its lower half underground)
const vec3 ROCK_RADII[PROP_ROCKS] = vec3[](vec3(0.382, 0.603, 0.55), vec3(0.29, 0.494, 0.55), vec3(0.281, 0.25, 0.55),
										   vec3(0.55, 0.384, 0.387), vec3(0.55, 0.513, 0.503), vec3(0.55, 0.412, 0.342));
#define ROCK_SINK 0.06 // rocks sit this far (times the scale) into the ground, over uneven ground too
// logs (along x, -0.5 .. 0.5): the trunk's radius and the height of its axis above the ground
const float LOG_RADII[PROP_LOGS] = float[](0.033, 0.051);
const float LOG_AXIS[PROP_LOGS] = float[](0.039, 0.169);
#define LOG_SINK 0.01
#define LOG_HALF_LENGTH 0.45 // the log meshes run -0.5..0.5 along x (Props::loadMesh), the ends taper
// stumps: the trunk's radius (before the roots flare out) and their height
const float STUMP_RADII[PROP_STUMPS] = float[](0.08, 0.089);
#define STUMP_HEIGHT 0.1

struct PropPlacement {
	vec2 xz;	 // terrain space
	vec2 uv;	 // heightmap uv
	uint mesh;	 // rocks, logs, stumps (PROP_FIRST_LOG / PROP_FIRST_STUMP)
	float scale; // world units
	float yaw, tilt, seed, moss; // moss: how wet / shaded it is, negative = snow lying on it (they never come together)
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
	// logs and stumps wherever the trees grow (the trees' own chances, mountain forests below the treeline too: a
	// taiga / wet meadow rule of its own left the forests on the slopes without any)
	vec3 trees = treeChances(data, 0.0, 1.0);
	float forest = smoothstep(0.08, 0.45, trees.x + trees.y);

	// boulders on the mountains, rocks (scree on steep ground, a scattering in meadows / forests), logs in the
	// forests. the chances are per cell
	float boulder = rocky * 0.035 + mountains * 0.01;
	float rock = rocky * 0.35 + 0.02 + weights[BIOME_DESERT] * 0.04;
	float fallenLog = forest * 0.05 * (1.0 - rocky);
	float stump = forest * 0.05 * (1.0 - rocky);
	float roll = rnd.z;
	if (roll < boulder) { p.mesh = min(uint(rnd.w * float(PROP_ROCKS)), uint(PROP_ROCKS - 1)); p.scale = mix(0.5, 1.4, rnd2.x); } // ~3-8m
	else if (roll < boulder + fallenLog) { p.mesh = PROP_FIRST_LOG + min(uint(rnd.w * float(PROP_LOGS)), uint(PROP_LOGS - 1)); p.scale = mix(0.6, 1.1, rnd2.x); } // logs, ~3.5-6.5m
	else if (roll < boulder + fallenLog + rock) { p.mesh = min(uint(rnd.w * float(PROP_ROCKS)), uint(PROP_ROCKS - 1)); p.scale = mix(0.06, 0.32, rnd2.x * rnd2.x); } // ~0.4-2m
	else if (roll < boulder + fallenLog + rock + stump) { p.mesh = PROP_FIRST_STUMP + min(uint(rnd.w * float(PROP_STUMPS)), uint(PROP_STUMPS - 1)); p.scale = mix(0.9, 1.7, rnd2.x); } // ~0.55-1m tall (shorter ones vanished in the ferns and grass)
	else return false;
	if (p.mesh < PROP_FIRST_LOG && slope > 1.5 && p.scale > 0.4) return false; // nothing big sits on a cliff face
	// small rocks don't show on snow fields (buried): scattered dark pebbles all over the snow looked like pepper
	float snowline = snowlineHeight(data.rgb); // terrain.frag's, without its noise
	float snowy = smoothstep(snowline - 0.03, snowline + 0.06, data.a) * mountains * (1.0 - smoothstep(0.5, 1.0, slope));
	if (p.mesh < PROP_FIRST_LOG && p.scale < 0.4 && snowy > 0.35) return false;

	p.yaw = rnd2.z * 6.2831853;
	// logs lie along the slope under their own axis (the yaw turns the mesh's x axis to (cos, -sin) in xz)
	vec2 gradient = vec2(hR - hL, hT - hB) * factor / (2.0 * terrainSize / size.x); // height per world unit
	// and every way across it: on the ground's normal (packTilt). rocks most of the way plus a random wobble,
	// logs all the way, stumps (a tree's foot, it grew upright) only a little
	float along = dot(gradient, vec2(cos(p.yaw), -sin(p.yaw)));
	float across = dot(gradient, vec2(sin(p.yaw), cos(p.yaw)));
	vec2 onGround = vec2(atan(along), -atan(across));
	// a log is long (3-6m): the slope under its middle isn't the slope between its ends, over a dip or a bump one
	// end hung in the air. its tilt along its length comes from the ground at its two ends
	if (p.mesh >= PROP_FIRST_LOG && p.mesh < PROP_FIRST_STUMP) {
		vec2 axisUV = vec2(cos(p.yaw), -sin(p.yaw)) * (LOG_HALF_LENGTH * p.scale / terrainSize);
		float endA = textureLod(hm, p.uv - axisUV, 0.0).a, endB = textureLod(hm, p.uv + axisUV, 0.0).a;
		onGround.x = atan((endB - endA) * factor / (2.0 * LOG_HALF_LENGTH * p.scale));
	}
	vec2 wobble = (vec2(rnd2.y, fract(rnd2.y * 7.31 + rnd2.w)) - 0.5) * 0.4;
	vec2 tilt = p.mesh >= PROP_FIRST_STUMP ? onGround * 0.25 : (p.mesh >= PROP_FIRST_LOG ? onGround : onGround * 0.8 + wobble);
	p.tilt = packTilt(tilt);
	p.seed = rnd2.w;
	p.moss = clamp(data.g * 1.3 - 0.3 + weights[BIOME_TAIGA] * 0.3, 0.0, 1.0) * (1.0 - weights[BIOME_DESERT]) * (1.0 - weights[BIOME_SNOW]);
	// snow on them where the ground is snowy: the snow biome and above the snowline (the same lines as the terrain's
	// snow cap, treecommon.glsl placeTree), stored as negative moss
	float snowCap = smoothstep(0.3, 0.7, smoothstep(snowline - 0.03, snowline + 0.06, data.a)) * mountains;
	float snow = max(smoothstep(0.3, 0.7, weights[BIOME_SNOW]), snowCap);
	if (snow > 0.05) p.moss = -snow;
	p.height = data.a;
	return true;
}

// (propRotation: proprotation.glsl, props.vert turns them the same way)

#endif
