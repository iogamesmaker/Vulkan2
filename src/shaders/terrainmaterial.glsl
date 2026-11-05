// the terrain's surface: which materials show where (climate, rock, snow), the detail textures at three
// scales, triplanar cliffs, parallax up close and the colour grading on top. shared by terrain.frag (per pixel)
// and terraincache.comp (bakes the same surface into the far field cache, see terraincache.hpp), so the two
// match exactly where one hands over to the other
// the includer declares heightmap, climateMap, albedo[], normalmap[], pom[] and the atmosphere UBO (ubo)
#ifndef TERRAINMATERIAL_GLSL
#define TERRAINMATERIAL_GLSL

// the detail textures' world space period: near (1.2), far (6, rotated 3-4-5) and macro (42, rotated 7-24-25)
// repeats all line up again after 1050 world units, so the detail space can wrap at it without a seam. it keeps
// the uvs small (precise), and the cache bakes the same texture wherever the map offset happens to be
#define TERRAIN_DETAIL_PERIOD 1050.0

// ------------------------------------------------------------------ texturing helpers

float hash12(vec2 p) {
	// exact integer math on the lattice point: the float hash (fract chains) is chaotic, the compiler fusing its
	// multiply-adds differently for the cells either side of a border gave different values for the same point,
	// hard seams in the noise (the snow edge's stripes), worse the further from the world's origin
	uvec2 q = uvec2(ivec2(p)) * uvec2(1597334673u, 3812015801u);
	uint n = (q.x ^ q.y) * 1597334673u;
	return float(n >> 8) * (1.0 / 16777216.0);
}

float valueNoise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(hash12(i), hash12(i + vec2(1.0, 0.0)), u.x),
			   mix(hash12(i + vec2(0.0, 1.0)), hash12(i + vec2(1.0, 1.0)), u.x), u.y);
}

// normal maps are BC5 (x and y only), z comes back from the unit length
vec3 unpackNormal(vec2 xy) {
	vec2 n = xy * 2.0 - 1.0;
	return vec3(n, sqrt(max(1.0 - dot(n, n), 0.0)));
}

struct Material {
	vec3 albedo;
	vec3 normal; // tangent space
	float height;
};

// every material is sampled at two scales: real world size up close, a 5x bigger rotated copy further away.
// the rotation + the different scale break up the repetition, the far layer stops the near one turning into
// a uniform mush (or moire) in the distance
// gradients are taken outside so sampling inside the (divergent) branches still gets proper mip levels
// (the gradients come from the surface position's derivatives, TerrainShading.dPdx / dPdy: a pixel's in
// terrain.frag, a cache texel's in terraincache.comp)
struct DetailUV { vec2 uv, dx, dy; };

// a third, much bigger scale takes over in the distance (macroWeight): the far layer's ~18m tile otherwise prints
// a visible lattice over whole mountain sides. at most two of the three layers have weight at any distance
// the height is only needed where two materials blend
void addLayer(inout Material m, int index, DetailUV layer, float weight, bool needHeight) {
	m.albedo += textureGrad(albedo[nonuniformEXT(index)], layer.uv, layer.dx, layer.dy).rgb * weight;
	m.normal += unpackNormal(textureGrad(normalmap[nonuniformEXT(index)], layer.uv, layer.dx, layer.dy).rg) * weight;
	if (needHeight) m.height += textureGrad(pom[nonuniformEXT(index)], layer.uv, layer.dx, layer.dy).r * weight;
}

Material sampleMaterial(int index, DetailUV near, DetailUV far, DetailUV macro, float farWeight, float macroWeight, bool needHeight) {
	Material m;
	m.albedo = vec3(0.0); m.normal = vec3(0.0); m.height = 0.0;
	float wNear = 1.0 - farWeight, wFar = farWeight * (1.0 - macroWeight), wMacro = farWeight * macroWeight;
	if (wNear > 0.0) addLayer(m, index, near, wNear, needHeight);
	if (wFar > 0.0) addLayer(m, index, far, wFar, needHeight);
	if (wMacro > 0.0) addLayer(m, index, macro, wMacro, needHeight);
	return m;
}

// ------------------------------------------------------------------ parallax occlusion mapping

// distance, depth, step count and self shadowing strength come from the UBO (ubo.pom*, settings window)
#define POM_STEP_LIMIT 64 // hard cap on ubo.pomSteps

// how much parallax each material gets (times ubo.pomDepth). grass' height map is noise, parallax on it just
// smears the stalks around, so it gets none. order: the BIOME_ defines
const float POM_MATERIAL_DEPTH[BIOME_COUNT] = float[](0.0, 0.6, 1.0, 0.6, 0.35, 0.3);

