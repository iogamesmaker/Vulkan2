// volumetric clouds: clouds.comp marches the cumulus at half resolution up to the scene's depth (mountains disappear
// into them, from above you see them over the land), lighting.comp composites it. the sky draws the cirrus:
//   - cumulus: a 1.6km slab over clouds.glsl's coverage pattern, denser cores grow taller (domes), 3D noise eats
//     into the edges (billows on top, wisps under the flat bases), lit by a short march towards the sun
//   - cirrus: a thin streaky layer high up (~9km), 2D, drawn by the sky only
// camera below, inside or above the slab all work. positions in km, planet centred (like atmosphere.glsl), the
// horizontal world position (continuous, map offset included) and the wind come in separately.
// needs atmosphere.glsl (ubo, PI) and clouds.glsl first
#ifndef VOLCLOUDS_GLSL
#define VOLCLOUDS_GLSL

const float CUMULUS_THICKNESS = 1.6; // km
const float CIRRUS_ALTITUDE = 9.0;	 // km above the ground

float volHash(vec3 p) {
	p = fract(p * 0.1031);
	p += dot(p, p.zyx + 31.32);
	return fract((p.x + p.y) * p.z);
}

float volNoise(vec3 p) {
	vec3 i = floor(p), f = fract(p);
	vec3 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(mix(volHash(i), volHash(i + vec3(1, 0, 0)), u.x), mix(volHash(i + vec3(0, 1, 0)), volHash(i + vec3(1, 1, 0)), u.x), u.y),
			   mix(mix(volHash(i + vec3(0, 0, 1)), volHash(i + vec3(1, 0, 1)), u.x), mix(volHash(i + vec3(0, 1, 1)), volHash(i + vec3(1, 1, 1)), u.x), u.y), u.z);
}

float cloudPhase(float cosTheta, float g) {
	float g2 = g * g;
	return (1.0 - g2) / (4.0 * PI * pow(1.0 + g2 - 2.0 * g * cosTheta, 1.5));
}

// both intersections with a sphere around the planet centre (t0 <= t1), (-1, -1) if missed
vec2 sphereHits(vec3 ro, vec3 rd, float radius) {
	float b = dot(ro, rd);
	float c = dot(ro, ro) - radius * radius;
	float d = b * b - c;
	if (d < 0.0) return vec2(-1.0);
	float s = sqrt(d);
	return vec2(-b - s, -b + s);
}

float cloudRemap(float v, float a, float b, float c, float d) { return clamp(c + (v - a) / max(b - a, 1e-4) * (d - c), min(c, d), max(c, d)); }

// 0..1 at a world km position and height in the slab (0 base .. 1 top). detail: 0 = the smooth dome only,
// 1 = carved into lobes (the light samples: each lobe shades itself), 2 = the billows / wisps on the edges too
float cumulusDensity(vec2 worldKm, float height, vec2 wind, int detail) {
	if (height < 0.0 || height > 1.0) return 0.0;
	float coverage = cloudCoverage3(worldKm);
	if (coverage <= 0.0) return 0.0;
	// flat bases, rounded tops: a dome whose height follows the coverage (thick cores reach the top of the slab, the
	// thin edges stay low). a linear falloff made pointed cones
	// (only the densest cores reach the top of the slab, otherwise they got cut off flat into columns)
	float top = mix(0.15, 0.95, smoothstep(0.0, 1.0, coverage));
	float x = height / top;
	float d = clamp(coverage * 1.5 * pow(max(1.0 - x * x, 0.0), 0.8), 0.0, 1.0);
	// the underside: every cloud's base at its own height (a slow noise), the thin edges higher than the core, so
	// the bottoms are rounded and uneven instead of all cut flat at the floor of the slab
	float base = cloudBaseNoise(worldKm) * 0.16 + (1.0 - smoothstep(0.1, 0.7, coverage)) * 0.1;
	d *= smoothstep(base, base + 0.08 + 0.12 * (1.0 - coverage), height);
	if (detail == 0 || d <= 0.0) return d;
	// km, the wind carries the shapes along (and a little upwards: they boil)
	vec3 q = vec3(worldKm.x, height * CUMULUS_THICKNESS, worldKm.y) + vec3(wind.x * 0.5, wind.x * 0.05, wind.y * 0.5);
#ifdef CLOUD_NOISE_LUT
	// the lobes: Perlin-Worley (cloudnoise.comp), ~400m cells. the dome recedes wherever the noise is low, so every
	// outline is a cluster of rounded heads instead of the smooth blob of the coverage pattern; the dense core stays
	float lobes = texture(CLOUD_NOISE_LUT, q * (1.0 / 1.6)).r;
#else
	float lobes = volNoise(q * 2.5) * 0.6 + volNoise(q * 5.1 + 3.0) * 0.4;
#endif
	d = cloudRemap(d, (1.0 - lobes) * 0.8, 1.0, 0.0, 1.0);
	if (detail == 1 || d <= 0.0) return d;
#ifdef CLOUD_NOISE_LUT
	// the billows: an fbm of inverted Worley cells (~45m and finer). billowy towards the top, turned inside out
	// into wisps along the bases. they eat the thin edges and leave the core alone: crisp, detailed rims
	vec4 h = texture(CLOUD_NOISE_LUT, q * (1.0 / 0.36) + 0.37);
	float billows = clamp((h.g * 0.625 + h.b * 0.25 + h.a * 0.125 - 0.2) / 0.5, 0.0, 1.0); // (the fbm sits in ~0.25 .. 0.65)
#else
	float billows = volNoise(q * 22.0) * 0.6 + volNoise(q * 45.0 + 7.0) * 0.4;
#endif
	billows = mix(1.0 - billows, billows, smoothstep(0.08, 0.35, height));
	return cloudRemap(d, billows * 0.45, 1.0, 0.0, 1.0);
}

