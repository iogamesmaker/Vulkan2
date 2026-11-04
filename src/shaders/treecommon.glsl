// shared between trees.comp / trees.vert / trees.frag / trees_shadow.frag
#ifndef TREE_COMMON_GLSL
#define TREE_COMMON_GLSL

#define SPECIES_CONIFER	  0
#define SPECIES_BROADLEAF 1
#define SPECIES_BUSH	  2
#define SPECIES_COUNT	  3
#define VARIANTS		  3 // meshes per species

// instance bins, must match trees.hpp: main LOD0 (9), main LOD1 (9), shadow casters (9, LOD1), impostors (9)
#define BINS_PER_GROUP 9
// per mesh, sized for wall to wall conifers (3 meshes share them) seen from above, where the whole radius is in view
#define CAPACITY_LOD0  4096u
#define CAPACITY_LOD1  32768u
#define CAPACITY_SHADOW 16384u
// impostors are one pool for all meshes (the mesh is packed into the fade, see packImpostorFade), as big as the
// placement itself: a tree goes in at most once, so it can never overflow and drop random trees
#define CAPACITY_IMPOSTOR MAX_PLACED_TREES

#define MAX_PLACED_TREES 1048576u // Trees::MAX_PLACED, more than the placement circle can hold (~994k cells)

uint binOffset(uint bin) {
	if (bin < 9u)  return bin * CAPACITY_LOD0;
	if (bin < 18u) return 9u * CAPACITY_LOD0 + (bin - 9u) * CAPACITY_LOD1;
	if (bin < 27u) return 9u * (CAPACITY_LOD0 + CAPACITY_LOD1) + (bin - 18u) * CAPACITY_SHADOW;
	return 9u * (CAPACITY_LOD0 + CAPACITY_LOD1 + CAPACITY_SHADOW) + (bin - 27u) * CAPACITY_IMPOSTOR;
}

// impostor instances carry their mesh in the integer part of the fade (the fade is 0..1)
float packImpostorFade(uint mesh, float fade) { return float(mesh) * 2.0 + clamp(fade, 0.0, 1.0); }
void unpackImpostorFade(float packed, out uint mesh, out float fade) { mesh = uint(packed * 0.5); fade = packed - float(mesh) * 2.0; }

uint binCapacity(uint bin) {
	return bin < 9u ? CAPACITY_LOD0 : (bin < 18u ? CAPACITY_LOD1 : (bin < 27u ? CAPACITY_SHADOW : CAPACITY_IMPOSTOR));
}

// persistent placement (trees_place.comp), re-run when the camera moved, culled every frame (trees.comp)
#define PLACE_RADIUS 900.0	   // ~5.4km
#define PLACE_GRID_SIDE 1125u  // 2 * PLACE_RADIUS / TREE_CELL_SIZE
#define BUSH_RADIUS 160.0
#define LOD0_DISTANCE 45.0 // the full crowns (inner branch rings) only up close
#define LOD1_DISTANCE 160.0	   // impostors beyond (the full crowns are too many cards for further out)
#define IMPOSTOR_BLEND 0.08   // fraction of LOD1_DISTANCE the mesh and the impostor crossfade over (dithered, a wide band shows as a checker pattern)
#define SHADOW_DISTANCE 160.0

struct TreeInstance {
	vec4 posScale; // xyz = position in terrain space, w = height in world units
	vec4 params;   // x = yaw, y = dryness (brown leaves), negative = snow, z = random seed, w = fade (dithered, see treeFaded)
};

#ifndef TREE_NO_PUSH_CONSTANTS
layout(push_constant) uniform constants {
	mat4 viewproj;
	vec3 campos;	   // terrain space
	float time;
	vec3 sundir;	   // trees_bake: direction to the viewer. otherwise x = last frame's time (wind motion vectors)
	float factor;	   // terrain height scale
	vec2 offsetWorld;  // terrain space -> continuous world
	ivec2 offset;	   // heightmap texel offset
	float terrainSize;
	float projScale;   // terrain LOD, see terrainheight.glsl
	float lodPixels;
	float density;	   // multiplier on the biome densities
} pc;


