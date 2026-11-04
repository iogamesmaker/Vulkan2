// the terrain's surface height, shared by the tessellation, the foliage / tree scatter passes and the walking
// probe so everything sits exactly on the same surface (no floating grass or trees, no walking through hills)
//   - bicubic (b-spline) heightmap sampling, smooth instead of the faceted bilinear look up close
//   - small procedural bumps near the camera for detail below the heightmap's 1.5 unit texels
//   - the same distance based prefilter the terrain mesh uses, so far away objects match its coarse LOD
#ifndef TERRAIN_HEIGHT_GLSL
#define TERRAIN_HEIGHT_GLSL

#include "heightmapscale.glsl"

#define TERRAIN_PATCHES 128.0 // patches per side, must match Application::initTerrainPatches
#define DETAIL_AMPLITUDE 0.12 // world units (~70cm)
#define DETAIL_FADE_START 30.0
#define DETAIL_FADE_END 120.0

// 4 bilinear taps instead of 16 point samples (GPU Gems 2, chapter 20)
float sampleHeightBicubic(sampler2D hm, vec2 uv) {
	vec2 size = vec2(textureSize(hm, 0));
	vec2 coord = uv * size - 0.5;
	vec2 f = fract(coord);
	coord -= f;

	vec2 f2 = f * f, f3 = f2 * f;
	vec2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
	vec2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
	vec2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
	vec2 w3 = f3 / 6.0;

	vec2 s0 = w0 + w1, s1 = w2 + w3;
	vec2 t0 = (coord - 0.5 + w1 / s0) / size; // between the centres of texels -1 and 0
	vec2 t1 = (coord + 1.5 + w3 / s1) / size; // between the centres of texels 1 and 2

	return (textureLod(hm, vec2(t0.x, t0.y), 0.0).a * s0.x + textureLod(hm, vec2(t1.x, t0.y), 0.0).a * s1.x) * s0.y
		 + (textureLod(hm, vec2(t0.x, t1.y), 0.0).a * s0.x + textureLod(hm, vec2(t1.x, t1.y), 0.0).a * s1.x) * s1.y;
}

float detailHash(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float detailValueNoise(vec2 p) {
	vec2 i = floor(p), f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(detailHash(i), detailHash(i + vec2(1, 0)), u.x), mix(detailHash(i + vec2(0, 1)), detailHash(i + vec2(1, 1)), u.x), u.y);
}

// how much of a noise octave the mesh can carry: its lattice (texels) against the triangle spacing (texels). an
// octave finer than ~2 triangles across only gets point sampled at the vertices, which turned the bumps into sharp
// spikes further out (the fine octave's lattice is ~0.7 units, the triangles there ~1)
float detailOctaveWeight(float lattice, float spacing) {
	return 1.0 - smoothstep(0.3 * lattice, 0.6 * lattice, spacing);
}

// small bumps in world units, p = heightmap texel coordinates of the 4096 map (continuous across map re-centering).
// spacing = the mesh's triangle size in those texels (0 = all of it: the probe, the shading)
float terrainDetail(vec2 texel, float spacing) {
	vec2 p = texel * 0.5;
	float n = (detailValueNoise(p) - 0.5) * 0.55 * detailOctaveWeight(2.0, spacing)
			+ (detailValueNoise(p * 2.13 + 17.0) - 0.5) * 0.3 * detailOctaveWeight(2.0 / 2.13, spacing)
			+ (detailValueNoise(p * 4.37 - 9.0) - 0.5) * 0.15 * detailOctaveWeight(2.0 / 4.37, spacing);
	return n * 2.0 * DETAIL_AMPLITUDE;
}

// value noise with its analytic gradient
vec2 detailValueNoiseGrad(vec2 p) {
	vec2 i = floor(p), f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f), du = 6.0 * f * (1.0 - f);
	float a = detailHash(i), b = detailHash(i + vec2(1, 0)), c = detailHash(i + vec2(0, 1)), d = detailHash(i + vec2(1, 1));
	return du * (vec2(b - a, c - a) + (a - b - c + d) * u.yx);
}

