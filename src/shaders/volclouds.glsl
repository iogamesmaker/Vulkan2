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

const float CUMULUS_THICKNESS = 1.6; // km, fair weather cumulus
const float STORM_TOWER = 2.6;		 // a storm cell grows this many times taller (~5.8km at full instability)
const float CIRRUS_ALTITUDE = 9.0;	 // km above the ground
// the slab the march covers: as tall as the tallest tower the weather allows
float cloudSlabThickness() { return CUMULUS_THICKNESS * (1.0 + ubo.weather3.x * STORM_TOWER); }

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

// 0..1 at a world km position, hKm above the slab's floor. detail: 0 = the smooth dome only, 1 = carved into lobes
// (the light samples: each lobe shades itself), 2 = the billows / wisps on the edges too.
// every place has its own height: fair weather cumulus ~1.6km, the weather field's cells (clouds.glsl) grow into
// towers as the air gets unstable (up to ~5.8km in a thunderstorm), whose tops spread out into a flat anvil
float cumulusDensity(vec2 worldKm, float hKm, vec2 wind, int detail) {
	float slab = cloudSlabThickness();
	if (hKm < 0.0 || hKm > slab) return 0.0;
	float cells = cloudWeatherCells(worldKm);
	float core = weatherCellCore(cells) * ubo.weather3.x;
	float thickness = CUMULUS_THICKNESS * (1.0 + core * STORM_TOWER);
	float height = hKm / thickness;
	// the anvil: a storm's top spreads out in a flat sheet under the tropopause, wider than the tower under it
	// (thunderstorms only, around the core: showers and a broad sheet looked like flying saucers)
	float anvil = 0.0;
	if (ubo.weather3.x > 0.85) {
		float spread = smoothstep(0.62, 0.8, cells) * smoothstep(0.85, 1.0, ubo.weather3.x);
		float a = hKm / slab;
		anvil = spread * smoothstep(0.85, 0.9, a) * (1.0 - smoothstep(0.94, 1.0, a)) * 0.28; // a thin, faint spreading top
	}
	if (height > 1.0 && anvil <= 0.0) return 0.0;
	float coverage = cloudCoverage3(worldKm);
	float d = 0.0;
	if (coverage > 0.0 && height <= 1.0) {
		// flat bases, rounded tops: a dome whose height follows the coverage (thick cores reach the top, thin edges
		// stay low). a linear falloff made pointed cones
		float top = mix(0.15, 0.95, smoothstep(0.0, 1.0, coverage));
		float x = height / top;
		d = clamp(coverage * 1.5 * pow(max(1.0 - x * x, 0.0), 0.8), 0.0, 1.0);
		// the underside: every cloud's base at its own height, the thin edges higher than the core, rounded and uneven
		float base = cloudBaseNoise(worldKm) * 0.14 + (1.0 - smoothstep(0.1, 0.7, coverage)) * 0.1;
		d *= smoothstep(base, base + 0.06 + 0.12 * (1.0 - coverage), height);
	}
	if (detail == 0) return max(d, anvil);
	if (d <= 0.0 && anvil <= 0.0) return 0.0;
	// km, the wind carries the shapes along (and a little upwards: they boil)
	vec3 q = vec3(worldKm.x, hKm, worldKm.y) + vec3(wind.x * 0.5, wind.x * 0.05, wind.y * 0.5);
#ifdef CLOUD_NOISE_LUT
	// the lobes: Perlin-Worley (cloudnoise.comp), ~400m cells. the dome recedes wherever the noise is low, so every
	// outline is a cluster of rounded heads instead of the smooth blob of the coverage pattern; the dense core stays
	float lobes = texture(CLOUD_NOISE_LUT, q * (1.0 / 1.6)).r;
#else
	float lobes = volNoise(q * 2.5) * 0.6 + volNoise(q * 5.1 + 3.0) * 0.4;
#endif
	// (carved deeper than they were: the cotton ball outlines become clusters of distinct, sharper turrets)
	d = cloudRemap(d, (1.0 - lobes) * 0.9, 1.0, 0.0, 1.0);
	// the anvil isn't turrets: a sheet, shaped gently by the same noise (carved like the turrets it broke into
	// speckles, left smooth it was a plastic disc)
	float sheet = anvil * mix(0.25, 1.0, lobes);
	if (detail == 1 || d <= 0.0) return max(d, sheet);
#ifdef CLOUD_NOISE_LUT
	// the billows: an fbm of inverted Worley cells (~45m and finer). billowy towards the top, turned inside out
	// into wisps along the bases. they eat the thin edges and leave the core alone: crisp, detailed rims
	vec4 h = texture(CLOUD_NOISE_LUT, q * (1.0 / 0.36) + 0.37);
	float billows = clamp((h.g * 0.625 + h.b * 0.25 + h.a * 0.125 - 0.2) / 0.5, 0.0, 1.0); // (the fbm sits in ~0.25 .. 0.65)
#else
	float billows = volNoise(q * 22.0) * 0.6 + volNoise(q * 45.0 + 7.0) * 0.4;
#endif
	billows = mix(1.0 - billows, billows, smoothstep(0.08, 0.35, height));
	return max(cloudRemap(d, billows * 0.52, 1.0, 0.0, 1.0), cloudRemap(sheet, billows * 0.3, 1.0, 0.0, 1.0));
}