// front to back through the cumulus slab, from the camera up to tMax km along the ray (the scene's depth, or far
// for the sky). returns the in-scattered light and the opacity
vec4 marchCumulus(sampler2D transmittanceLut, sampler2D skyviewLut, vec3 camKm, vec2 camWorldKm, vec2 wind, vec3 dir, float tMax, vec3 sunDir, float jitter) {
	if (ubo.clouds < 0.5 || ubo.cloudCoverage <= 0.0) return vec4(0.0);
	float bottom = ubo.radiusbottom + ubo.cloudAltitude, top = bottom + CUMULUS_THICKNESS;
	float r = length(camKm);
	vec2 hitTop = sphereHits(camKm, dir, top), hitBottom = sphereHits(camKm, dir, bottom);
	float tEnter, tExit;
	if (r < bottom) {			 // below: up through the base, out through the top
		if (hitBottom.y <= 0.0) return vec4(0.0);
		tEnter = hitBottom.y;
		tExit = hitTop.y;
	} else if (r < top) {		 // inside: until the top, or the base on the way down
		tEnter = 0.0;
		tExit = hitBottom.x > 0.0 ? hitBottom.x : hitTop.y;
	} else {					 // above: down through the top, out through the base (or the top again at a grazing angle)
		if (hitTop.x <= 0.0) return vec4(0.0);
		tEnter = hitTop.x;
		tExit = hitBottom.x > 0.0 ? hitBottom.x : hitTop.y;
	}
	// a render distance: clouds starting further than ~60km fade out (far away the march is too coarse to look
	// good, and a downward ray from below the slab "hits" its base again on the far side of the planet, through the
	// ground, thousands of km away: that drew clouds under the world)
	const float CLOUD_FADE_START = 40.0, CLOUD_FADE_END = 60.0; // km
	if (tEnter > CLOUD_FADE_END) return vec4(0.0);
	float distanceFade = 1.0 - smoothstep(CLOUD_FADE_START, CLOUD_FADE_END, tEnter);
	tExit = min(min(tExit, tMax), tEnter + 20.0); // at most 20km through the slab at a grazing angle
	if (tExit <= tEnter) return vec4(0.0);

	const int STEPS = 48;
	float dt = (tExit - tEnter) / float(STEPS);
	// lit by the sun AND the moon, each fading out with its own disc going under the horizon (picking one with a
	// hard sunDir.y > 0 switched the clouds' colour in a single frame at sunset). the self shadow march goes towards
	// whichever is up, it swaps while both are ~dark
	vec3 entry = camKm + dir * tEnter;
	// order at dusk: the sun's light fades out (to 0.06 below the horizon), only THEN does the self shadow march turn
	// to the moon, which fades in after (0.06 .. 0.12). turning the march while the sun still lit them flipped the
	// clouds' shading in one frame
	vec3 sunLight = sunTransmittance(transmittanceLut, entry, sunDir) * ubo.sunIlluminance * smoothstep(-0.06, 0.0, sunDir.y);
	vec3 moonLight = sunTransmittance(transmittanceLut, entry, -sunDir) * ubo.moonIlluminance * smoothstep(0.06, 0.12, -sunDir.y);
	vec3 lightDir = sunDir.y > -0.06 ? sunDir : -sunDir;
	// a strong forward lobe (silver lining towards the light), a little back scattering
	float sunPhase = mix(cloudPhase(dot(dir, sunDir), -0.2), cloudPhase(dot(dir, sunDir), 0.8), 0.6) * 4.0 * PI;
	float moonPhase = mix(cloudPhase(dot(dir, -sunDir), -0.2), cloudPhase(dot(dir, -sunDir), 0.8), 0.6) * 4.0 * PI;
	vec3 light = sunLight * sunPhase + moonLight * moonPhase;
	// the light that bounced around inside a while has forgotten where it came from: the multiple scattering terms
	// get a much flatter lobe (Wrenninge's octaves). with the sharp one thick bases glowed white towards the sun
	float sunPhaseSoft = mix(cloudPhase(dot(dir, sunDir), -0.1), cloudPhase(dot(dir, sunDir), 0.3), 0.6) * 4.0 * PI;
	float moonPhaseSoft = mix(cloudPhase(dot(dir, -sunDir), -0.1), cloudPhase(dot(dir, -sunDir), 0.3), 0.6) * 4.0 * PI;
	vec3 lightSoft = sunLight * sunPhaseSoft + moonLight * moonPhaseSoft;
	vec2 skySize = vec2(textureSize(skyviewLut, 0));
	vec3 zenith = texture(skyviewLut, vec2(0.5, 1.0 - 0.5 / skySize.y)).rgb * ubo.sunIlluminance;
	vec3 groundBounce = (sunLight * max(sunDir.y, 0.0) + moonLight * max(-sunDir.y, 0.0)) * vec3(0.09, 0.095, 0.1); // the lit land under the bases (neutral: green tinted the bases olive)
	float extinction = ubo.cloudDensity * 28.0; // per km at density 1: dense cores, real self shadowing (each lobe shades the next)

	float viewTransmittance = 1.0;
	vec3 scattered = vec3(0.0);
	for (int i = 0; i < STEPS; i++) {
		vec3 p = camKm + dir * (tEnter + (float(i) + jitter) * dt);
		float height = (length(p) - bottom) / CUMULUS_THICKNESS;
		float d = cumulusDensity(p.xz + camWorldKm + wind, height, wind, 2);
		if (d <= 0.004) continue;

		// the cloud between here and the light: 5 growing steps (big clouds shade their own far side and bases)
		float towardsLight = 0.0;
		for (int j = 1; j <= 5; j++) {
			float stepLength = 0.06 * float(j * j); // 0.06 .. 1.5 km
			vec3 q = p + lightDir * stepLength;
			// (the first steps see the lobes: each one shades itself and the ones behind it, the far ones only the dome)
			towardsLight += cumulusDensity(q.xz + camWorldKm + wind, (length(q) - bottom) / CUMULUS_THICKNESS, wind, j <= 3 ? 1 : 0) * 0.06 * float(2 * j - 1);
		}
		float lightPath = towardsLight * extinction;
		// beer + softer multiple scattering terms, "powder": thin edges facing the light are darker than the inside
		// (less of the flattening multiple scattering terms than before: they lit every cloud evenly white)
		float single = exp(-lightPath);
		float multiple = exp(-lightPath * 0.25) * 0.14 + exp(-lightPath * 0.06) * 0.04; // (more fill flattened them into evenly lit cotton)
		float sigma = d * extinction;
		float powder = 1.0 - exp(-sigma * 1.0);
		// the bases see the darker ground, not the sky: grey undersides, the tops less flat white than before
		vec3 ambient = (zenith * mix(0.08, 0.7, height * height) + groundBounce * (1.0 - height)) * PI * 0.25;
		vec3 luminance = (light * single + lightSoft * multiple) * mix(0.35, 1.0, powder) * 0.32 + ambient;

		float stepTransmittance = exp(-sigma * dt);
		scattered += viewTransmittance * luminance * (1.0 - stepTransmittance);
		viewTransmittance *= stepTransmittance;
		if (viewTransmittance < 0.02) break;
	}
	float alpha = 1.0 - viewTransmittance;
	if (alpha <= 0.0) return vec4(0.0);
	vec3 color = scattered / alpha;
	// far clouds sink into the haze
	float haze = 1.0 - exp(-tEnter / 25.0);
	// (the sky LUT can't take an altitude below the ground, the camera may be there, see clouds.comp)
	vec3 lutCamKm = vec3(0.0, max(length(camKm), ubo.radiusbottom + PLANET_RADIUS_OFFSET), 0.0);
	vec3 hazeColor = texture(skyviewLut, skyviewUV(lutCamKm, normalize(vec3(dir.x, max(dir.y, 0.0), dir.z)), sunDir)).rgb * ubo.sunIlluminance;
	// (the haze takes their colour, not their opacity: thinned out far clouds let the cirrus behind them shine through.
	// only the render distance fade, 40-60km, lets them go)
	return vec4(mix(color, hazeColor, haze * 0.85), alpha * distanceFade);
}