#endif

float snowHash(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float snowNoise(vec2 p) {
	vec2 i = floor(p), f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(snowHash(i), snowHash(i + vec2(1.0, 0.0)), u.x), mix(snowHash(i + vec2(0.0, 1.0)), snowHash(i + vec2(1.0, 1.0)), u.x), u.y);
}

// snow settles in clumps on what faces up (shared by the trees and the impostors). a soft partial cover everywhere
// just read as greyed leaves, so this is a hard edged mask instead: a pixel is either snow or bare leaf, and how
// much snow there is only moves the threshold. the clumps are value noise in world space (~1.5m and ~0.4m, hashed
// uv cells showed up as hard little squares on the leaf cards), the edge is antialiased over a pixel so far away
// the clumps blur into their coverage instead of aliasing into salt and pepper
#ifdef GL_FRAGMENT_SHADER // fwidth
float snowCover(float snow, vec3 normal, vec3 worldPos) {
	if (snow <= 0.0) return 0.0;
	vec2 p = worldPos.xz + worldPos.y * 0.5;
	float clumps = snowNoise(p * 0.7) * 0.65 + snowNoise(p * 2.5 + 11.0) * 0.35;
	float field = normal.y * 0.7 + clumps;
	float threshold = mix(1.45, 0.75, snow); // little snow: only the most upward, clumpiest bits
	float edge = fwidth(field) + 0.02;
	return smoothstep(threshold - edge, threshold + edge, field) * 0.95;
}
#endif

// snow on top of a leaf / bark colour: keeps a bit of the texture's detail (darker needles stay a bit darker under
// the snow) so it reads as snow lying on the tree instead of flat white cutouts. linear colours
vec3 applySnow(vec3 albedo, float cover) {
	float luma = dot(albedo, vec3(0.3, 0.59, 0.11));
	vec3 snow = vec3(0.8, 0.83, 0.88) * mix(0.65, 1.0, smoothstep(0.0, 0.12, luma));
	return mix(albedo, snow, cover);
}

float bayer4(vec2 p) {
	ivec2 i = ivec2(p) & 3;
	const float m[16] = float[](0., 8., 2., 10., 12., 4., 14., 6., 3., 11., 1., 9., 15., 7., 13., 5.);
	return (m[i.y * 4 + i.x] + 0.5) / 16.0;
}

// dithered fade. the meshes keep the pixels below the threshold, the impostors the ones above it (inverted), so
// where a tree is drawn as both during the LOD crossfade every pixel comes from exactly one of them
bool treeFaded(float fade, vec2 fragCoord, bool impostor) {
	float threshold = bayer4(fragCoord);
	return fade < (impostor ? 1.0 - threshold : threshold);
}

vec4 hash42(vec2 p) {
	vec4 p4 = fract(vec4(p.xyxy) * vec4(0.1031, 0.1030, 0.0973, 0.1099));
	p4 += dot(p4, p4.wzxy + 33.33);
	return fract((p4.xxyz + p4.yzzw) * p4.zywx);
}

// ------------------------------------------------------------------ placement
// one tree at most per grid cell (world anchored). shared by the scatter pass and the walking probe (collision)
#ifdef BIOME_TOOLS_GLSL

#define TREE_CELL_SIZE 1.6 // world units (~10m)

struct TreePlacement {
	vec2 xz;	   // terrain space
	vec2 uv;	   // heightmap uv
	uint species;
	float height;  // world units
	float slope;   // squared heightmap gradient, see terrain.frag
	float dryness;
	vec4 rnd, rnd2;
};

