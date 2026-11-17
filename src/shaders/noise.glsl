#ifndef NOISE_GLSL
#define NOISE_GLSL

// the hashes and value noises every pass shares (they used to be copied into each file under its own name)

// exact integer math on the lattice point: the float hash (fract chains) is chaotic, the compiler fusing its
// multiply-adds differently for the cells either side of a border gave different values for the same point,
// hard seams in the noise (the snow edge's stripes), worse the further from the world's origin. anything placed
// in the world (biomes, communities, trees, the terrain's detail) uses this one
float latticeHash(vec2 p) {
	uvec2 q = uvec2(ivec2(p)) * uvec2(1597334673u, 3812015801u);
	uint n = (q.x ^ q.y) * 1597334673u;
	return float(n >> 8) * (1.0 / 16777216.0);
}

// smooth value noise on latticeHash, 0..1
float latticeNoise(vec2 p) {
	vec2 i = floor(p), f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(latticeHash(i), latticeHash(i + vec2(1.0, 0.0)), u.x), mix(latticeHash(i + vec2(0.0, 1.0)), latticeHash(i + vec2(1.0, 1.0)), u.x), u.y);
}

// Dave Hoskins' float hash (hash12): cheap, for what isn't stitched across tiles (clouds, the weather, snow on the trees)
float fractHash(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

// smooth value noise on fractHash, 0..1
float fractNoise(vec2 p) {
	vec2 i = floor(p), f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(fractHash(i), fractHash(i + vec2(1.0, 0.0)), u.x), mix(fractHash(i + vec2(0.0, 1.0)), fractHash(i + vec2(1.0, 1.0)), u.x), u.y);
}

// four random numbers from a 2D point (Hoskins' hash42)
vec4 hash42(vec2 p) {
	vec4 p4 = fract(vec4(p.xyxy) * vec4(0.1031, 0.1030, 0.0973, 0.1099));
	p4 += dot(p4, p4.wzxy + 33.33);
	return fract((p4.xxyz + p4.yzzw) * p4.zywx);
}

// interleaved gradient noise (Jimenez 2014): per pixel dither, 0..1. move p by 5.588238 per frame for the TAA
float interleavedGradientNoise(vec2 p) {
	return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

#endif