// height blending: where both are present the material that's physically higher wins (sand in the cracks, grass
// tufts poking through), which looks a lot more natural than a plain crossfade. wFirst is normalised (the two
// weights add up to 1), returns the weight of the second material
// the heights count less than the weights: a material whose weight is below HEIGHT_BLEND_CUTOFF can't
// poke through at all, so it fades out completely before it swaps places with another one, and isn't sampled
#define HEIGHT_BLEND_CUTOFF 0.125 // (1 - 0.4 - blendDepth) / 2
float heightBlend(float h1, float h2, float wFirst) {
	// a deep blend band and heights at 0.4: wide, soft transitions (narrower ones read as outlined patches)
	const float blendDepth = 0.35;
	float a1 = h1 * 0.4 + wFirst;
	float a2 = h2 * 0.4 + 1.0 - wFirst;
	float level = max(a1, a2) - blendDepth;
	float b1 = max(a1 - level, 0.0);
	float b2 = max(a2 - level, 0.0);
	return b2 / max(b1 + b2, 1e-4);
}

// the two materials of one projection, height blended. one alone where the other can't show (most pixels)
void sampleBlend(int first, int second, float wA, DetailUV near, DetailUV far, DetailUV macro, float farWeight, float macroWeight, out vec3 albedo, out vec3 normal) {
	if (wA > 1.0 - HEIGHT_BLEND_CUTOFF || wA < HEIGHT_BLEND_CUTOFF) {
		Material m = sampleMaterial(wA > 0.5 ? first : second, near, far, macro, farWeight, macroWeight, false);
		albedo = m.albedo; normal = normalize(m.normal);
		return;
	}
	Material m1 = sampleMaterial(first, near, far, macro, farWeight, macroWeight, true);
	Material m2 = sampleMaterial(second, near, far, macro, farWeight, macroWeight, true);
	float blend = heightBlend(m1.height, m2.height, wA); // 0 = first, 1 = second
	albedo = mix(m1.albedo, m2.albedo, blend);
	normal = normalize(mix(m1.normal, m2.normal, blend));
}

// the height of the blended surface here, with exactly the blend the textures get, so the parallax traces the
// same surface that ends up textured
float blendedHeight(int first, int second, float wFirst, DetailUV near, vec2 uv) {
	if (wFirst > 1.0 - HEIGHT_BLEND_CUTOFF) return textureGrad(pom[nonuniformEXT(first)], uv, near.dx, near.dy).r;
	if (wFirst < HEIGHT_BLEND_CUTOFF) return textureGrad(pom[nonuniformEXT(second)], uv, near.dx, near.dy).r;
	float h1 = textureGrad(pom[nonuniformEXT(first)], uv, near.dx, near.dy).r;
	float h2 = textureGrad(pom[nonuniformEXT(second)], uv, near.dx, near.dy).r;
	return mix(h1, h2, heightBlend(h1, h2, wFirst));
}

// steps down the view ray (tangent space, towards the eye) through the height field until it's below the surface,
// then interpolates between the last two steps. returns the shifted uv, depth = how far down it hit (0 top, 1 bottom)
vec2 parallaxOcclusion(int first, int second, float wFirst, DetailUV near, vec3 viewTS, float scale, float fade, out float depth) {
	// fewer steps looking straight down. not scaled by the fade: the depth already shrinks there, and few steps
	// on a still deep height field shows up as stacked slices right where the effect fades out
	float maxSteps = clamp(ubo.pomSteps, 2.0, float(POM_STEP_LIMIT));
	float steps = max(ceil(mix(maxSteps, min(4.0, maxSteps), abs(viewTS.z))), 2.0);
	float layer = 1.0 / steps;
	// the shift is capped at grazing angles, past that it only stretches the texture into streaks
	vec2 delta = viewTS.xy / max(viewTS.z, 0.35) * scale * layer;

	vec2 uv = near.uv;
	float rayDepth = 0.0;
	float surfaceDepth = 1.0 - blendedHeight(first, second, wFirst, near, uv);
	bool crossed = rayDepth >= surfaceDepth;
	for (int i = 0; i < POM_STEP_LIMIT; i++) {
		if (float(i) >= steps || crossed) break;
		uv -= delta;
		rayDepth += layer;
		surfaceDepth = 1.0 - blendedHeight(first, second, wFirst, near, uv);
		crossed = rayDepth >= surfaceDepth;
	}
	if (!crossed || rayDepth == 0.0) { depth = rayDepth; return uv; }

	// the crossing is somewhere in the last step: a few binary search steps pin it down, the linear guess
	// between two coarse steps is what shows up as stair stepping / layered slices
	vec2 uvAbove = uv + delta, uvBelow = uv;
	float depthAbove = rayDepth - layer, depthBelow = rayDepth;
	for (int i = 0; i < 5; i++) {
		vec2 uvMid = (uvAbove + uvBelow) * 0.5;
		float depthMid = (depthAbove + depthBelow) * 0.5;
		if (depthMid >= 1.0 - blendedHeight(first, second, wFirst, near, uvMid)) { uvBelow = uvMid; depthBelow = depthMid; }
		else { uvAbove = uvMid; depthAbove = depthMid; }
	}
	depth = (depthAbove + depthBelow) * 0.5;
	return (uvAbove + uvBelow) * 0.5;
}

