// contour lines of slow, warped noise: naturally meandering networks without any simulation (roads and trails,
// baked into the path map by heightmap.comp)
#ifndef PATHS_GLSL
#define PATHS_GLSL

#define ROAD_SCALE 0.006	  // roads every ~1.5km (heightmap.comp still bakes the distances, nothing draws them)
#define TRAIL_SCALE 0.022	  // trails every ~400m

float pathHash(vec2 p) {
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float pathNoise(vec2 p) {
	vec2 i = floor(p), f = fract(p);
	vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0); // quintic, smooth gradients for the distance estimate
	return mix(mix(pathHash(i), pathHash(i + vec2(1, 0)), u.x), mix(pathHash(i + vec2(0, 1)), pathHash(i + vec2(1, 1)), u.x), u.y);
}

float contourNoise(vec2 p) {
	vec2 warp = vec2(pathNoise(p * 0.7 + 3.1), pathNoise(p * 0.7 - 5.7)) - 0.5;
	p += warp * 0.6;
	return pathNoise(p) * 0.75 + pathNoise(p * 2.3 + 11.0) * 0.25;
}

// approximate signed distance (in texels) to the 0.5 contour of contourNoise(texel * scale), clamped so the
// slow parts of the noise don't produce huge values
float contourSignedDistance(vec2 texel, float scale, float seed) {
	vec2 p = texel * scale + seed;
	const float e = 0.05;
	float n = contourNoise(p);
	vec2 gradient = vec2(contourNoise(p + vec2(e, 0.0)) - n, contourNoise(p + vec2(0.0, e)) - n) / e * scale;
	return clamp((n - 0.5) / max(length(gradient), 1e-5), -8.0, 8.0);
}

#endif
