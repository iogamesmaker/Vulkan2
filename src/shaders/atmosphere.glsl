// shared atmosphere stuff (based on Hillaire 2020 "A Scalable and Production Ready Sky and Atmosphere Rendering Technique")
// all distances are in km, the planet centre is at the origin and +Y is up.
// the moon is always exactly opposite the sun (a permanent full moon), so it fits in the same sun-relative LUTs.
// define ATMOSPHERE_SET and ATMOSPHERE_BINDING before including this

#ifndef ATMOSPHERE_GLSL
#define ATMOSPHERE_GLSL

layout(set = ATMOSPHERE_SET, binding = ATMOSPHERE_BINDING) uniform AtmosphereUBO {
	vec3 rayleigh;		 // rayleigh scattering coefficient (per km)
	float rayleighscale; // rayleigh density scale height (km)
	vec3 mie;			 // mie scattering coefficient (per km)
	float miescale;		 // mie density scale height (km)
	vec3 absorbtion;	 // ozone absorption coefficient (per km)
	float mieg;			 // mie phase asymmetry
	vec3 groundAlbedo;
	float radiustop;

	float radiusbottom;
	float sunIlluminance;
	float moonIlluminance;
	float worldScale;	   // km per world unit

	float aerialDistance;  // km covered by the aerial perspective LUT
	float baseAltitude;	   // km above the planet surface at world y = 0
	float sunAngularRadius;
	float moonAngularRadius;

	float fogStart;		   // world units
	float fogEnd;		   // world units, fully opaque from here on (the edge of the terrain)
	float fogDensity;
	float starBrightness;
	
	float terrainSize;	   // world units covered by the heightmap
	float textureRepeatNear; // world units per detail texture repeat up close
	float textureRepeatFar;	// and further away, has to divide 3 (the map offset step) too
	float terrainCache;	   // 1 = terrain.frag reads the far field cache

	float pomDistance;	   // world units, parallax occlusion mapping fades out over the last 40%, 0 = off
	float pomDepth;		   // world units between the lowest and highest point of a height map
	float pomSteps;		   // most steps down the view ray (looking along the surface)
	float pomShadow;	   // strength of the parallax self shadowing, 0 = off

	float cloudCoverage;   // 0 clear .. 1 overcast (clouds.glsl)
	float cloudAltitude;   // km above the planet surface (same scale as baseAltitude)
	float cloudDensity;	   // how opaque the clouds are
	float fogDistance;	   // world units the volumetric fog's froxel grid reaches (volfog.glsl)

	// graphics settings (settings.hpp), 1 = on
	float terrainTriplanar;
	float terrainBumps;
	float terrainMacro;
	float cloudShadows;
	float clouds;			 // the cloud layer at all (and its shadows)
	float cloudQuality;		 // 0 = fewer noise octaves + light samples
	float aerialPerspective; // the haze over distance
	float volumetricFog;	 // 1 = the froxel fog is on

	vec4 weather;	// weather.cpp: x = overcast, y = wetness, z = lightning flash, w = snow cover
	vec4 weather2;	// x = rain, y = snowfall, z = sand, w = wind (0 calm .. 1 storm)
	// TEMPORARY fog playground (settings.hpp), volfog_inject.comp
	vec4 fogTuning;	 // haze strength, haze height (m), mist height scale, mist bank size (m)
	vec4 fogTuning2; // forward scattering g, light scale, temporal blend, mist breakup
	vec4 fogTuning3; // x = mist all day
} ubo;

// the weather's dimming of the sky's light: an overcast sky is a grey lid, much darker than the clear sky LUT
vec3 overcastSky(vec3 sky) {
	float grey = dot(sky, vec3(0.3, 0.5, 0.2));
	return mix(sky, vec3(grey) * vec3(0.92, 0.96, 1.0) * 0.45, ubo.weather.x * 0.85);
}