// soft self shadowing: march from the hit point towards the light, anything of the height field above the ray
// darkens it, the closer and the further above the more
float parallaxShadow(int first, int second, float wFirst, DetailUV near, vec2 uv, float depth, vec3 lightTS, float scale) {
	if (lightTS.z <= 0.0) return 1.0;
	const int STEPS = 6;
	float startHeight = 1.0 - depth;
	float layer = (1.0 - startHeight) / float(STEPS);
	vec2 delta = lightTS.xy / max(lightTS.z, 0.1) * scale * layer;
	float occlusion = 0.0;
	for (int i = 1; i <= STEPS; i++) {
		float rayHeight = startHeight + layer * float(i);
		float h = blendedHeight(first, second, wFirst, near, uv + delta * float(i));
		occlusion = max(occlusion, (h - rayHeight) * (1.0 - float(i) / float(STEPS + 1)));
	}
	return 1.0 - clamp(occlusion * 6.0 * ubo.pomShadow, 0.0, 1.0);
}

// ------------------------------------------------------------------ the surface

struct TerrainShading {
	vec3 position;		// terrain space (y = height): its y and its distance to campos matter
	vec3 dPdx, dPdy;	// the position's derivatives over a pixel (terrain.frag) / a cache texel (terraincache.comp)
	vec2 readCoord;		// heightmap uv (wraps)
	vec2 detailPos;		// xz in the detail textures' space (wraps at TERRAIN_DETAIL_PERIOD)
	vec2 worldXZ;		// continuous world xz, the noise is anchored to it
	float factor;		// world units of height per unit of heightmap
	float viewDistance; // parallax up close (none in the cache)
	vec3 campos;
	vec3 sundir;
	float farWeight;	// 0 = the near texture scale, 1 = the far one
	float macroWeight;	// the macro layer's share of the far scale
	float bumpFade;		// the procedural bumps in the normal
	float canopyNear;	// how much of the wheat canopy shows (up close it's partly the floor under the plants)
};

struct TerrainSurface {
	vec3 albedo;	 // linear
	vec3 normal;	 // world space
	float canopy;	 // the wheat canopy's share of the albedo (the caller animates its wind gusts)
	float pomShadow; // the parallax self shadow
};

// rock detail below the mesh's resolution, as a height field on top of the terrain's: its slope (height per world
// unit, for the normal), a colour factor and how much of it shows
struct RockDetail {
	vec2 slope;
	vec3 tint;
	float weight;		// the relief (slope, tint)
	int material;		// this layer's rock (TERRAIN_STRATA_FIRST + 0..2), sampled over the base rock
	float materialMix;	// how much of it shows (the layers' colours carry further than their relief)
};

#define TERRAIN_STRATA_FIRST 6 // the strata rocks after the biome materials, Application::loadTextures

#define STRATA_THICKNESS 0.45 // world units (~2.7m) per big rock layer, a finer layering inside them
#define FLUTE_WIDTH 0.5		  // the erosion runnels down the faces, ~3m apart
#define FLUTE_DEPTH 0.16

// one set of layers: a flat tread then a steep riser up to the next layer, the surface becomes
// thickness * smoothfloor(layer) - its own height, a staircase cut into the slope. returns the slope that adds
// (height per world unit), f = where in its layer this is (0 tread start .. 1 riser top)
vec2 strataStep(float layer, vec2 layerGrad, float riserStart, out float f) {
	f = fract(layer);
	float t = clamp((f - riserStart) / (1.0 - riserStart), 0.0, 1.0);
	float riser = 6.0 * t * (1.0 - t) / (1.0 - riserStart); // d smoothstep(riserStart, 1, f) / df
	return (riser - 1.0) * layerGrad;
}