// the slope of terrainDetail (height per texel), for the shading normal: 3 noise evaluations instead of the 9 a
// finite difference takes. the octaves are scaled like the 0.25 texel difference used to (it averaged the finest
// one over half its period), so the bumps look the same
vec2 terrainDetailGrad(vec2 texel) {
	vec2 p = texel * 0.5;
	vec2 g = detailValueNoiseGrad(p) * (0.55 * 0.97)
		   + detailValueNoiseGrad(p * 2.13 + 17.0) * (0.3 * 2.13 * 0.89)
		   + detailValueNoiseGrad(p * 4.37 - 9.0) * (0.15 * 4.37 * 0.58);
	return g * 0.5 * 2.0 * DETAIL_AMPLITUDE;
}

float detailFade(float dist) {
	return 1.0 - smoothstep(DETAIL_FADE_START, DETAIL_FADE_END, dist);
}

// world space height (terrain space y) at heightmap uv. dist = horizontal distance to the camera,
// spacing = triangle size the terrain mesh has there (world units), 0 for no prefiltering
float terrainHeight(sampler2D hm, vec2 uv, float dist, float spacing, float factor, float terrainSize) {
	vec2 size = vec2(textureSize(hm, 0));
	float height = sampleHeightBicubic(hm, uv);

	// average over about a triangle once triangles are bigger than a texel (coarse LODs, see terrain.tese)
	float radius = 0.5 * spacing / terrainSize;
	float filterWeight = smoothstep(0.75, 1.5, radius * size.x);
	if (filterWeight > 0.0) {
		float filtered = height * 0.5
					   + 0.125 * (textureLod(hm, uv + vec2(radius, 0.0), 0.0).a + textureLod(hm, uv - vec2(radius, 0.0), 0.0).a
								+ textureLod(hm, uv + vec2(0.0, radius), 0.0).a + textureLod(hm, uv - vec2(0.0, radius), 0.0).a);
		height = mix(height, filtered, filterWeight);
	}

	float y = height * factor - factor * 0.5;
	float fade = detailFade(dist);
	// the same bumps at any heightmap size, band limited to what the mesh's triangles here can show. everything that
	// sits on the terrain (grass, rocks, trees, shadows) passes the same spacing, so it stays on the same surface
	float spacingTexels = spacing / (terrainSize / HEIGHTMAP_REFERENCE_SIZE);
	if (fade > 0.0) y += terrainDetail(uv * HEIGHTMAP_REFERENCE_SIZE, spacingTexels) * fade;
	return y;
}

// how far the terrain mesh's flat triangles (spacing apart, world units) sit under the curved surface here: on a
// rounded hilltop the mesh is a chord below the arc, and anything placed on the true surface floats above what's
// drawn (most at low tessellation, where the triangles are big). the bulge of this point over the average a half
// triangle away, only on convex ground (in a dip the mesh is above, things sink in a little, which doesn't show)
float meshSag(sampler2D hm, vec2 uv, float spacing, float factor, float terrainSize) {
	if (spacing <= 0.0) return 0.0;
	float r = 0.5 * spacing / terrainSize;
	float centre = textureLod(hm, uv, 0.0).a;
	float around = 0.25 * (textureLod(hm, uv + vec2(r, 0.0), 0.0).a + textureLod(hm, uv - vec2(r, 0.0), 0.0).a
						 + textureLod(hm, uv + vec2(0.0, r), 0.0).a + textureLod(hm, uv - vec2(0.0, r), 0.0).a);
	return max(centre - around, 0.0) * factor;
}

// terrain space xz -> heightmap uv
vec2 terrainUV(vec2 xz, ivec2 offset, float terrainSize, vec2 heightmapSize) {
	return (xz + 0.5 * terrainSize) / terrainSize + vec2(offset) / heightmapSize;
}

// the triangle size the terrain mesh uses at a distance (see terrain.tesc), lodPixels = 40 / tessellation factor
float terrainSpacing(float dist, float lodPixels, float projScale) {
	return max(dist, 1.0) * lodPixels / projScale;
}

#endif