// the chances of a conifer, a broadleaf and a bush in a cell (before its roll), from the ground there (the heightmap's
// rgba: climate + height). treelineJitter moves the treeline (-0.5..0.5 per tree, 0 = the average), density = the
// setting. placeTree rolls against these; horizonao.comp bakes their average into the forest canopy map
vec3 treeChances(vec4 data, float treelineJitter, float density) {
	float temperature = data.r, moisture = data.g, height = data.a;
	float weights[BIOME_COUNT];
	groundCover(data.rgb, true, weights);
	float mountains = mountainWeight(data.rgb);

	// treeline on mountains, a bit lower where it's cold
	float treeline = mix(0.42, 0.56, temperature) + treelineJitter * 0.05;
	float belowTreeline = mountains * (1.0 - smoothstep(treeline - 0.04, treeline, height));
	float lowland = 1.0 - mountains;

	float conifer = (weights[BIOME_TAIGA] * 0.75 + weights[BIOME_SNOW] * 0.1) * lowland
				  + belowTreeline * (temperature < 0.5 ? 0.45 : 0.1);
	float broadleaf = weights[BIOME_PLAINS] * (0.03 + 0.55 * smoothstep(0.6, 0.78, moisture)) * lowland
					+ belowTreeline * (temperature < 0.5 ? 0.05 : 0.35);
	float bush = (weights[BIOME_PLAINS] * 0.08 + weights[BIOME_TAIGA] * 0.06 + weights[BIOME_DESERT] * 0.04) * lowland
			   + belowTreeline * 0.05;
	// no trees or bushes in wheat fields (the same field mask as foliage.comp)
	float field = smoothstep(0.35, 0.5, weights[BIOME_WHEAT] * lowland);
	return vec3(conifer, broadleaf, bush) * density * (1.0 - field);
}

bool placeTree(sampler2D hm, vec2 cellIndex, vec2 offsetWorld, ivec2 offset, float terrainSize, float density, out TreePlacement t) {
	t.rnd = hash42(cellIndex * 1.37 + 11.0);
	t.rnd2 = hash42(cellIndex * 0.71 - 5.0);
	t.xz = (cellIndex + 0.15 + t.rnd.xy * 0.7) * TREE_CELL_SIZE - offsetWorld;

	vec2 size = vec2(textureSize(hm, 0));
	t.uv = (t.xz + 0.5 * terrainSize) / terrainSize + vec2(offset) / size;
	vec4 data = textureLod(hm, t.uv, 0.0);
	float hR = textureLodOffset(hm, t.uv, 0.0, ivec2(1, 0)).a;
	float hL = textureLodOffset(hm, t.uv, 0.0, ivec2(-1, 0)).a;
	float hT = textureLodOffset(hm, t.uv, 0.0, ivec2(0, 1)).a;
	float hB = textureLodOffset(hm, t.uv, 0.0, ivec2(0, -1)).a;
	t.slope = pow((hR - hL) * heightmapSlopeScale(size.x), 2.0) + pow((hT - hB) * heightmapSlopeScale(size.x), 2.0);
	if (t.slope > 0.35) return false; // nothing on cliffs

	float temperature = data.r, moisture = data.g, height = data.a;
	float weights[BIOME_COUNT];
	groundCover(data.rgb, true, weights);
	float mountains = mountainWeight(data.rgb);
	vec3 chances = treeChances(data, t.rnd2.x - 0.5, density);

	float roll = t.rnd.z;
	if (roll < chances.x) t.species = SPECIES_CONIFER;
	else if (roll < chances.x + chances.y) t.species = SPECIES_BROADLEAF;
	else if (roll < chances.x + chances.y + chances.z) t.species = SPECIES_BUSH;
	else return false;

	if (t.species == SPECIES_CONIFER)		 t.height = mix(2.4, 4.4, t.rnd2.y);	// ~15-26m
	else if (t.species == SPECIES_BROADLEAF) t.height = mix(2.0, 3.4, t.rnd2.y);	// ~12-20m
	else									 t.height = mix(0.18, 0.42, t.rnd2.y); // ~1-2.5m

	// snowy trees where the ground is snowy too: the snow biome and above the snowline on mountains (terrain.frag).
	// (cold alone used to frost them, which gave white trees standing on green ground in the taiga)
	// snow and dryness never happen together, so snow is stored as negative dryness
	// the same lines as the terrain's snow cap (terrainmaterial.glsl) and only once the snow actually shows on the
	// ground: the raw weights frosted trees at the taiga's edge and near the treeline grey on green ground
	float snowline = mix(0.44, 0.68, temperature);
	float snowCap = smoothstep(0.3, 0.7, smoothstep(snowline - 0.03, snowline + 0.06, height)) * mountains;
	float snow = max(smoothstep(0.3, 0.7, weights[BIOME_SNOW]), snowCap);
	// dry (olive / brown leaves) only where the ground looks dry too: the desert, and only the very driest of the
	// rest (moisture alone used to wash out whole forests standing on lush green grass)
	t.dryness = snow > 0.05 ? -snow : weights[BIOME_DESERT] + smoothstep(0.22, 0.1, moisture) * 0.3;
	return true;
}