// h = the surface's height, gradient = the terrain's height per world unit, mask = how much bare steep rock this is.
// the pixel footprint (dPdx / dPdy) fades it out before the layers get thinner than a few pixels (and in the
// terrain cache, whose texels are far bigger than a layer)
RockDetail rockStrata(float h, vec2 worldXZ, vec2 gradient, vec3 dPdx, vec3 dPdy, float viewDistance, float mask) {
	RockDetail r;
	r.slope = vec2(0.0);
	r.tint = vec3(1.0);
	r.material = TERRAIN_STRATA_FIRST;
	r.materialMix = 0.0;
	float footprint = max(max(abs(dPdx.y), abs(dPdy.y)), 0.3 * max(length(dPdx.xz), length(dPdy.xz)));
	// close up only: further out the layers lined whole mountainsides like rice terraces (viewDistance is 0 in the
	// cache, its texel footprint fades it there)
	float colourWeight = mask * (1.0 - smoothstep(0.15, 0.4, footprint / STRATA_THICKNESS)) * (1.0 - smoothstep(60.0, 110.0, viewDistance));
	r.weight = mask * (1.0 - smoothstep(0.06, 0.2, footprint / STRATA_THICKNESS)) * (1.0 - smoothstep(6.0, 22.0, viewDistance));
	if (colourWeight <= 0.0) return r;
	float fine = 1.0 - smoothstep(0.15, 0.4, footprint / (STRATA_THICKNESS * 0.3)); // the thin layers go first

	// the layers are horizontal-ish, tilted a little (the dip) and bent by a slow warp so they wander along the face
	// and swell / pinch (no two the same thickness)
	const vec2 dip = vec2(0.12, -0.07);
	float warp = (detailValueNoise(worldXZ * 0.12) - 0.5) * 2.5 + (detailValueNoise(worldXZ * 0.5 + 7.0) - 0.5) * 0.6; // in layers
	vec2 warpGrad = detailValueNoiseGrad(worldXZ * 0.12) * 0.12 * 2.5 + detailValueNoiseGrad(worldXZ * 0.5 + 7.0) * 0.5 * 0.6;
	float layer = (h + dot(worldXZ, dip)) / STRATA_THICKNESS + warp;
	vec2 layerGrad = (gradient + dip) + warpGrad * STRATA_THICKNESS; // in height units per world unit

	// every big layer its own rock: buff sandstone, dark slate, rusty iron stained stone, or the base rock. only
	// partly (a tint of it over the base rock, they're one cliff) and only in some stretches of a face (formations
	// come and go). soft at the layer edges, where it fades into the next one
	float id = floor(layer);
	float pick = fract(sin(id * 78.233) * 43758.5453);
	r.material = TERRAIN_STRATA_FIRST + int(pick * 4.0) % 3;
	float fl = fract(layer);
	float soft = smoothstep(0.0, 0.12, fl) * (1.0 - smoothstep(0.88, 1.0, fl));
	float formation = smoothstep(0.2, 0.5, detailValueNoise(worldXZ * 0.03 + 3.0)) * smoothstep(0.15, 0.45, detailValueNoise(worldXZ * 0.15 + id * 1.7));
	r.materialMix = colourWeight * (pick < 0.75 ? 0.5 : 0.0) * soft * formation;
	if (r.weight <= 0.0) return r;

	// big layers: their ledges come and go along the face (blocky, broken, not contour lines)
	float f;
	vec2 big = strataStep(layer, layerGrad, 0.3, f); // wide, rounded risers (narrow ones read as knife cuts)
	float ledge = smoothstep(0.2, 0.5, detailValueNoise(worldXZ * vec2(0.35, 0.35) + id * 3.7));
	r.slope = big * ledge * 0.4; // part way: full treads lay flat and the faces read as rice terraces
	// thin layering inside them (shale), weaker
	float fFine;
	vec2 thin = strataStep(layer * 3.3 + 0.37, layerGrad, 0.3, fFine) * 0.08 * fine; // a hint, many sharp lines looked like wood grain
	r.slope += thin;

	// the flutes: ridged noise across the fall line, stretched down it, only on real faces
	float g = length(gradient);
	vec2 down = gradient / max(g, 1e-3), across = vec2(-down.y, down.x);
	vec2 q = vec2(dot(worldXZ, across) / FLUTE_WIDTH, h / (FLUTE_WIDTH * 5.0));
	float n = detailValueNoise(q);
	vec2 nGrad = detailValueNoiseGrad(q);
	float ridged = 1.0 - abs(2.0 * n - 1.0);
	vec2 ridgedGrad = -2.0 * sign(2.0 * n - 1.0) * nGrad; // per q
	float faces = smoothstep(0.5, 1.1, g);
	r.slope += FLUTE_DEPTH * (ridgedGrad.x * across / FLUTE_WIDTH + ridgedGrad.y * gradient / (FLUTE_WIDTH * 5.0)) * faces;
	r.slope *= r.weight;

	// colour: every big layer its own shade of grey / buff (sediment), dark in the crease where a tread meets the
	// riser above it (where the ledge is), light along the weathered edges, darker down the runnels
	float band = fract(sin(id * 12.9898) * 43758.5453);
	float crease = smoothstep(0.15, 0.3, f) * (1.0 - smoothstep(0.3, 0.55, f)) * ledge;
	float edge = (smoothstep(0.85, 1.0, f) + 1.0 - smoothstep(0.0, 0.1, f)) * ledge;
	float thinLine = smoothstep(0.3, 0.5, fFine) * (1.0 - smoothstep(0.5, 0.8, fFine)) * fine;
	r.tint = mix(vec3(0.94, 0.93, 0.92), vec3(1.04, 1.03, 1.0), band) * (1.0 - crease * 0.22) * (1.0 + edge * 0.08)
		   * (1.0 - thinLine * 0.04) * (1.0 - (1.0 - ridged) * 0.25 * faces);
	return r;
}

// the layer's own rock over a projection's base material
void strataBlend(RockDetail r, DetailUV near, DetailUV far, DetailUV macro, float farWeight, float macroWeight, inout vec3 albedo, inout vec3 normal) {
	if (r.materialMix <= 0.01) return;
	Material m = sampleMaterial(r.material, near, far, macro, farWeight, macroWeight, false);
	vec3 layer = mix(vec3(dot(m.albedo, vec3(0.3, 0.59, 0.11))), m.albedo, 0.6); // less saturated, it's weathered
	albedo = mix(albedo, layer, r.materialMix);
	normal = normalize(mix(normal, normalize(m.normal), r.materialMix));
}

