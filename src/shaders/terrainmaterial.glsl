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

// the near layer without its repeat on the cliffs' side projections (Quilez' "texture repetition", technique 3):
// a 7m tile printed a lattice all over a rock face. a slow noise over the uv picks one of 8 random offsets into the
// texture per patch, two neighbouring offsets cross fade where the noise moves from one to the next (the contrast
// decides who wins there, less ghosting than a plain fade). the fade is narrow: outside it one offset is sampled,
// only inside it both (~1ms on a screen full of cliffs; a wide fade, and the far / macro layers too, cost ~6)
// not on the top projection: the parallax traces the near layer's height as it is. ubo.cliffNoTile turns it off
// noiseScale: patches per tile, the noise has to repeat where the detail space wraps (TERRAIN_DETAIL_PERIOD, 875
// near tiles): 0.6
void sampleOffset(inout Material m, int index, DetailUV layer, vec2 offset, float weight, bool needHeight) {
	addLayer(m, index, DetailUV(layer.uv + offset, layer.dx, layer.dy), weight, needHeight);
}
void addLayerNoTile(inout Material m, int index, DetailUV layer, float weight, bool needHeight, float noiseScale) {
	float k = valueNoise(layer.uv * noiseScale) * 8.0;
	float i = floor(k), f = fract(k);
	vec2 offsetA = sin(vec2(3.0, 7.0) * i), offsetB = sin(vec2(3.0, 7.0) * (i + 1.0));
	// (the contrast shifts the fade by up to 0.15 either way. a narrow band: it's on all the ground now, inside it
	// everything is sampled twice)
	if (f < 0.36) { sampleOffset(m, index, layer, offsetA, weight, needHeight); return; }
	if (f > 0.64) { sampleOffset(m, index, layer, offsetB, weight, needHeight); return; }
	Material a, b;
	a.albedo = vec3(0.0); a.normal = vec3(0.0); a.height = 0.0;
	b = a;
	sampleOffset(a, index, layer, offsetA, 1.0, needHeight);
	sampleOffset(b, index, layer, offsetB, 1.0, needHeight);
	float t = smoothstep(0.4, 0.6, f - 0.05 * dot(a.albedo - b.albedo, vec3(1.0)));
	m.albedo += mix(a.albedo, b.albedo, t) * weight;
	m.normal += mix(a.normal, b.normal, t) * weight;
	m.height += mix(a.height, b.height, t) * weight;
}

// the top projection's anti-tiling, decided once per pixel from where its uv is BEFORE the parallax shifts it
// (topNoTileBegin): the two offsets and the cross fade between them. the parallax traces its height with exactly
// these and the textures are sampled with them too, so the two agree. picked per sample instead, the textures cross
// faded where the traced height didn't: the relief and the texture slid apart along every patch border
bool g_TopNoTile = false;
vec2 g_TopA = vec2(0.0), g_TopB = vec2(0.0);
float g_TopT = 0.0;
void topNoTileBegin(vec2 uv) {
	float k = valueNoise(uv * 0.6) * 8.0;
	float i = floor(k), f = fract(k);
	g_TopA = sin(vec2(3.0, 7.0) * i);
	g_TopB = sin(vec2(3.0, 7.0) * (i + 1.0));
	g_TopT = smoothstep(0.36, 0.64, f);
	g_TopNoTile = true;
}
void addLayerPair(inout Material m, int index, DetailUV layer, float weight, bool needHeight) {
	if (g_TopT < 0.001) { sampleOffset(m, index, layer, g_TopA, weight, needHeight); return; }
	if (g_TopT > 0.999) { sampleOffset(m, index, layer, g_TopB, weight, needHeight); return; }
	sampleOffset(m, index, layer, g_TopA, weight * (1.0 - g_TopT), needHeight);
	sampleOffset(m, index, layer, g_TopB, weight * g_TopT, needHeight);
}
// a height map read the same way (ubo.cliffNoTile off: plain)
float pairHeight(int index, vec2 uv, vec2 dx, vec2 dy) {
	if (!g_TopNoTile) return textureGrad(pom[nonuniformEXT(index)], uv, dx, dy).r;
	float a = g_TopT < 0.999 ? textureGrad(pom[nonuniformEXT(index)], uv + g_TopA, dx, dy).r : 0.0;
	float b = g_TopT > 0.001 ? textureGrad(pom[nonuniformEXT(index)], uv + g_TopB, dx, dy).r : 0.0;
	return mix(a, b, g_TopT);
}