const float PI = 3.14159265359;
const float PLANET_RADIUS_OFFSET = 0.01; // keeps samples from being exactly on the ground

// ------------------------------------------------------------------ medium

float ozoneDensity(float height) { // tent profile centred at 25km
	return max(0.0, 1.0 - abs(height - 25.0) / 15.0);
}

void getScatteringValues(vec3 pos, out vec3 rayleighScattering, out vec3 mieScattering, out vec3 extinction) {
	float height = length(pos) - ubo.radiusbottom;

	float densityRay = exp(-max(0.0, height) / ubo.rayleighscale);
	float densityMie = exp(-max(0.0, height) / ubo.miescale);

	rayleighScattering = ubo.rayleigh * densityRay;
	mieScattering = ubo.mie * densityMie;

	vec3 mieExtinction = ubo.mie * 1.11 * densityMie; // mie absorbs a bit too
	vec3 ozoneExtinction = ubo.absorbtion * ozoneDensity(height);

	extinction = rayleighScattering + mieExtinction + ozoneExtinction;
}

// ------------------------------------------------------------------ phase functions

float rayleighPhase(float cosTheta) {
	return 3.0 * (1.0 + cosTheta * cosTheta) / (16.0 * PI);
}

float miePhase(float cosTheta) { // cornette-shanks
	float g = ubo.mieg;
	float g2 = g * g;
	float num = (1.0 - g2) * (1.0 + cosTheta * cosTheta);
	float denom = (2.0 + g2) * pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5);
	return 3.0 / (8.0 * PI) * num / denom;
}

// ------------------------------------------------------------------ geometry

// returns distance to the nearest intersection in front of the ray, -1 if none
float raySphereIntersectNearest(vec3 ro, vec3 rd, float radius) {
	float b = dot(ro, rd);
	float c = dot(ro, ro) - radius * radius;
	float d = b * b - c;
	if (d < 0.0) return -1.0;
	float sd = sqrt(d);
	float t1 = -b - sd;
	float t2 = -b + sd;
	if (t1 > 0.0) return t1;
	if (t2 > 0.0) return t2;
	return -1.0;
}

// ------------------------------------------------------------------ LUT lookups

// bruneton parameterisation, must match transmittancelut.comp
vec2 transmittanceUV(float viewHeight, float cosSunZenith) {
	float H = sqrt(max(0.0, ubo.radiustop * ubo.radiustop - ubo.radiusbottom * ubo.radiusbottom));
	float rho = sqrt(max(0.0, viewHeight * viewHeight - ubo.radiusbottom * ubo.radiusbottom));

	float d_min = ubo.radiustop - viewHeight;
	float d_max = rho + H;
	float d = sqrt(max(0.0, viewHeight * viewHeight * (cosSunZenith * cosSunZenith - 1.0) + ubo.radiustop * ubo.radiustop)) - viewHeight * cosSunZenith;

	return vec2(clamp((d - d_min) / (d_max - d_min), 0.0, 1.0), clamp(rho / H, 0.0, 1.0));
}

vec3 sampleTransmittance(sampler2D lut, vec3 pos, vec3 sunDir) {
	float height = length(pos);
	vec3 up = pos / height;
	return texture(lut, transmittanceUV(height, dot(sunDir, up))).rgb;
}

// transmittance towards the sun, zero when the planet is in the way
// fades over the sun's disc as it sinks below the planet's horizon (a hard cut when its centre crossed it made
// the light jump at sunset / moonrise)
vec3 sunTransmittance(sampler2D lut, vec3 pos, vec3 sunDir) {
	float height = length(pos);
	vec3 up = pos / height;
	float mu = dot(sunDir, up);
	float ratio = ubo.radiusbottom / max(height, ubo.radiusbottom);
	float horizonMu = -sqrt(max(1.0 - ratio * ratio, 0.0)); // cos of the angle to the horizon from up here
	float visible = smoothstep(horizonMu - ubo.sunAngularRadius, horizonMu + ubo.sunAngularRadius, mu);
	if (visible <= 0.0) return vec3(0.0);
	return texture(lut, transmittanceUV(height, max(mu, horizonMu + 1e-4))).rgb * visible;
}

