#pragma once
// a CPU copy of the bits of heightmap.comp that decide the climate, so the application can read it without reading
// the GPU's heightmap back (the weather follows the climate under the camera).
// MUST MATCH heightmap.comp: hash, simplex, fbm, climateAt
#include <glm/glm.hpp>
#include <cmath>

namespace worldgen {

inline glm::vec2 hash(glm::vec2 p) {
	glm::vec3 p3 = glm::fract(glm::vec3(p.x, p.y, p.x) * glm::vec3(0.1031f, 0.1030f, 0.0973f));
	p3 += glm::dot(p3, glm::vec3(p3.y, p3.z, p3.x) + 33.33f);
	return glm::fract((glm::vec2(p3.x, p3.x) + glm::vec2(p3.y, p3.z)) * glm::vec2(p3.z, p3.y)) * 2.0f - 1.0f;
}

inline float simplex(glm::vec2 p) {
	const float K1 = 0.366025404f, K2 = 0.211324865f;
	glm::vec2 i = glm::floor(p + (p.x + p.y) * K1);
	glm::vec2 a = p - i + (i.x + i.y) * K2;
	float m = a.x >= a.y ? 1.0f : 0.0f; // step(a.y, a.x)
	glm::vec2 o(m, 1.0f - m);
	glm::vec2 b = a - o + K2;
	glm::vec2 c = a - 1.0f + 2.0f * K2;
	glm::vec3 h = glm::max(0.5f - glm::vec3(glm::dot(a, a), glm::dot(b, b), glm::dot(c, c)), 0.0f);
	glm::vec3 n = h * h * h * h * glm::vec3(glm::dot(a, hash(i)), glm::dot(b, hash(i + o)), glm::dot(c, hash(i + 1.0f)));
	return glm::dot(n, glm::vec3(70.0f));
}

inline float fbm(glm::vec2 p, int octaves) {
	float result = 0.0f, freq = 1.0f, ampl = 1.0f, normalization = 0.0f;
	for(int i = 0; i < octaves; i++) {
		result += simplex(p * freq) * ampl;
		normalization += ampl;
		freq *= 2.0f;
		ampl *= 0.5f;
	}
	return result / normalization;
}

inline glm::vec3 climateAt(glm::vec2 p) {
	glm::vec2 warp = glm::vec2(fbm(p * 0.15f + glm::vec2(3.7f, 9.1f), 2), fbm(p * 0.15f + glm::vec2(-8.3f, 2.4f), 2)) * 2.5f;
	glm::vec2 q = p + warp;
	float temperature = fbm(q * 0.07f + glm::vec2(17.3f, -41.7f), 2) * 1.3f + 0.5f;
	float moisture = fbm(q * 0.09f + glm::vec2(-93.1f, 12.9f), 2) * 1.3f + 0.5f;
	float ruggedness = fbm(q * 0.11f + glm::vec2(55.5f, 77.2f), 2) * 1.4f + 0.5f;
	return glm::clamp(glm::vec3(temperature, moisture, ruggedness), 0.0f, 1.0f);
}

// heightmap.comp's p for a continuous world position (world units, x / z): its texel coordinate is the world
// position + half the terrain (the map is centred on the origin at offset 0), p = texel * 4 / heightmap size.
// texelsPerUnit = heightmap texels per world unit
inline glm::vec2 noisePosition(glm::vec2 worldXZ, float terrainSize, float texelsPerUnit, float heightmapSize) {
	return (worldXZ + 0.5f * terrainSize) * texelsPerUnit * 4.0f / heightmapSize;
}

}