// the cirrus: thin streaks stretched along the wind, very high up. the sky only (nothing gets that high)
// one value noise octave for the cirrus: the same smoothstep interpolated lattice as cloudNoise, but where the sky
// binds the baked lattice the hardware's bilinear filter does the interpolation (the smoothstep goes into the
// coordinate), one lookup instead of four hashes. the lattice repeats every 256 units, finer octaves are rotated
float cirrusNoise(vec2 p) {
#ifdef CIRRUS_NOISE_TEXTURE
	vec2 i = floor(p), f = fract(p);
	return textureLod(CIRRUS_NOISE_TEXTURE, (i + f * f * (3.0 - 2.0 * f) + 0.5) / 256.0, 0.0).r;
#else
	return cloudNoise(p);
#endif
}

// the streaks' fbm, octaves finer than a pixel faded out (they only aliased into shimmer) and replaced by their
// average. most of the sky is far away and grazing, so this skips most of the octaves there. footprint: the size of a
// pixel in p's units
float cirrusFbm(vec2 p, float footprint) {
	float sum = 0.0, amplitude = 0.5, frequency = 1.0;
	const mat2 rotation = mat2(0.8, 0.6, -0.6, 0.8);
	for (int i = 0; i < 4; i++) {
		float keep = clamp(2.0 - 2.0 * frequency * footprint, 0.0, 1.0); // full up to 1/2 cycle per pixel, gone at 1
		if (keep <= 0.0) { sum += amplitude; break; } // this and every finer octave at their average (0.5 * 2 * amplitude)
		sum += mix(0.5, cirrusNoise(p), keep) * amplitude;
		p = rotation * p * 2.02 + 17.3;
		amplitude *= 0.5;
		frequency *= 2.02;
	}
	return sum;
}