// lightning (weather.cpp picks the strike, ubo.weather4): the channel from the cloud base down to the ground. real
// bolts are chains of short straight steps with sharp kinks (the stepped leader), wandering sideways as they come
// down, with a few thinner branches forking off. piecewise linear offsets: k = km below the bolt's top
vec2 boltHash(float i, float seed) {
	return vec2(volHash(vec3(i, seed, 1.7)), volHash(vec3(i, seed, 9.3))) - 0.5;
}
vec2 boltOffset(float k, float seed) {
	float a = k / 0.12, b = k / 0.6; // ~120m steps, ~600m wander
	vec2 steps = mix(boltHash(floor(a), seed), boltHash(floor(a) + 1.0, seed), fract(a)) * 0.3;
	vec2 wander = mix(boltHash(floor(b) + 50.0, seed), boltHash(floor(b) + 51.0, seed), fract(b)) * 0.9;
	return steps + wander;
}
// the light of one channel the ray passes: offset = where the channel is at this height (km, from boltRel)
float boltChannel(vec2 boltRel, vec2 offset, vec2 along, float footprint) {
	vec2 b = boltRel + offset;
	float sideways = length(b - along * dot(b, along)); // km between the ray and the channel
	return exp(-sideways / footprint) * 5.0 + exp(-sideways / (footprint * 5.0 + 0.012)) * 0.05;
}
// returns the light it sends along the ray within tMax km. camAltKm: the camera above the planet's surface, boltRel:
// the strike relative to the camera (km)
vec3 lightningBolt(vec3 dir, float camAltKm, vec2 boltRel, float tMax) {
	float flash = ubo.weather4.z;
	if (flash <= 0.002) return vec3(0.0);
	float horizontal = length(dir.xz);
	if (horizontal < 1e-4) return vec3(0.0);
	vec2 along = dir.xz / horizontal;
	float t = dot(boltRel, along) / horizontal; // where the ray passes closest to the strike
	if (t <= 0.0 || t > tMax) return vec3(0.0);
	float alt = camAltKm + dir.y * t;
	float top = ubo.cloudAltitude + 0.15, bottom = ubo.baseAltitude - 1.5;
	if (alt < bottom || alt > top) return vec3(0.0);
	float k = top - alt, s = ubo.weather4.w;
	float footprint = t * 0.0005 + 0.001; // about half a pixel at that distance
	float light = boltChannel(boltRel, boltOffset(k, s), along, footprint);
	// two branches forking off the main channel part of the way down, reaching out and fading
	for (int i = 0; i < 2; i++) {
		float start = 0.25 + 0.3 * volHash(vec3(s, float(i), 4.1)) + float(i) * 0.25; // km below the top
		float length_ = 0.35 + 0.4 * volHash(vec3(s, float(i), 8.2));
		float into = k - start;
		if (into <= 0.0 || into > length_) continue;
		vec2 side = normalize(boltHash(float(i) + 90.0, s) + 1e-3) * 0.55;
		vec2 offset = boltOffset(start, s) + side * into + boltOffset(k + 7.0 * float(i + 1), s + 13.0) * 0.25;
		light += boltChannel(boltRel, offset, along, footprint * 0.7) * 0.45 * (1.0 - into / length_);
	}
	float fadeTop = smoothstep(0.0, 0.2, k); // it comes out of the cloud
	return vec3(0.8, 0.85, 1.0) * ubo.sunIlluminance * flash * fadeTop * light;
}

