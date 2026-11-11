// the wind's gusts, shared by the grass (foliage.vert), the ground cover plants (groundcover.vert) and the trees
// (trees.vert) so one gust is seen rolling through all of them: patches of stronger wind, wider across the wind than
// along it, racing downwind over the world with calm gaps between them. travel = how far the wind has blown
// (Application::m_WindTravel, world units, faster in stronger wind: interaction.trailInfo.w)
#ifndef WINDFIELD_GLSL
#define WINDFIELD_GLSL

const vec2 WIND_DIR = vec2(0.91615, 0.40081); // normalize(0.8, 0.35), the rain's and the clouds' (settings.cpp)

// how fast the gusts travel (world units / s) in the weather's wind 0..1, Application::update's
float windSpeed(float wind) { return 0.6 + 2.4 * wind; }

// (an integer hash: world coordinates go to tens of thousands, a fract(sin) hash went blocky out there)
float windHash(ivec2 c) {
	uvec2 u = uvec2(c) * uvec2(1597334673u, 3812015801u);
	uint n = (u.x ^ u.y) * 1597334673u;
	return float(n >> 8) * (1.0 / 16777216.0);
}

float windNoise(vec2 p) {
	vec2 cell = floor(p), f = p - cell;
	ivec2 c = ivec2(cell);
	f = f * f * (3.0 - 2.0 * f);
	return mix(mix(windHash(c), windHash(c + ivec2(1, 0)), f.x), mix(windHash(c + ivec2(0, 1)), windHash(c + ivec2(1, 1)), f.x), f.y);
}

// 0 in a lull .. 1 in the heart of a gust. the big patches ~30-60m, smaller ones riding a little faster through
// them so the pattern changes as it goes instead of sliding by rigidly
float windGust(vec2 worldXZ, float travel) {
	vec2 across = vec2(-WIND_DIR.y, WIND_DIR.x);
	vec2 p1 = worldXZ - WIND_DIR * travel, p2 = worldXZ - WIND_DIR * (travel * 1.35);
	float big = windNoise(vec2(dot(p1, WIND_DIR), dot(p1, across)) * vec2(0.15, 0.065));
	float small = windNoise(vec2(dot(p2, WIND_DIR), dot(p2, across)) * vec2(0.42, 0.2) + 31.7);
	return smoothstep(0.32, 0.78, big * 0.68 + small * 0.32);
}

#endif