vec4 cirrus(sampler2D transmittanceLut, vec3 camKm, vec2 camWorldKm, vec2 wind, vec3 dir, vec3 sunDir, vec3 skyLuminance) {
	if (ubo.clouds < 0.5) return vec4(0.0);
	float radius = ubo.radiusbottom + CIRRUS_ALTITUDE;
	vec2 hit = sphereHits(camKm, dir, radius);
	float t = length(camKm) < radius ? hit.y : hit.x;
	if (t <= 0.0) return vec4(0.0);
	vec3 p = camKm + dir * t;
	vec2 w = (p.xz + camWorldKm + wind * 2.0) * 0.12; // they drift faster up there
	// not everywhere: the patches first, most of the sky has none and skips the streaks below
	vec2 pp = w * 0.25 + 19.0;
	float patches = smoothstep(0.35, 0.65, cirrusNoise(pp) * 0.5 + cirrusNoise(mat2(0.8, 0.6, -0.6, 0.8) * pp * 2.02 + 17.3) * 0.25); // cloudFbm(pp, 2)
	if (patches <= 0.0) return vec4(0.0);
	vec2 along = normalize(vec2(0.8, 0.35)), across = vec2(-along.y, along.x);
	vec2 q = vec2(dot(w, along) * 0.35, dot(w, across) * 2.4); // long thin streaks
	q += vec2(cirrusNoise(w * 0.6 + 3.0), cirrusNoise(w * 0.6 - 7.0)) * 1.35; // bent, hooked (one octave each is plenty)
	// a pixel's size up there (~70 degrees over 1080 pixels), stretched where the layer is seen edge on, in q's units
	// (the across axis is the finest)
	float grazing = max(abs(dot(dir, normalize(p))), 0.02);
	float n = cirrusFbm(q, t * 0.0011 / grazing * 0.12 * 2.4);
	float density = smoothstep(0.5, 0.8, n) * patches * (0.4 + 0.6 * ubo.cloudCoverage);
	if (density <= 0.0) return vec4(0.0);
	// sun and moon, each fading with its disc (no switch at sunset). ice crystals: bright towards the light
	vec3 light = sunTransmittance(transmittanceLut, p, sunDir) * ubo.sunIlluminance * smoothstep(-0.06, 0.0, sunDir.y) * cloudPhase(dot(dir, sunDir), 0.6)
			   + sunTransmittance(transmittanceLut, p, -sunDir) * ubo.moonIlluminance * smoothstep(0.06, 0.12, -sunDir.y) * cloudPhase(dot(dir, -sunDir), 0.6);
	vec3 color = light * 4.0 * PI * 0.25 + skyLuminance * 0.6;
	float haze = 1.0 - exp(-t / 120.0);
	return vec4(mix(color, skyLuminance, haze * 0.7), density * 0.55 * (1.0 - haze * 0.6));
}

#endif