// front to back through the cumulus slab, from the camera up to tMax km along the ray (the scene's depth, or far
// for the sky). returns the in-scattered light (premultiplied) and the opacity. on the way the rain falling out of the clouds
// (curtains under the showers and storms) and lightning
// cloudEnter: where the ray enters the cloud slab (km), -1 if it doesn't
vec4 marchCumulus(sampler2D transmittanceLut, sampler2D skyviewLut, vec3 camKm, vec2 camWorldKm, vec2 wind, vec3 dir, float tMax, vec3 sunDir, float jitter, out float cloudEnter) {
	cloudEnter = -1.0;
	// (the pass also runs for the far fog alone, volumetric clouds off: no slab then)
	bool anyClouds = ubo.clouds > 0.5 && ubo.cloudQuality > 0.5 && (ubo.cloudCoverage > 0.0 || ubo.weather3.x > 0.0);
	if (!anyClouds) return vec4(0.0);
	float slab = cloudSlabThickness();
	float bottom = ubo.radiusbottom + ubo.cloudAltitude, top = bottom + slab;
	float r = length(camKm);
	float camAlt = r - ubo.radiusbottom;
	vec2 hitTop = sphereHits(camKm, dir, top), hitBottom = sphereHits(camKm, dir, bottom);

	vec2 skySize = vec2(textureSize(skyviewLut, 0));
	vec3 zenith = texture(skyviewLut, vec2(0.5, 1.0 - 0.5 / skySize.y)).rgb * ubo.sunIlluminance;
	vec3 sunLight = sunTransmittance(transmittanceLut, camKm + vec3(0.0, ubo.cloudAltitude, 0.0), sunDir) * ubo.sunIlluminance * smoothstep(-0.06, 0.0, sunDir.y);
	vec2 boltRel = ubo.weather4.xy - camWorldKm;

	// ---- the rain under the clouds: grey curtains hanging from the raining parts of the field. near the camera the
	// volumetric fog has the rain's haze (it reaches ~2.4km), these take over past it
	vec3 scattered = vec3(0.0);
	float viewTransmittance = 1.0;
	bool raining = ubo.weather3.y + ubo.weather3.z > 0.01;
	if (raining && camAlt < ubo.cloudAltitude + slab) {
		float tBelow = r < bottom ? (dir.y > 0.0 ? hitBottom.y : tMax) : (hitBottom.x > 0.0 ? hitBottom.y : 0.0);
		float tStart = r < bottom ? 1.5 : max(hitBottom.x, 1.5);
		// (not past the render distance either: curtains hanging over the void beyond the land)
		float horizontalKm = max(length(dir.xz), 1e-4);
		float edgeKm = ubo.fogEnd * ubo.worldScale;
		float tEnd = min(min(min(tBelow, tMax), 35.0), edgeKm / horizontalKm);
		if (tEnd > tStart) {
			const int RAIN_STEPS = 10;
			float dt = (tEnd - tStart) / float(RAIN_STEPS);
			bool snow = ubo.weather2.y > ubo.weather2.x;
			vec3 rainLight = zenith * (snow ? 0.55 : 0.32) * (1.0 - ubo.weather.x * 0.35) + sunLight * 0.02;
			for (int i = 0; i < RAIN_STEPS; i++) {
				float t = tStart + (float(i) + jitter) * dt;
				vec3 q = camKm + dir * t;
				if (length(q) > bottom) continue;
				float precip = localPrecipitation(cloudWeatherCells(q.xz + camWorldKm + wind));
				float sigma = precip * (snow ? 0.9 : 0.55) * smoothstep(1.5, 3.0, t) // per km: heavy rain ~3km of visibility
							* (1.0 - smoothstep(edgeKm * 0.85, edgeKm, t * horizontalKm));
				if (sigma <= 0.0) continue;
				float stepT = exp(-sigma * dt);
				scattered += viewTransmittance * rainLight * (1.0 - stepT);
				viewTransmittance *= stepT;
			}
		}
	}
	// the strike's channel below the base (the curtains in front dim it)
	scattered += viewTransmittance * lightningBolt(dir, camAlt, boltRel, tMax);

	// ---- the cloud slab
	float tEnter = -1.0, tExit = -1.0;
	if (r < bottom) {			 // below: up through the base, out through the top
		if (hitBottom.y > 0.0) { tEnter = hitBottom.y; tExit = hitTop.y; }
	} else if (r < top) {		 // inside: until the top, or the base on the way down
		tEnter = 0.0;
		tExit = hitBottom.x > 0.0 ? hitBottom.x : hitTop.y;
	} else if (hitTop.x > 0.0) { // above: down through the top, out through the base (or the top again at a grazing angle)
		tEnter = hitTop.x;
		tExit = hitBottom.x > 0.0 ? hitBottom.x : hitTop.y;
	}
	// a render distance: clouds starting further than ~60km fade out (far away the march is too coarse to look
	// good, and a downward ray from below the slab "hits" its base again on the far side of the planet)
	const float CLOUD_FADE_START = 40.0, CLOUD_FADE_END = 60.0; // km
	float distanceFade = 1.0 - smoothstep(CLOUD_FADE_START, CLOUD_FADE_END, tEnter);
	cloudEnter = tEnter;
	tExit = min(min(tExit, tMax), tEnter + 24.0); // at most 24km through the slab at a grazing angle
	vec3 cloudScattered = vec3(0.0);
	float cloudTransmittance = 1.0;
	if (tEnter >= 0.0 && tEnter < CLOUD_FADE_END && tExit > tEnter) {
		// lit by the sun AND the moon, each fading out with its own disc going under the horizon. order at dusk: the
		// sun's light fades out first, only then does the self shadow march turn to the moon, which fades in after
		vec3 entry = camKm + dir * tEnter;
		vec3 cloudSun = sunTransmittance(transmittanceLut, entry, sunDir) * ubo.sunIlluminance * smoothstep(-0.06, 0.0, sunDir.y);
		vec3 moonLight = sunTransmittance(transmittanceLut, entry, -sunDir) * ubo.moonIlluminance * smoothstep(0.06, 0.12, -sunDir.y);
		vec3 lightDir = sunDir.y > -0.06 ? sunDir : -sunDir;
		// a strong forward lobe (silver lining towards the light), a little back scattering
		float sunPhase = mix(cloudPhase(dot(dir, sunDir), -0.2), cloudPhase(dot(dir, sunDir), 0.82), 0.6) * 4.0 * PI;
		float moonPhase = mix(cloudPhase(dot(dir, -sunDir), -0.2), cloudPhase(dot(dir, -sunDir), 0.82), 0.6) * 4.0 * PI;
		vec3 light = cloudSun * sunPhase + moonLight * moonPhase;
		// the light that bounced around inside a while has forgotten where it came from: the multiple scattering terms
		// get a much flatter lobe (Wrenninge's octaves)
		float sunPhaseSoft = mix(cloudPhase(dot(dir, sunDir), -0.1), cloudPhase(dot(dir, sunDir), 0.3), 0.6) * 4.0 * PI;
		float moonPhaseSoft = mix(cloudPhase(dot(dir, -sunDir), -0.1), cloudPhase(dot(dir, -sunDir), 0.3), 0.6) * 4.0 * PI;
		vec3 lightSoft = cloudSun * sunPhaseSoft + moonLight * moonPhaseSoft;
		vec3 groundBounce = (cloudSun * max(sunDir.y, 0.0) + moonLight * max(-sunDir.y, 0.0)) * vec3(0.08, 0.085, 0.09);
		float extinction = ubo.cloudDensity * 28.0; // per km at density 1
		vec3 flashLight = vec3(0.8, 0.85, 1.0) * ubo.sunIlluminance * ubo.weather4.z;

		// steps sized to the way through (35..200m), two and a half times as long through empty air (most of the slab
		// is). a long step that lands inside a cloud steps back once and comes in at the fine size: the cloud's edge
		// isn't snapped to the coarse steps (that sliced the towers into stacked pancakes)
		float baseDt = clamp((tExit - tEnter) / 42.0, 0.055, 0.25);
		float t = tEnter + jitter * baseDt;
		bool skipping = false;
		for (int i = 0; i < 96 && t < tExit; i++) {
			vec3 p = camKm + dir * t;
			float hKm = length(p) - bottom;
			vec2 at = p.xz + camWorldKm + wind;
			// the finest detail (the billows) only within ~12km: further out it's under a pixel and costs the most
			float d = cumulusDensity(at, hKm, wind, t < 12.0 ? 2 : 1);
			if (d <= 0.004) { t += baseDt * 3.0; skipping = true; continue; }
			if (skipping) { skipping = false; t -= baseDt * 2.0; continue; } // back to just behind the edge
			float dt = baseDt;

			// the cloud between here and the light: 3 growing steps (big clouds shade their own far side and bases),
			// the first sees the lobes, the others only the dome
			float towardsLight = 0.0;
			for (int j = 1; j <= 3; j++) {
				float stepLength = 0.12 * float(j * j); // 0.12 .. 1.08 km
				vec3 q = p + lightDir * stepLength;
				towardsLight += cumulusDensity(q.xz + camWorldKm + wind, length(q) - bottom, wind, j == 1 ? 1 : 0) * 0.12 * float(2 * j - 1);
			}
			// a storm's tower is taller than the light samples reach: the darkness of the sky it makes (the overcast
			// the weather has where it's grey) dims what reaches the inside too, or the clouds stayed brilliant white
			// under a black sky
			float lightPath = towardsLight * extinction + ubo.weather.x * 2.2;
			// beer + softer multiple scattering terms, "powder": thin edges facing the light are darker than the inside
			float single = exp(-lightPath);
			float multiple = exp(-lightPath * 0.25) * 0.14 + exp(-lightPath * 0.06) * 0.04;
			float sigma = d * extinction;
			float powder = 1.0 - exp(-sigma * 1.0);
			// the bases see the darker ground, not the sky: grey undersides; a tall storm's bases are darker still
			float rel = clamp(hKm / slab, 0.0, 1.0);
			vec3 ambient = (zenith * mix(0.06, 0.7, rel * rel) + groundBounce * (1.0 - rel)) * PI * 0.25 * (1.0 - ubo.weather.x * 0.5);
			vec3 luminance = (light * single + lightSoft * multiple) * mix(0.35, 1.0, powder) * 0.32 + ambient;
			// a strike lights its cloud from inside
			if (ubo.weather4.z > 0.002) {
				vec3 rel3 = vec3(p.x - boltRel.x, hKm - 0.6, p.z - boltRel.y);
				luminance += flashLight * exp(-length(rel3) / 1.6) * 0.6;
			}

			float stepTransmittance = exp(-sigma * dt);
			cloudScattered += cloudTransmittance * luminance * (1.0 - stepTransmittance);
			cloudTransmittance *= stepTransmittance;
			if (cloudTransmittance < 0.02) break;
			t += dt;
		}
		float alpha = 1.0 - cloudTransmittance;
		if (alpha > 0.0) {
			// far clouds sink into the haze (their colour, not their opacity: thinned out far clouds let the cirrus
			// behind them shine through; only the render distance fade lets them go)
			float haze = 1.0 - exp(-tEnter / 25.0);
			vec3 lutCamKm = vec3(0.0, max(r, ubo.radiusbottom + PLANET_RADIUS_OFFSET), 0.0);
			vec3 hazeColor = texture(skyviewLut, skyviewUV(lutCamKm, normalize(vec3(dir.x, max(dir.y, 0.0), dir.z)), sunDir)).rgb * ubo.sunIlluminance;
			vec3 color = mix(cloudScattered / alpha, hazeColor, haze * 0.85);
			alpha *= distanceFade;
			cloudScattered = color * alpha;
			cloudTransmittance = 1.0 - alpha;
		}
	}
	// the clouds behind the rain
	scattered += viewTransmittance * cloudScattered;
	viewTransmittance *= cloudTransmittance;
	return vec4(scattered, 1.0 - viewTransmittance); // premultiplied: the bolt shows against clear air too
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
	// (thinner and patchier than they were: at sunset the converging streaks fanned out over the whole sky)
	float density = smoothstep(0.55, 0.85, n) * patches * (0.3 + 0.4 * ubo.cloudCoverage) * (1.0 - ubo.weather.x * 0.8);
	if (density <= 0.0) return vec4(0.0);
	// sun and moon, each fading with its disc (no switch at sunset). ice crystals: bright towards the light
	vec3 light = sunTransmittance(transmittanceLut, p, sunDir) * ubo.sunIlluminance * smoothstep(-0.06, 0.0, sunDir.y) * cloudPhase(dot(dir, sunDir), 0.4)
			   + sunTransmittance(transmittanceLut, p, -sunDir) * ubo.moonIlluminance * smoothstep(0.06, 0.12, -sunDir.y) * cloudPhase(dot(dir, -sunDir), 0.6);
	vec3 color = light * 4.0 * PI * 0.12 + skyLuminance * 0.6;
	float haze = 1.0 - exp(-t / 120.0);
	return vec4(mix(color, skyLuminance, haze * 0.7), density * 0.4 * (1.0 - haze * 0.6));
}

#endif
