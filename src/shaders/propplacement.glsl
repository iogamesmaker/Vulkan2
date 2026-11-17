// which rock / boulder / log sits in a placement cell (world anchored, a pure function of the cell), shared by the
// scatter (props.comp) and the walking probe (probe.comp, collisions), so you collide with exactly what's drawn.
// include biometools.glsl first
#ifndef PROP_PLACEMENT_GLSL
#define PROP_PLACEMENT_GLSL

#define PROP_CELL 0.9 // world units (~5m) per placement cell

#include "propmeshes.glsl" // rocks, then logs, then stumps
#include "proprotation.glsl"
#include "noise.glsl"

// (the walking collision uses each mesh's own baked shape, Props::bakeShape)
#define ROCK_SINK 0.06 // rocks sit this far (times the scale) into the ground, over uneven ground too
#define LOG_SINK 0.01
#define LOG_HALF_LENGTH 0.45 // the log meshes run -0.5..0.5 along x (Props::loadMesh), the ends taper
#define STUMP_HEIGHT 0.1

struct PropPlacement {
	vec2 xz;	 // terrain space
	vec2 uv;	 // heightmap uv
	uint mesh;	 // rocks, logs, stumps (PROP_FIRST_LOG / PROP_FIRST_STUMP)
	float scale; // world units
	float yaw, tilt, seed, moss; // moss: how wet / shaded it is, negative = snow lying on it, 1 + x = sand on it (desert)
	float sink;	 // how far it sits in the ground, times the scale (props.comp draws it there, probe.comp collides with it there)
	float height; // raw heightmap height (0..1), a rough idea of the altitude
};

bool placeProp(sampler2D hm, sampler2D climate, vec2 cellIndex, vec2 offsetWorld, ivec2 offset, float terrainSize, float factor, out PropPlacement p) {
	vec4 rnd = hash42(cellIndex * 1.71 + 3.3);
	vec4 rnd2 = hash42(cellIndex * 0.37 - 9.1);
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
	// a stump is a felled tree's foot: never out on bare rock, scree or the snow cap (the masks the terrain shows them
	// with, biometools.glsl), nor anywhere rocky enough for the scree
	if (p.mesh >= PROP_FIRST_STUMP) {
		GroundBareness ground = groundBareness(p.uv, data, slope);
		if (ground.stone > 0.03 || ground.snowCap > 0.2 || rocky > 0.1) return false;
	}
	// small rocks don't show on snow fields (buried): scattered dark pebbles all over the snow looked like pepper
	float snowline = snowlineHeight(data.rgb); // terrain.frag's, without its noise
	float snowy = smoothstep(snowline - 0.03, snowline + 0.06, data.a) * mountains * (1.0 - smoothstep(0.5, 1.0, slope));
	if (p.mesh < PROP_FIRST_LOG && p.scale < 0.4 && snowy > 0.35) return false;

	p.yaw = rnd2.z * 6.2831853;
	// logs lie along the slope under their own axis (the yaw turns the mesh's x axis to (cos, -sin) in xz)
	vec2 gradient = vec2(hR - hL, hT - hB) * factor / (2.0 * terrainSize / size.x); // height per world unit
	// and every way across it: on the ground's normal (packTilt). rocks most of the way plus a random wobble,
	// logs and stumps all the way (a stump standing upright on a slope had half its foot in the air)
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
	vec2 tilt = p.mesh >= PROP_FIRST_LOG ? onGround : onGround * 0.8 + wobble;
	p.tilt = packTilt(tilt);
	p.seed = rnd2.w;
	p.moss = clamp(data.g * 1.3 - 0.3 + weights[BIOME_TAIGA] * 0.3, 0.0, 1.0) * (1.0 - weights[BIOME_DESERT]) * (1.0 - weights[BIOME_SNOW]);
	// snow on them where the ground is snowy: the snow biome and above the snowline (the same lines as the terrain's
	// snow cap, treecommon.glsl placeTree), stored as negative moss
	float snowCap = smoothstep(0.3, 0.7, smoothstep(snowline - 0.03, snowline + 0.06, data.a)) * mountains;
	float snow = max(smoothstep(0.3, 0.7, weights[BIOME_SNOW]), snowCap);
	if (snow > 0.05) p.moss = -snow;
	// the desert: rocks half buried in the sand (a stone sitting on top of a dune read as dropped there), sand drifted
	// up their foot and lying on their tops (props.frag), stored as 1 + how much
	float sand = smoothstep(0.15, 0.5, weights[BIOME_DESERT]) * (1.0 - mountains * 0.6);
	p.sink = p.mesh < PROP_FIRST_LOG ? ROCK_SINK + sand * mix(0.12, 0.3, rnd2.y) : (p.mesh < PROP_FIRST_STUMP ? LOG_SINK : 0.01);
	if (sand > 0.05 && snow <= 0.05) p.moss = 1.0 + sand;
	p.height = data.a;
	return true;
}

// is xz (terrain space) under a rock, log or stump? the grass and the ground cover (foliage.comp, groundcover.comp) ask
// so nothing grows up through them. the footprints are rough: a rock's widest horizontal half extent is 0.55 (mesh
// space, Props::loadMesh), most of it is narrower, 0.42 keeps the blades at its foot; a log is a capsule along its axis,
// a stump a disc. margin (world units) widens them for a plant's own size
#define PROP_REACH 0.72 // the furthest any footprint reaches from its prop (a 1.4 boulder), plus a margin
bool insideProp(sampler2D hm, sampler2D climate, vec2 xz, vec2 offsetWorld, ivec2 offset, float terrainSize, float factor, float margin) {
	vec2 home = floor((xz + offsetWorld) / PROP_CELL);
	for (int y = -1; y <= 1; y++) {
		for (int x = -1; x <= 1; x++) {
			vec2 cellIndex = home + vec2(x, y);
			// where the cell's prop would sit is a pure hash (placeProp's first line): skip the cells it can't reach from
			// before the textures. leaves ~2 of the 9 to place for real
			vec4 rnd = hash42(cellIndex * 1.71 + 3.3);
			vec2 at = (cellIndex + 0.1 + rnd.xy * 0.8) * PROP_CELL - offsetWorld;
			if (distance(at, xz) > PROP_REACH + margin) continue;
			PropPlacement p;
			if (!placeProp(hm, climate, cellIndex, offsetWorld, offset, terrainSize, factor, p)) continue;
			vec2 d = xz - p.xz;
			if (p.mesh < PROP_FIRST_LOG) {
				if (length(d) < 0.42 * p.scale + margin) return true;
			} else if (p.mesh < PROP_FIRST_STUMP) {
				vec2 axis = vec2(cos(p.yaw), -sin(p.yaw));
				float along = clamp(dot(d, axis), -LOG_HALF_LENGTH * p.scale, LOG_HALF_LENGTH * p.scale);
				if (length(d - axis * along) < 0.07 * p.scale + margin) return true;
			} else if (length(d) < 0.12 * p.scale + margin) return true;
		}
	}
	return false;
}

// (propRotation: proprotation.glsl, props.vert turns them the same way)

#endif