// trunk radius at the base, matches the meshes in trees.cpp (0 for bushes, you can walk through those)
float trunkRadius(uint species, float height) {
	return species == SPECIES_CONIFER ? height * 0.018 : (species == SPECIES_BROADLEAF ? height * 0.03 : 0.0);
}

#endif

// the tree types' colours (uv.w of the mesh, trees.cpp): the branch photos are shared, this sets them apart.
// 1 spruce, 2 pine, 3 oak, 4 autumn bush, 5 birch, 6 bush. albedo linear, bark uv wraps around the trunk once
float treeHash(vec2 p) { vec3 p3 = fract(vec3(p.xyx) * 0.1031); p3 += dot(p3, p3.yzx + 33.33); return fract((p3.x + p3.y) * p3.z); }
float treeNoise(vec2 p) {
	vec2 i = floor(p), f = fract(p), u = f * f * (3.0 - 2.0 * f);
	return mix(mix(treeHash(i), treeHash(i + vec2(1, 0)), u.x), mix(treeHash(i + vec2(0, 1)), treeHash(i + vec2(1, 1)), u.x), u.y);
}
vec3 treeTint(vec3 albedo, float tint, bool leaves, vec2 uv) {
	int type = int(tint + 0.5);
	if (leaves) {
		// the leaf photos are dark (shot in shade: linear albedo ~0.023 for the needles, ~0.048 for the broadleaf
		// atlas), real foliage is ~0.06-0.1, about what the grass has. without this the canopies were near black
		albedo *= type <= 2 ? 2.4 : 1.7; // spruce / pine use the conifer atlas
		if (type == 1) return albedo * vec3(0.8, 0.92, 0.88);  // spruce: deep blue green
		if (type == 2) return albedo * vec3(1.0, 1.02, 0.9);
		if (type == 3) return albedo * vec3(0.9, 1.0, 0.82);
		if (type == 5) return albedo * vec3(1.18, 1.2, 0.8);  // birch: light, yellowish
		if (type == 6) return albedo * vec3(0.85, 0.95, 0.8);
		return albedo;
	}
	// bark: the photos are calibrated bright (linear ~0.15 / ~0.2), real bark is ~0.05-0.1 and next to the leaves
	// (~0.06) the trunks glowed through the crowns
	albedo *= type <= 2 ? 0.55 : 0.42; // spruce / pine use the conifer bark
	if (type == 2) return albedo * vec3(1.3, 0.95, 0.72); // pine: orange brown bark
	if (type == 5) {
		// birch: chalky white with dark horizontal marks (lenticels) and darker patches
		float marks = smoothstep(0.62, 0.78, treeNoise(vec2(uv.x * 5.0, uv.y * 30.0))) * smoothstep(0.3, 0.6, treeNoise(vec2(uv.x * 2.0, uv.y * 4.0) + 7.0));
		float patches = smoothstep(0.55, 0.8, treeNoise(uv * vec2(3.0, 2.5) + 19.0));
		float grain = dot(albedo, vec3(0.33)) / 0.12; // the bark texture's own structure, a little
		vec3 white = vec3(0.62, 0.6, 0.56) * mix(0.9, 1.1, clamp(grain, 0.0, 2.0) * 0.5);
		return mix(white, vec3(0.05, 0.045, 0.04), max(marks, patches * 0.6));
	}
	return albedo;
}

#endif