Material sampleMaterial(int index, DetailUV near, DetailUV far, DetailUV macro, float farWeight, float macroWeight, bool needHeight, bool nearNoTile) {
	Material m;
	m.albedo = vec3(0.0); m.normal = vec3(0.0); m.height = 0.0;
	float wNear = 1.0 - farWeight, wFar = farWeight * (1.0 - macroWeight), wMacro = farWeight * macroWeight;
	if (wNear > 0.0) {
		if (nearNoTile && g_TopNoTile) addLayerPair(m, index, near, wNear, needHeight);
		else if (nearNoTile) addLayerNoTile(m, index, near, wNear, needHeight, 0.6);
		else addLayer(m, index, near, wNear, needHeight);
	}
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

// heightBlend for a pair of materials. snow is the exception: it has no relief of its own (it lies smooth and fills
// the cracks), and the soft band showed the grass / rock through it like frosted glass, a milky film with a bright
// rim around every patch. against snow the other material's own bumps decide (tufts and stone tops poke out first)
// and the band is narrow, so a pixel is snow or ground, not a see-through mix of both
float materialBlend(int first, int second, float h1, float h2, float wFirst) {
	// turf against bare rock: a deep blend where the rock's relief decides (its bumps come up through thinning turf,
	// the turf holds on in its cracks) over the whole band, instead of a cut edge with an outline
	if ((first == BIOME_MOUNTAINS) != (second == BIOME_MOUNTAINS) && first != BIOME_SNOW && second != BIOME_SNOW) {
		float hRock = first == BIOME_MOUNTAINS ? h1 : h2, hOther = first == BIOME_MOUNTAINS ? h2 : h1;
		float wRock = first == BIOME_MOUNTAINS ? wFirst : 1.0 - wFirst;
		float a1 = hRock * 0.9 + wRock, a2 = hOther * 0.3 + 1.0 - wRock;
		float level = max(a1, a2) - 0.45;
		float bRock = max(a1 - level, 0.0), bOther = max(a2 - level, 0.0);
		float rockShare = bRock / max(bRock + bOther, 1e-4);
		// (back to the plain winner towards the cutoff: past it only one of the two is sampled, no pop at the switch)
		float inBand = smoothstep(HEIGHT_BLEND_CUTOFF, HEIGHT_BLEND_CUTOFF + 0.12, wRock) * smoothstep(HEIGHT_BLEND_CUTOFF, HEIGHT_BLEND_CUTOFF + 0.12, 1.0 - wRock);
		rockShare = mix(step(0.5, wRock), rockShare, inBand);
		return first == BIOME_MOUNTAINS ? 1.0 - rockShare : rockShare;
	}
	if (first != BIOME_SNOW && second != BIOME_SNOW) return heightBlend(h1, h2, wFirst);
	if (first == BIOME_SNOW) h1 = mix(0.45, h1, 0.2); else h2 = mix(0.45, h2, 0.2);
	const float blendDepth = 0.06;
	float a1 = h1 * 0.6 + wFirst;
	float a2 = h2 * 0.6 + 1.0 - wFirst;
	float level = max(a1, a2) - blendDepth;
	float b1 = max(a1 - level, 0.0);
	float b2 = max(a2 - level, 0.0);
	return b2 / max(b1 + b2, 1e-4);
}

// snow is a layer over the ground, not one of the two ranked materials: ranked, it won outright wherever the two
// ground materials under it swapped places (both had the third strongest taken off, the ground went to zero), a
// thin white line along every such swap, rings and ovals in the grass at the snowline. 0..1, terrainSurface sets it
float g_SnowOver = 0.0;
// the forest floor the same way, under the snow: fallen leaves under broadleaf trees, needles under conifers, where
// the canopy map (horizonao.comp, g) says the crowns are. 0..1 and which texture (TERRAIN_LEAF_LITTER / _NEEDLES)
float g_LitterOver = 0.0;
int g_LitterKind = 10;
#define TERRAIN_LEAF_LITTER 10 // Application::loadTextures: Poly Haven forest_leaves_02
#define TERRAIN_NEEDLES 11	   // forrest_ground_03
// the litter photos cover ~2m, the terrain's near tile is ~3.6m: at that size the leaves smeared into blobs
const float LITTER_SCALE = 1.8;
DetailUV litterUV(DetailUV d) { return DetailUV(d.uv * LITTER_SCALE, d.dx * LITTER_SCALE, d.dy * LITTER_SCALE); }

// the two ground materials of one projection, height blended (one alone where the other can't show, most pixels),
// then the snow over them, height blended against the ground's relief
void sampleBlend(int first, int second, float wA, DetailUV near, DetailUV far, DetailUV macro, float farWeight, float macroWeight, bool nearNoTile, out vec3 albedo, out vec3 normal) {
	float snow = g_SnowOver;
	if (snow > 1.0 - HEIGHT_BLEND_CUTOFF) {
		Material m = sampleMaterial(BIOME_SNOW, near, far, macro, farWeight, macroWeight, false, nearNoTile);
		albedo = m.albedo; normal = normalize(m.normal);
		return;
	}
	bool snowEdge = snow >= HEIGHT_BLEND_CUTOFF;
	float litter = g_LitterOver;
	bool litterEdge = litter >= HEIGHT_BLEND_CUTOFF && litter <= 1.0 - HEIGHT_BLEND_CUTOFF;
	float groundHeight;
	if (litter > 1.0 - HEIGHT_BLEND_CUTOFF) {
		Material m = sampleMaterial(g_LitterKind, litterUV(near), litterUV(far), macro, farWeight, macroWeight, snowEdge, nearNoTile);
		albedo = m.albedo; normal = normalize(m.normal);
		groundHeight = m.height;
	} else if (wA > 1.0 - HEIGHT_BLEND_CUTOFF || wA < HEIGHT_BLEND_CUTOFF) {
		Material m = sampleMaterial(wA > 0.5 ? first : second, near, far, macro, farWeight, macroWeight, snowEdge || litterEdge, nearNoTile);
		albedo = m.albedo; normal = normalize(m.normal);
		groundHeight = m.height;
	} else {
		Material m1 = sampleMaterial(first, near, far, macro, farWeight, macroWeight, true, nearNoTile);
		Material m2 = sampleMaterial(second, near, far, macro, farWeight, macroWeight, true, nearNoTile);
		float blend = materialBlend(first, second, m1.height, m2.height, wA); // 0 = first, 1 = second
		albedo = mix(m1.albedo, m2.albedo, blend);
		normal = normalize(mix(m1.normal, m2.normal, blend));
		groundHeight = mix(m1.height, m2.height, blend);
	}
	if (litterEdge) {
		// leaves and needles pile in the hollows first, the ground's bumps poke through (the plain height blend)
		Material l = sampleMaterial(g_LitterKind, litterUV(near), litterUV(far), macro, farWeight, macroWeight, true, nearNoTile);
		float blend = heightBlend(groundHeight, l.height, 1.0 - litter);
		albedo = mix(albedo, l.albedo, blend);
		normal = normalize(mix(normal, l.normal, blend));
		groundHeight = mix(groundHeight, l.height, blend);
	}
	if (snowEdge) {
		Material s = sampleMaterial(BIOME_SNOW, near, far, macro, farWeight, macroWeight, true, nearNoTile);
		float blend = materialBlend(-1, BIOME_SNOW, groundHeight, s.height, 1.0 - snow);
		albedo = mix(albedo, s.albedo, blend);
		normal = normalize(mix(normal, s.normal, blend));
	}
}

// the height of the blended surface here, with exactly the blend the textures get, so the parallax traces the
// same surface that ends up textured
float groundHeight(int first, int second, float wFirst, DetailUV near, vec2 uv) {
	if (wFirst > 1.0 - HEIGHT_BLEND_CUTOFF) return pairHeight(first, uv, near.dx, near.dy);
	if (wFirst < HEIGHT_BLEND_CUTOFF) return pairHeight(second, uv, near.dx, near.dy);
	float h1 = pairHeight(first, uv, near.dx, near.dy);
	float h2 = pairHeight(second, uv, near.dx, near.dy);
	return mix(h1, h2, materialBlend(first, second, h1, h2, wFirst));
}
float blendedHeight(int first, int second, float wFirst, DetailUV near, vec2 uv) {
	float snow = g_SnowOver;
	if (snow > 1.0 - HEIGHT_BLEND_CUTOFF) return pairHeight(BIOME_SNOW, uv, near.dx, near.dy);
	float litter = g_LitterOver;
	float h;
	if (litter > 1.0 - HEIGHT_BLEND_CUTOFF) h = pairHeight(g_LitterKind, uv * LITTER_SCALE, near.dx * LITTER_SCALE, near.dy * LITTER_SCALE);
	else {
		h = groundHeight(first, second, wFirst, near, uv);
		if (litter >= HEIGHT_BLEND_CUTOFF) {
			float hl = pairHeight(g_LitterKind, uv * LITTER_SCALE, near.dx * LITTER_SCALE, near.dy * LITTER_SCALE);
			h = mix(h, hl, heightBlend(h, hl, 1.0 - litter));
		}
	}
	if (snow >= HEIGHT_BLEND_CUTOFF) {
		float hs = pairHeight(BIOME_SNOW, uv, near.dx, near.dy);
		h = mix(h, hs, materialBlend(-1, BIOME_SNOW, h, hs, 1.0 - snow));
	}
	return h;
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

// ------------------------------------------------------------------ the surface

struct TerrainShading {
	vec3 position;		// terrain space (y = height): its y and its distance to campos matter
	vec3 dPdx, dPdy;	// the position's derivatives over a pixel (terrain.frag) / a cache texel (terraincache.comp)
	vec2 readCoord;		// heightmap uv (wraps)
	vec2 detailPos;		// xz in the detail textures' space (wraps at TERRAIN_DETAIL_PERIOD)
	vec2 detailLocal;	// the same split in two: small per pixel (the position near the camera) + the same for every
	vec2 detailOffset;	// pixel. the uvs are built from the two (detailUV), see there
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
	float directScale; // the parallax' self shadow (G-buffer's direct light scale), 1 = lit
};

// the parallax' self shadow: from the point the view ray hit (uv, its depth below the top) up towards the light
// through the same height field. soft: how far under the surface the light ray passes, more weight close to the
// point (pebbles shade their own foot, a ridge further off only darkens). returns 1 lit .. ~0.45 in shadow (the sky still lights it)
float parallaxShadow(int first, int second, float wFirst, DetailUV near, vec2 uv, float depth, vec3 lightTS, float scale) {
	if (lightTS.z <= 0.02 || depth <= 0.0) return 1.0;
	const int STEPS = 8;
	float stepDepth = depth / float(STEPS);
	vec2 delta = lightTS.xy / max(lightTS.z, 0.2) * scale * stepDepth; // (capped like the view ray at grazing light)
	float occlusion = 0.0;
	for (int i = 1; i <= STEPS; i++) {
		float rayDepth = depth - stepDepth * float(i);
		float surfaceDepth = 1.0 - blendedHeight(first, second, wFirst, near, uv + delta * float(i));
		occlusion = max(occlusion, (rayDepth - surfaceDepth) * (1.0 - float(i - 1) / float(STEPS)) * 3.5);
	}
	return 1.0 - 0.55 * clamp(occlusion, 0.0, 1.0);
}

// bare rock: a photographed cliff face (Poly Haven CC0 cliff_side) at its real size over the base
// rock texture, the real structure of a cliff (blocks, joints, ledges) where a texture tiling every 7m and the
// smooth 9m heightmap made a plastered slope. and in big slow patches other kinds of rock (buff, dark slate, rust
// stained), so a mountainside isn't all one stone. no procedural relief: generated ledges / bands looked fake
#define TERRAIN_ROCK_KINDS_FIRST 6 // buff, dark, rust (Application::loadTextures)
#define TERRAIN_CLIFF 9	   // cliff_side
#define CLIFF_SIZE 3.05	   // world units the photo covers (18.3m)

struct RockCover {
	float cliffMix; // how much of the cliff photo over the base rock
	float cliffBig; // its second, bigger and shifted copy's share (no 18m repeat across a whole face)
	int kind;		// which other rock
	float kindMix;	// how much of it over the cliff / base rock
};

// mask = how much bare rock this is. every choice comes from its own slow noise, so a mountainside changes stone
// every 50-150m: the cliff photo in about half of it, the other rocks in their own patches over it (they tint the
// cliff's structure, they don't hide it), the base rock where neither is
RockCover rockCover(vec2 worldXZ, float mask) {
	RockCover r;
	r.cliffMix = mask * 0.85 * smoothstep(0.38, 0.6, detailValueNoise(worldXZ * 0.03 + 41.0));
	r.cliffBig = smoothstep(0.3, 0.7, detailValueNoise(worldXZ * 0.05 - 71.0));
	float k = detailValueNoise(worldXZ * 0.035 - 13.0) * 3.0;
	float within = fract(k);
	r.kind = TERRAIN_ROCK_KINDS_FIRST + min(int(k), 2);
	r.kindMix = mask * 0.6 * smoothstep(0.0, 0.2, within) * (1.0 - smoothstep(0.8, 1.0, within))
			  * smoothstep(0.35, 0.6, detailValueNoise(worldXZ * 0.06 + 7.0));
	return r;
}

DetailUV scaleUV(DetailUV d, float k) { return DetailUV(d.uv * k, d.dx * k, d.dy * k); }

// the cliff photo, then the other rock, over one projection's base material. near = that projection's near layer
// (uv = world / textureRepeatNear)
void rockBlend(RockCover r, DetailUV near, DetailUV far, DetailUV macro, float farWeight, float macroWeight, bool nearNoTile, inout vec3 albedo, inout vec3 normal) {
	if (r.cliffMix > 0.01) {
		vec3 cAlbedo = vec3(0.0), cNormal = vec3(0.0);
		for (int i = 0; i < 2; i++) {
			float w = i == 0 ? 1.0 - r.cliffBig : r.cliffBig;
			if (w < 0.01) continue;
			float size = CLIFF_SIZE * (i == 0 ? 1.0 : 1.7);
			DetailUV cliffNear = scaleUV(near, ubo.textureRepeatNear / size);
			cliffNear.uv += i == 0 ? vec2(0.0) : vec2(0.37, 0.61); // a different part of the photo
			DetailUV cliffFar = scaleUV(cliffNear, 1.0 / 1.6); // a bigger tile in the distance
			Material m = sampleMaterial(TERRAIN_CLIFF, cliffNear, cliffFar, cliffFar, farWeight, 0.0, false, false); // (its two copies vary it already)
			cAlbedo += m.albedo * w;
			cNormal += normalize(m.normal) * w;
		}
		// mostly its light and shade: cliff_side is canyon orange, the mountains here are grey stone
		float luma = dot(cAlbedo, vec3(0.3, 0.59, 0.11));
		vec3 cliff = mix(vec3(luma) * vec3(1.02, 1.0, 0.97), cAlbedo, 0.18);
		// most of it up close, where its blocks and joints show; further out the base rock's streaks carry the shape
		float mixHere = r.cliffMix * mix(1.0, 0.45, farWeight);
		albedo = mix(albedo, cliff, mixHere);
		normal = normalize(mix(normal, normalize(cNormal), mixHere));
	}
	if (r.kindMix > 0.01) {
		Material m = sampleMaterial(r.kind, near, far, macro, farWeight, macroWeight, false, false); // (a tint over patches)
		vec3 rock = mix(vec3(dot(m.albedo, vec3(0.3, 0.59, 0.11))), m.albedo, 0.65); // weathered, less saturated
		albedo = mix(albedo, rock, r.kindMix);
		normal = normalize(mix(normal, normalize(m.normal), r.kindMix * 0.6)); // the cliff's relief stays
	}
}

// the heightmap bilinearly filtered at full precision (the hardware's filter weights are only ~8 bit), wrapping
// like the ring buffer's REPEAT sampler
float heightmapExact(vec2 uv) {
	ivec2 size = textureSize(heightmap, 0);
	vec2 coord = uv * vec2(size) - 0.5;
	vec2 f = fract(coord);
	ivec2 i = ivec2(floor(coord));
	ivec2 i0 = (i % size + size) % size, i1 = ((i + 1) % size + size) % size;
	float a = texelFetch(heightmap, ivec2(i0.x, i0.y), 0).a, b = texelFetch(heightmap, ivec2(i1.x, i0.y), 0).a;
	float c = texelFetch(heightmap, ivec2(i0.x, i1.y), 0).a, d = texelFetch(heightmap, ivec2(i1.x, i1.y), 0).a;
	return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

TerrainSurface shadeTerrain(TerrainShading s) {
	vec2 size = vec2(textureSize(heightmap, 0));
	vec2 readCoord = s.readCoord;

	// (exact filtering: the slope and the normal are differences of nearly equal heights, scaled up a lot)
	float hR = heightmapExact(readCoord + vec2(1.0, 0.0) / size);
	float hL = heightmapExact(readCoord - vec2(1.0, 0.0) / size);
	float hT = heightmapExact(readCoord + vec2(0.0, 1.0) / size);
	float hB = heightmapExact(readCoord - vec2(0.0, 1.0) / size);

	float dzdx = (hR - hL) * heightmapSlopeScale(size.x);
	float dzdy = (hT - hB) * heightmapSlopeScale(size.x);
	float slope = dzdx * dzdx + dzdy * dzdy;

	vec4 terrainData = vec4(textureLod(climateMap, readCoord, 0.0).rgb, heightmapExact(readCoord)); // climate (half resolution), height
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
		// exact bilinear (texel fetches, filtered in fp32): the difference of nearly equal heights times ~160 amplified
		// the hardware filter's coarse sub-texel weights into blocky steps in the snow cover
		vec2 curvatureStep = 3.0 / size;
		float around = heightmapExact(readCoord + vec2(curvatureStep.x, 0.0)) + heightmapExact(readCoord - vec2(curvatureStep.x, 0.0))
					 + heightmapExact(readCoord + vec2(0.0, curvatureStep.y)) + heightmapExact(readCoord - vec2(0.0, curvatureStep.y));
		curvature = (around * 0.25 - terrainHeight) * s.factor / (3.0 * ubo.terrainSize / size.x);
	}
	float gully = smoothstep(0.01, 0.12, curvature), ridge = smoothstep(-0.01, -0.12, curvature);

	// bare rock on cliffs: in the lowlands only where it's really steep (the erosion's small scarps on the grassy hills
	// were all rock streaks), the mountains from a gentler slope
	// (a finer world noise on the slope too and a wider ramp: the grass patches between the rock had the smooth
	// outline of the slope's contour, lime blobs painted on the scree)
	// (slow noise only, and a wide ramp: noise added to the slope makes the threshold flip over a short distance, a
	// fine one cut the grass out of the rock like paper; the wide ramp gives the material blend room to fade)
	float rockFray = valueNoise(s.worldXZ * 0.35 + 3.0);
	float rockSlope = slope + (lineNoise - 0.5) * 0.25 + (rockFray - 0.5) * 0.2;
	float steepRock = smoothstep(mix(0.55, 0.2, mountains), mix(1.6, 1.0, mountains), rockSlope);
	steepRock *= 1.0 - weights[BIOME_DESERT] * (1.0 - mountains); // dune and wadi walls are sand, not stone
	float rockline = 0.58 + (lineNoise - 0.5) * 0.12;
	float highRock = smoothstep(rockline, rockline + 0.06, terrainHeight) * mountains;
	float rock = max(steepRock, highRock);
	for (int i = 0; i < BIOME_COUNT; i++) weights[i] *= 1.0 - rock;
	weights[BIOME_MOUNTAINS] += rock;

	// snow on the upper half of most mountains (lower where it's cold), only the steepest cliffs stay bare
	float snowline = snowlineHeight(climate) + (lineNoise - 0.5) * 0.08; // (biometools.glsl: none in the desert)
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
	// the snow goes over the two ground materials (sampleBlend), it isn't ranked with them
	g_SnowOver = materials[BIOME_SNOW];
	materials[BIOME_SNOW] = 0.0;
	// the forest floor under the crowns (the canopy map follows the trees actually placed), patchy at the edges of a
	// stand and between trees, never on rock, snow or in a field. needles where the conifers grow, leaves elsewhere
	{
		// (only where the crowns close up, and up close only: from afar the crowns hide the floor, the litter turned
		// every forested mountainside an autumn brown. the cache's viewDistance is huge: none baked in)
		g_LitterOver = forestFloorCover(skyVisibility, readCoord, s.worldXZ) * (1.0 - rock) * (1.0 - weights[BIOME_WHEAT])
					 * (1.0 - smoothstep(45.0, 110.0, s.viewDistance));
		vec3 trees = treeChances(vec4(climate, terrainHeight), 0.0, 1.0);
		g_LitterKind = trees.x > trees.y ? TERRAIN_NEEDLES : TERRAIN_LEAF_LITTER;
	}

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
	// the uvs from the per pixel part plus the uniform part wrapped to the texture's own repeat. built from detailPos
	// directly they reached ~875 tiles far from the origin, where a float only steps ~0.2 texels: the textures
	// sampled in stripes and the parallax traced blocks (the wrapping's rounding is the same for every pixel)
	#define LAYER_UV(R, repeat) ((R) * (s.detailLocal / (repeat)) + fract((R) * (s.detailOffset / (repeat))))
	vec2 dx = s.dPdx.xz, dy = s.dPdy.xz;
	DetailUV near = DetailUV(LAYER_UV(mat2(1.0), ubo.textureRepeatNear), dx / ubo.textureRepeatNear, dy / ubo.textureRepeatNear);
	const mat2 farRotation = mat2(0.8, 0.6, -0.6, 0.8); // ~37 degrees, so the two layers never line up
	DetailUV far = DetailUV(LAYER_UV(farRotation, ubo.textureRepeatFar), farRotation * (dx / ubo.textureRepeatFar), farRotation * (dy / ubo.textureRepeatFar));
	float farWeight = s.farWeight;

	// the macro layer, a much bigger scale in the distance: the far layer's ~18m tile otherwise prints a visible
	// lattice over whole mountain sides
	float macroRepeat = ubo.textureRepeatFar * 7.0;
	const mat2 macroRotation = mat2(0.28, -0.96, 0.96, 0.28); // another ~74 degrees
	float macroWeight = s.macroWeight;
	// a 7x bigger tile reads a 7x sharper mip at the same distance: all that detail is sub pixel out here and the
	// bandwidth isn't (1.6ms), so the macro layer samples as if it were 4x further away
	const float MACRO_MIP_BIAS = 3.0;
	DetailUV macroLayer = DetailUV(LAYER_UV(macroRotation, macroRepeat), macroRotation * dx / macroRepeat * MACRO_MIP_BIAS, macroRotation * dy / macroRepeat * MACRO_MIP_BIAS);

	// cliffs: the top down projection stretches the textures into long streaks there, so steep ground is
	// textured from the sides instead (triplanar, x and z facing planes), blended by the geometric normal.
	// side uvs use the height directly, it doesn't move when the map re-centres
	vec3 axisSign = sign(normal);
	float y = s.position.y;
	vec2 sideX = vec2(detailPos.y * axisSign.x, y); // plane facing x: (z, y)
	vec2 sideZ = vec2(-detailPos.x * axisSign.z, y); // plane facing z: (x, y)
	vec2 sideXdx = vec2(s.dPdx.z * axisSign.x, s.dPdx.y), sideXdy = vec2(s.dPdy.z * axisSign.x, s.dPdy.y);
	vec2 sideZdx = vec2(-s.dPdx.x * axisSign.z, s.dPdx.y), sideZdy = vec2(-s.dPdy.x * axisSign.z, s.dPdy.y);
	// (the horizontal coordinate split like the top's, LAYER_UV)
	mat2 sideXAxis = mat2(0.0, 0.0, axisSign.x, 0.0), sideZAxis = mat2(-axisSign.z, 0.0, 0.0, 0.0); // picks z / x into u
	#define SIDE_UV(A, repeat) (LAYER_UV(A, repeat) + vec2(0.0, y / (repeat)))
	DetailUV sideXNear = DetailUV(SIDE_UV(sideXAxis, ubo.textureRepeatNear), sideXdx / ubo.textureRepeatNear, sideXdy / ubo.textureRepeatNear);
	DetailUV sideXFar = DetailUV(SIDE_UV(sideXAxis, ubo.textureRepeatFar), sideXdx / ubo.textureRepeatFar, sideXdy / ubo.textureRepeatFar);
	DetailUV sideZNear = DetailUV(SIDE_UV(sideZAxis, ubo.textureRepeatNear), sideZdx / ubo.textureRepeatNear, sideZdy / ubo.textureRepeatNear);
	DetailUV sideZFar = DetailUV(SIDE_UV(sideZAxis, ubo.textureRepeatFar), sideZdx / ubo.textureRepeatFar, sideZdy / ubo.textureRepeatFar);
	DetailUV sideXMacro = DetailUV(SIDE_UV(sideXAxis, macroRepeat), sideXdx / macroRepeat * MACRO_MIP_BIAS, sideXdy / macroRepeat * MACRO_MIP_BIAS);
	DetailUV sideZMacro = DetailUV(SIDE_UV(sideZAxis, macroRepeat), sideZdx / macroRepeat * MACRO_MIP_BIAS, sideZdy / macroRepeat * MACRO_MIP_BIAS);
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
	// grass against bare rock: frayed harder and finer (tongues and islands of turf in the scree), a smooth line
	// read as a painted lime blob on the mountainside
	wA = clamp(wA + (edgeNoise - 0.5) * 0.45 * smoothstep(0.0, 0.25, wA * (1.0 - wA)), 0.0, 1.0);
	float pomMaterial = mix(mix(mix(POM_MATERIAL_DEPTH[second], POM_MATERIAL_DEPTH[first], wA), 0.5, g_LitterOver), POM_MATERIAL_DEPTH[BIOME_SNOW], g_SnowOver);
	// faded over the outer 70% of the range
	float pomFade = ubo.pomDistance > 0.0 && ubo.pomDepth > 0.0 ? 1.0 - smoothstep(ubo.pomDistance * 0.3, ubo.pomDistance, viewDistance) : 0.0;
	pomFade *= wTop * smoothstep(0.02, 0.1, pomMaterial);
	if (ubo.cliffNoTile > 0.5) topNoTileBegin(near.uv); // (before the parallax moves near.uv)
	float pomShadow = 1.0;
	if (pomFade > 0.0) {
		vec3 toEye = normalize(s.campos - s.position);
		vec3 viewTS = vec3(dot(toEye, va), dot(toEye, vb), dot(toEye, normal));
		float scale = ubo.pomDepth * pomMaterial / ubo.textureRepeatNear * pomFade; // uv units
		float depth;
		near.uv = parallaxOcclusion(first, second, wA, near, viewTS, scale, pomFade, depth);
		// lit by whichever of the sun / moon is up, like the shadow maps
		vec3 light = s.sundir.y > 0.0 ? s.sundir : -s.sundir;
		vec3 lightTS = vec3(dot(light, va), dot(light, vb), dot(light, normal));
		pomShadow = mix(1.0, parallaxShadow(first, second, wA, near, near.uv, depth, lightTS, scale), pomFade);
	}

	RockCover rockDetail = rockCover(s.worldXZ, rock * smoothstep(0.2, 0.5, slope) * (1.0 - snowCap));
	// every projection that has weight here: the two materials height blended, the normal map turned into world
	// space (whiteout blend for the side planes, see Golus' triplanar normal mapping notes)
	vec3 albedoSum = vec3(0.0), normalSum = vec3(0.0);
	float wSum = 0.0;
	if (wTop > 0.02) {
		vec3 a, n;
		// (the repeat broken up on the flat ground too: the forest floor's dirt patches printed a grid every 3.6m seen
		// from a few metres up)
		sampleBlend(first, second, wA, near, far, macroLayer, farWeight, macroWeight, ubo.cliffNoTile > 0.5, a, n);
		rockBlend(rockDetail, near, far, macroLayer, farWeight, macroWeight, false, a, n);
		albedoSum += a * wTop;
		normalSum += normalize(mat3(va, vb, normal) * n) * wTop;
		wSum += wTop;
	}
	g_TopNoTile = false; // the side projections pick their own per sample (no parallax there)
	if (wX > 0.02) {
		vec3 a, n;
		sampleBlend(first, second, wA, sideXNear, sideXFar, sideXMacro, farWeight, macroWeight, ubo.cliffNoTile > 0.5, a, n);
		rockBlend(rockDetail, sideXNear, sideXFar, sideXMacro, farWeight, macroWeight, ubo.cliffNoTile > 0.5, a, n);
		n.x *= axisSign.x;
		vec3 t = vec3(n.xy + normal.zy, abs(n.z) * normal.x);
		albedoSum += a * wX;
		normalSum += normalize(t.zyx) * wX;
		wSum += wX;
	}
	if (wZ > 0.02) {
		vec3 a, n;
		sampleBlend(first, second, wA, sideZNear, sideZFar, sideZMacro, farWeight, macroWeight, ubo.cliffNoTile > 0.5, a, n);
		rockBlend(rockDetail, sideZNear, sideZFar, sideZMacro, farWeight, macroWeight, ubo.cliffNoTile > 0.5, a, n);
		n.x *= -axisSign.z;
		vec3 t = vec3(n.xy + normal.xy, abs(n.z) * normal.z);
		albedoSum += a * wZ;
		normalSum += normalize(t) * wZ;
		wSum += wZ;
	}
	normal = normalize(normalSum);

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
	albedo = mix(albedo, rockLuma * vec3(0.96, 0.97, 1.02), rockiness * 0.55 * (1.0 - rockDetail.kindMix * 0.6)); // (the other rocks keep some colour)
	// grass: a little less saturated, with drier yellowish patches (one flat bright green looked painted)
	// (not on the forest floor: the litter isn't grass, the meadow's tints turned it a flat yellow green)
	float grassiness = clamp(weights[BIOME_PLAINS] + weights[BIOME_TAIGA] * 0.5, 0.0, 1.0) * (1.0 - snowCap) * (1.0 - g_LitterOver);
	// the forest floor: damp, shaded litter is darker and browner than the photos' sunny autumn ground (their orange
	// read as a patch of something spilt over the grass)
	{
		float l = dot(albedo, vec3(0.3, 0.59, 0.11));
		albedo = mix(albedo, mix(vec3(l), albedo, 0.7) * vec3(0.72, 0.7, 0.66), g_LitterOver * (1.0 - snowCap));
	}
	float grassLuma = dot(albedo, vec3(0.3, 0.59, 0.11));
	albedo = mix(albedo, mix(vec3(grassLuma), albedo, 0.8), grassiness);
	// the meadow ground leans towards the grass blades' own colour, so where the grass rings thin out (or end) the
	// ground underneath doesn't show as a paler band
	albedo = mix(albedo, vec3(0.085, 0.15, 0.035) * (0.7 + 0.6 * dot(albedo, vec3(0.3, 0.59, 0.11)) / 0.12), grassiness * 0.35);
	float dryPatches = smoothstep(0.45, 0.8, valueNoise(snowPos * 0.08 + 31.0) * 0.7 + valueNoise(snowPos * 0.4) * 0.3);
	albedo *= mix(vec3(1.0), vec3(1.12, 1.04, 0.72), dryPatches * grassiness * 0.6);
	// the meadows from afar were one smooth green, a golf course: break them up at every scale. slow drifts between
	// lush and sun-dried (~150m), darker clumps of scrub, tall grass and bracken (a few metres, where the grass rings
	// don't reach they're what reads as vegetation), and a fine mottle in between
	float lush = valueNoise(snowPos * 0.012 + 57.0) * 0.65 + valueNoise(snowPos * 0.05 - 9.0) * 0.35;
	albedo *= mix(vec3(1.0), mix(vec3(0.82, 1.0, 0.86), vec3(1.16, 1.02, 0.74), smoothstep(0.25, 0.75, lush)), grassiness * 0.7);
	float clumps = valueNoise(snowPos * 0.55 + 3.3) * 0.6 + valueNoise(snowPos * 2.1 - 7.7) * 0.4;
	float scrub = smoothstep(0.6, 0.78, clumps) * smoothstep(0.35, 0.7, valueNoise(snowPos * 0.07 + 21.0));
	albedo *= mix(vec3(1.0), vec3(0.52, 0.58, 0.45), scrub * grassiness * 0.85);
	float mottle = valueNoise(snowPos * 0.22 + 41.0) * 0.6 + valueNoise(snowPos * 0.9) * 0.4;
	albedo *= mix(1.0, mix(0.86, 1.1, mottle), grassiness);

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
	surface.directScale = pomShadow;
	surface.canopy = canopy;
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