TerrainSurface shadeTerrain(TerrainShading s) {
	vec2 size = vec2(textureSize(heightmap, 0));
	vec2 readCoord = s.readCoord;

	float hR = textureLodOffset(heightmap, readCoord, 0.0, ivec2(1, 0)).a;
	float hL = textureLodOffset(heightmap, readCoord, 0.0, ivec2(-1, 0)).a;
	float hT = textureLodOffset(heightmap, readCoord, 0.0, ivec2(0, 1)).a;
	float hB = textureLodOffset(heightmap, readCoord, 0.0, ivec2(0, -1)).a;

	float dzdx = (hR - hL) * heightmapSlopeScale(size.x);
	float dzdy = (hT - hB) * heightmapSlopeScale(size.x);
	float slope = dzdx * dzdx + dzdy * dzdy;

	vec4 terrainData = vec4(textureLod(climateMap, readCoord, 0.0).rgb, textureLod(heightmap, readCoord, 0.0).a); // climate (half resolution), height
	vec3 climate = terrainData.rgb;
	float terrainHeight = terrainData.a;

	// low frequency noise to make the rock / snow lines and the colour variation less uniform
	float lineNoise = valueNoise(readCoord * 180.0) * 0.65 + valueNoise(readCoord * 900.0) * 0.35;

	// what grows here according to the climate. mountains don't have their own cover, their slopes keep the
	// local one (no farmland though), rock shows up on cliffs and above a rock line, snow above a snow line
	float mountains = mountainWeight(climate);
	float weights[BIOME_COUNT];
	groundCover(climate, true, weights);
	float farmland = weights[BIOME_WHEAT] * mountains;
	weights[BIOME_WHEAT] -= farmland;
	weights[BIOME_PLAINS] += farmland;
	// where foliage.comp grows wheat fields (the same mask), the canopy below takes over from the cards far away
	float wheatField = smoothstep(0.35, 0.5, weights[BIOME_WHEAT]) * (1.0 - smoothstep(0.1, 0.3, weights[BIOME_DESERT] + weights[BIOME_SNOW]));

	// curvature over ~3 texels (~25m): how far the surroundings sit above (gully, > 0) or below (ridge, < 0) this
	// point, per unit of distance (world units over world units). the shape the far mountains get their definition
	// from: gullies darker and catching snow, ridges lighter and blown bare
	// (only where it shows: mountains, and cliffs anywhere. the lowlands skip the 4 extra heightmap reads)
	float curvature = 0.0;
	if (mountains > 0.01 || slope > 0.3) {
		vec2 curvatureStep = 3.0 / size;
		float around = textureLod(heightmap, readCoord + vec2(curvatureStep.x, 0.0), 0.0).a + textureLod(heightmap, readCoord - vec2(curvatureStep.x, 0.0), 0.0).a
					 + textureLod(heightmap, readCoord + vec2(0.0, curvatureStep.y), 0.0).a + textureLod(heightmap, readCoord - vec2(0.0, curvatureStep.y), 0.0).a;
		curvature = (around * 0.25 - terrainHeight) * s.factor / (3.0 * ubo.terrainSize / size.x);
	}
	float gully = smoothstep(0.01, 0.12, curvature), ridge = smoothstep(-0.01, -0.12, curvature);

	// bare rock on cliffs: in the lowlands only where it's really steep (the erosion's small scarps on the grassy hills
	// were all rock streaks), the mountains from a gentler slope
	float steepRock = smoothstep(mix(0.6, 0.35, mountains), mix(1.0, 0.75, mountains), slope + (lineNoise - 0.5) * 0.3);
	steepRock *= 1.0 - weights[BIOME_DESERT] * (1.0 - mountains); // dune and wadi walls are sand, not stone
	float rockline = 0.58 + (lineNoise - 0.5) * 0.12;
	float highRock = smoothstep(rockline, rockline + 0.06, terrainHeight) * mountains;
	float rock = max(steepRock, highRock);
	for (int i = 0; i < BIOME_COUNT; i++) weights[i] *= 1.0 - rock;
	weights[BIOME_MOUNTAINS] += rock;

	// snow on the upper half of most mountains (lower where it's cold), only the steepest cliffs stay bare
	float snowline = mix(0.44, 0.68, climate.r) + (lineNoise - 0.5) * 0.08;
	// snow settles on ledges and gentle ground and collects in gullies, steep faces and wind blown ridges stay bare.
	// a crisp edge (the noise only moves it around) instead of soft blotches
	float snowHeight = smoothstep(snowline - 0.03, snowline + 0.06, terrainHeight);
	float snowCover = snowHeight * (1.0 - steepRock * 0.8) + gully * snowHeight * 0.6 - ridge * 0.25;
	// a soft, frayed edge (noise at ~7m and ~1.5m): snow thins out and melts away into the rock and grass in patches,
	// the height blend then lets it settle in the cracks first. a crisp edge cut it out like a sticker
	// (on the continuous world position: in the wrapped detail space the pattern jumped whenever it wrapped)
	vec2 snowPos = s.worldXZ;
	float snowFray = valueNoise(snowPos * 0.9) * 0.6 + valueNoise(snowPos * 3.7 + 5.0) * 0.4;
	float snowCap = smoothstep(0.3, 0.7, snowCover + (lineNoise - 0.5) * 0.2 + (snowFray - 0.5) * 0.4) * mountains;
	for (int i = 0; i < BIOME_COUNT; i++) weights[i] *= 1.0 - snowCap;
	weights[BIOME_SNOW] += snowCap;

	float materials[BIOME_COUNT];
	for (int i = 0; i < BIOME_COUNT; i++) materials[i] = weights[i];

	// only the two strongest materials get sampled. the third strongest is taken off both weights, so a material
	// has faded out completely by the time it swaps places with the next one (otherwise every swap is a seam)
	int first = 0, second = 1;
	if (materials[1] > materials[0]) { first = 1; second = 0; }
	float third = 0.0;
	for (int i = 2; i < BIOME_COUNT; i++) {
		if (materials[i] > materials[first]) { third = materials[second]; second = first; first = i; }
		else if (materials[i] > materials[second]) { third = materials[second]; second = i; }
		else third = max(third, materials[i]);
	}
	float wFirst = materials[first] - third;
	float wSecond = materials[second] - third;

	// geometric normal (the heightmap's slope at the real height scale: factor world units per unit of height,
	// the differences span two texels) and the small tessellation bumps near the camera
	float heightPerTexel = s.factor / (2.0 * ubo.terrainSize / size.x);
	vec3 va = normalize(vec3(1.0, (hR - hL) * heightPerTexel, 0.0));
	vec3 vb = normalize(vec3(0.0, (hT - hB) * heightPerTexel, 1.0));
	vec3 normal = normalize(cross(vb, va));
	if (s.bumpFade > 0.0) {
		vec2 bumpSlope = terrainDetailGrad(readCoord * HEIGHTMAP_REFERENCE_SIZE);
		float texelWorld = ubo.terrainSize / HEIGHTMAP_REFERENCE_SIZE; // the bumps' texels
		normal = normalize(normal - vec3(bumpSlope.x, 0.0, bumpSlope.y) / texelWorld * s.bumpFade);
	}

	// detail uvs in world units (in the wrapped detail space: small and precise), their gradients from the position's
	vec2 detailPos = s.detailPos;
	vec2 dx = s.dPdx.xz, dy = s.dPdy.xz;
	DetailUV near = DetailUV(detailPos / ubo.textureRepeatNear, dx / ubo.textureRepeatNear, dy / ubo.textureRepeatNear);
	const mat2 farRotation = mat2(0.8, 0.6, -0.6, 0.8); // ~37 degrees, so the two layers never line up
	DetailUV far = DetailUV(farRotation * (detailPos / ubo.textureRepeatFar), farRotation * (dx / ubo.textureRepeatFar), farRotation * (dy / ubo.textureRepeatFar));
	float farWeight = s.farWeight;

	// the macro layer, a much bigger scale in the distance: the far layer's ~18m tile otherwise prints a visible
	// lattice over whole mountain sides
	float macroRepeat = ubo.textureRepeatFar * 7.0;
	const mat2 macroRotation = mat2(0.28, -0.96, 0.96, 0.28); // another ~74 degrees
	float macroWeight = s.macroWeight;
	// a 7x bigger tile reads a 7x sharper mip at the same distance: all that detail is sub pixel out here and the
	// bandwidth isn't (1.6ms), so the macro layer samples as if it were 4x further away
	const float MACRO_MIP_BIAS = 3.0;
	DetailUV macroLayer = DetailUV(macroRotation * (detailPos / macroRepeat), macroRotation * dx / macroRepeat * MACRO_MIP_BIAS, macroRotation * dy / macroRepeat * MACRO_MIP_BIAS);

	// cliffs: the top down projection stretches the textures into long streaks there, so steep ground is
	// textured from the sides instead (triplanar, x and z facing planes), blended by the geometric normal.
	// side uvs use the height directly, it doesn't move when the map re-centres
	vec3 axisSign = sign(normal);
	float y = s.position.y;
	vec2 sideX = vec2(detailPos.y * axisSign.x, y); // plane facing x: (z, y)
	vec2 sideZ = vec2(-detailPos.x * axisSign.z, y); // plane facing z: (x, y)
	vec2 sideXdx = vec2(s.dPdx.z * axisSign.x, s.dPdx.y), sideXdy = vec2(s.dPdy.z * axisSign.x, s.dPdy.y);
	vec2 sideZdx = vec2(-s.dPdx.x * axisSign.z, s.dPdx.y), sideZdy = vec2(-s.dPdy.x * axisSign.z, s.dPdy.y);
	DetailUV sideXNear = DetailUV(sideX / ubo.textureRepeatNear, sideXdx / ubo.textureRepeatNear, sideXdy / ubo.textureRepeatNear);
	DetailUV sideXFar = DetailUV(sideX / ubo.textureRepeatFar, sideXdx / ubo.textureRepeatFar, sideXdy / ubo.textureRepeatFar);
	DetailUV sideZNear = DetailUV(sideZ / ubo.textureRepeatNear, sideZdx / ubo.textureRepeatNear, sideZdy / ubo.textureRepeatNear);
	DetailUV sideZFar = DetailUV(sideZ / ubo.textureRepeatFar, sideZdx / ubo.textureRepeatFar, sideZdy / ubo.textureRepeatFar);
	DetailUV sideXMacro = DetailUV(sideX / macroRepeat, sideXdx / macroRepeat * MACRO_MIP_BIAS, sideXdy / macroRepeat * MACRO_MIP_BIAS);
	DetailUV sideZMacro = DetailUV(sideZ / macroRepeat, sideZdx / macroRepeat * MACRO_MIP_BIAS, sideZdy / macroRepeat * MACRO_MIP_BIAS);
	// (a narrower band where the top and the side projections are both sampled)
	float sideWeight = smoothstep(0.8, 0.66, normal.y) * ubo.terrainTriplanar; // from 0.8: medium slopes stretched the top projection into streaks
	vec2 sideAxes = pow(abs(normal.xz), vec2(6.0));
	sideAxes /= max(sideAxes.x + sideAxes.y, 1e-5);
	float wTop = 1.0 - sideWeight;
	float wX = sideWeight * sideAxes.x, wZ = sideWeight * sideAxes.y;

	// parallax occlusion mapping up close: stones, cracks and ripples get real depth and shade themselves. only
	// on the top projection (fades out on steep ground) and scaled per material (none on grass)
	float viewDistance = s.viewDistance;
	float wA = wFirst / max(wFirst + wSecond, 1e-4);
	// break up the border between two materials with a little world space noise (~10-20m), only inside transitions,
	// so it meanders organically instead of following the smooth climate / slope gradient as a clean line
	float edgeNoise = valueNoise(s.worldXZ * 0.35) * 0.65 + valueNoise(s.worldXZ * 1.3 + 17.0) * 0.35;
	wA = clamp(wA + (edgeNoise - 0.5) * 0.45 * smoothstep(0.0, 0.25, wA * (1.0 - wA)), 0.0, 1.0);
	float pomMaterial = mix(POM_MATERIAL_DEPTH[second], POM_MATERIAL_DEPTH[first], wA);
	// faded over the outer 70% of the range, the depth and the self shadowing go together
	float pomFade = ubo.pomDistance > 0.0 && ubo.pomDepth > 0.0 ? 1.0 - smoothstep(ubo.pomDistance * 0.3, ubo.pomDistance, viewDistance) : 0.0;
	pomFade *= wTop * smoothstep(0.02, 0.1, pomMaterial);
	float pomShadow = 1.0;
	if (pomFade > 0.0) {
		vec3 toEye = normalize(s.campos - s.position);
		vec3 viewTS = vec3(dot(toEye, va), dot(toEye, vb), dot(toEye, normal));
		float scale = ubo.pomDepth * pomMaterial / ubo.textureRepeatNear * pomFade; // uv units
		float depth;
		near.uv = parallaxOcclusion(first, second, wA, near, viewTS, scale, pomFade, depth);

		vec3 light = s.sundir.y > 0.0 ? s.sundir : -s.sundir;
		vec3 lightTS = vec3(dot(light, va), dot(light, vb), dot(light, normal));
		if (ubo.pomShadow > 0.0) pomShadow = mix(1.0, parallaxShadow(first, second, wA, near, near.uv, depth, lightTS, scale), pomFade);
	}

	// rock faces up close: the heightmap is ~9m a texel and the mesh's triangles are no finer, so a cliff was one
	// smooth plastered slope. layered strata (ledges + risers, tilted a little and warped) and vertical erosion
	// flutes in the shading normal, plus banding / creases in the colour (rockStrata, applied after the albedo)
	vec2 heightGrad = vec2(hR - hL, hT - hB) * heightPerTexel; // height per world unit
	RockDetail rockDetail = rockStrata(s.position.y, s.worldXZ, heightGrad, s.dPdx, s.dPdy, s.viewDistance,
									   rock * smoothstep(0.25, 0.6, slope) * (1.0 - snowCap));
	// every projection that has weight here: the two materials height blended, the normal map turned into world
	// space (whiteout blend for the side planes, see Golus' triplanar normal mapping notes)
	vec3 albedoSum = vec3(0.0), normalSum = vec3(0.0);
	float wSum = 0.0;
	if (wTop > 0.02) {
		vec3 a, n;
		sampleBlend(first, second, wA, near, far, macroLayer, farWeight, macroWeight, a, n);
		strataBlend(rockDetail, near, far, macroLayer, farWeight, macroWeight, a, n);
		albedoSum += a * wTop;
		normalSum += normalize(mat3(va, vb, normal) * n) * wTop;
		wSum += wTop;
	}
	if (wX > 0.02) {
		vec3 a, n;
		sampleBlend(first, second, wA, sideXNear, sideXFar, sideXMacro, farWeight, macroWeight, a, n);
		strataBlend(rockDetail, sideXNear, sideXFar, sideXMacro, farWeight, macroWeight, a, n);
		n.x *= axisSign.x;
		vec3 t = vec3(n.xy + normal.zy, abs(n.z) * normal.x);
		albedoSum += a * wX;
		normalSum += normalize(t.zyx) * wX;
		wSum += wX;
	}
	if (wZ > 0.02) {
		vec3 a, n;
		sampleBlend(first, second, wA, sideZNear, sideZFar, sideZMacro, farWeight, macroWeight, a, n);
		strataBlend(rockDetail, sideZNear, sideZFar, sideZMacro, farWeight, macroWeight, a, n);
		n.x *= -axisSign.z;
		vec3 t = vec3(n.xy + normal.xy, abs(n.z) * normal.z);
		albedoSum += a * wZ;
		normalSum += normalize(t) * wZ;
		wSum += wZ;
	}
	normal = normalize(normalSum);

	if (rockDetail.weight > 0.0) {
		vec3 n = normal / max(normal.y, 0.2); // as a height field's (-dx, 1, -dz), the detail's slope adds on
		normal = normalize(n - vec3(rockDetail.slope.x, 0.0, rockDetail.slope.y));
	}

	vec3 albedo = pow(albedoSum / max(wSum, 1e-4), vec3(2.2)); // textures are srgb, light in linear

	// large scale brightness / tint variation so big areas of one material don't look copy pasted
	float macro = valueNoise(readCoord * 60.0) * 0.6 + lineNoise * 0.4;
	albedo *= mix(vec3(0.78, 0.8, 0.84), vec3(1.12, 1.1, 1.04), macro);
	// the land's shape in its colour: gullies darker (shaded, wetter, debris), ridges lighter (weathered), strongest
	// on bare rock. far away this is most of what tells a mountainside's structure apart
	float rockiness = clamp(rock + weights[BIOME_MOUNTAINS] * 0.5, 0.0, 1.0) * (1.0 - snowCap);
	albedo *= 1.0 - gully * mix(0.12, 0.28, rockiness) + ridge * mix(0.05, 0.14, rockiness);
	// stone is grey: the rock texture's brown read as mud on whole mountainsides
	float rockLuma = dot(albedo, vec3(0.3, 0.59, 0.11));
	albedo = mix(albedo, rockLuma * vec3(0.96, 0.97, 1.02), rockiness * 0.55 * (1.0 - rockDetail.materialMix * 0.7)); // (the strata keep their colours)
	albedo *= mix(vec3(1.0), rockDetail.tint, rockDetail.weight);
	// grass: a little less saturated, with drier yellowish patches (one flat bright green looked painted)
	float grassiness = clamp(weights[BIOME_PLAINS] + weights[BIOME_TAIGA] * 0.5, 0.0, 1.0) * (1.0 - snowCap);
	float grassLuma = dot(albedo, vec3(0.3, 0.59, 0.11));
	albedo = mix(albedo, mix(vec3(grassLuma), albedo, 0.8), grassiness);
	// the meadow ground leans towards the grass blades' own colour, so where the grass rings thin out (or end) the
	// ground underneath doesn't show as a paler band
	albedo = mix(albedo, vec3(0.085, 0.15, 0.035) * (0.7 + 0.6 * dot(albedo, vec3(0.3, 0.59, 0.11)) / 0.12), grassiness * 0.35);
	float dryPatches = smoothstep(0.45, 0.8, valueNoise(snowPos * 0.08 + 31.0) * 0.7 + valueNoise(snowPos * 0.4) * 0.3);
	albedo *= mix(vec3(1.0), vec3(1.12, 1.04, 0.72), dryPatches * grassiness * 0.6);

	// wheat canopy: past the foliage rings (~108m, the cards shrink away from 13.5 world units) a field would be
	// bare soil, so the ground itself turns into the tops of the plants. the oat atlas' average colour and some
	// patchiness (the caller adds the wind gusts rolling over it, canopyGust: they move, the cache can't hold them)
	// only where foliage.comp actually grows the fields: gentle slopes (< 0.3, it uses the same gradient), no rock,
	// no snow, not up on the mountains. up close (under the plants) the floor is straw / stubble, not black soil:
	// partly tinted (canopyNear). further out it's the canopy itself, nearly all of it (at half strength the soil
	// showed through as a dark muddy band between the near plants and the distance)
	float canopy = wheatField *(1.0 - smoothstep(0.2, 0.3, slope)) * (1.0 - rock) * (1.0 - snowCap)
				 * (1.0 - step(0.4, mountains) * step(0.56, terrainHeight)) * s.canopyNear;
	if (canopy > 0.0) {
		float patches = valueNoise(s.worldXZ * 0.35) * 0.6 + valueNoise(s.worldXZ * 2.1) * 0.4;
		vec3 straw = vec3(0.46, 0.33, 0.11) * 0.85; // foliage.frag's WHEAT_GOLD, a little darker (the canopy's depth)
		straw *= mix(vec3(0.82, 0.85, 0.75), vec3(1.1, 1.05, 1.0), patches);
		albedo = mix(albedo, straw, canopy);
	}

	TerrainSurface surface;
	surface.albedo = albedo;
	surface.normal = normal;
	surface.canopy = canopy;
	surface.pomShadow = pomShadow;
	return surface;
}

// the wind gusts over the wheat canopy, a brightness factor: foliage.vert's wave, but bent by the patch noise so
// the bands don't run as straight stripes to the horizon, and softer (mostly a hint of motion this far out)
float canopyGust(vec2 worldXZ, float time) {
	vec2 windDir = normalize(vec2(0.8, 0.35)); // foliage.vert
	float patches = valueNoise(worldXZ * 0.35) * 0.6 + valueNoise(worldXZ * 2.1) * 0.4;
	float gust = sin(time * 1.3 + dot(worldXZ, windDir) * 2.5 + patches * 6.0) * 0.5 + 0.5;
	return mix(0.94, 1.04, gust);
}

#endif