vec3 sampleMultiscattering(sampler2D lut, vec3 pos, vec3 sunDir) {
	float height = length(pos);
	vec3 up = pos / height;
	vec2 res = vec2(textureSize(lut, 0));
	vec2 uv = vec2(
		dot(sunDir, up) * 0.5 + 0.5,
		clamp((height - ubo.radiusbottom) / (ubo.radiustop - ubo.radiusbottom), 0.0, 1.0)
	);
	uv = (0.5 + uv * (res - 1.0)) / res; // unit -> sub uv, matches multiscatterlut.comp
	return texture(lut, uv).rgb;
}

// light scattered towards the viewer at pos, from both the sun and the moon, per unit of sun illuminance
// cosTheta = dot(view direction, sun direction)
vec3 inScattering(sampler2D transmittanceLut, sampler2D multiscatteringLut, vec3 pos, vec3 sunDir, float cosTheta, vec3 rayleighScattering, vec3 mieScattering) {
	float moonRatio = ubo.moonIlluminance / ubo.sunIlluminance;

	vec3 sunTrans  = sunTransmittance(transmittanceLut, pos, sunDir);
	vec3 moonTrans = sunTransmittance(transmittanceLut, pos, -sunDir);
	vec3 sunMS	   = sampleMultiscattering(multiscatteringLut, pos, sunDir);
	vec3 moonMS	   = sampleMultiscattering(multiscatteringLut, pos, -sunDir);

	vec3 rayleighLight = rayleighPhase(cosTheta) * sunTrans + sunMS + moonRatio * (rayleighPhase(-cosTheta) * moonTrans + moonMS);
	vec3 mieLight	   = miePhase(cosTheta)		 * sunTrans + sunMS + moonRatio * (miePhase(-cosTheta)	   * moonTrans + moonMS);

	return rayleighScattering * rayleighLight + mieScattering * mieLight;
}

// sky view LUT lookup, cameraPos is the planet centred camera position
vec2 skyviewUV(vec3 cameraPos, vec3 rayDir, vec3 sunDir) {
	float height = length(cameraPos);
	vec3 up = cameraPos / height;

	float horizonAngle = acos(clamp(sqrt(max(height * height - ubo.radiusbottom * ubo.radiusbottom, 0.0)) / height, -1.0, 1.0));
	float altitudeAngle = horizonAngle - acos(clamp(dot(rayDir, up), -1.0, 1.0)); // -pi/2 .. pi/2 relative to the horizon

	float azimuthAngle = 0.0;
	if (abs(altitudeAngle) < 0.5 * PI - 1e-4) {
		vec3 right = cross(sunDir, up);
		if (dot(right, right) < 1e-8) right = vec3(1.0, 0.0, 0.0); // sun straight up
		right = normalize(right);
		vec3 forward = cross(up, right);
		vec3 projected = normalize(rayDir - up * dot(rayDir, up));
		azimuthAngle = atan(dot(projected, right), dot(projected, forward)) + PI;
	}

	// non-linear altitude mapping puts more texels near the horizon
	float v = 0.5 + 0.5 * sign(altitudeAngle) * sqrt(abs(altitudeAngle) * 2.0 / PI);
	return vec2(azimuthAngle / (2.0 * PI), v);
}

// distance fog (horizontal distance), 0 at fogStart -> exactly 1 at fogEnd
// stays light for most of the range and only closes in near the edge
float fogAmount(float distanceWorld) {
	float x = clamp((distanceWorld - ubo.fogStart) / max(ubo.fogEnd - ubo.fogStart, 1.0), 0.0, 1.0);
	return pow(smoothstep(0.0, 1.0, x), max(ubo.fogDensity, 0.1)); // density = how late it closes in
}

#endif
