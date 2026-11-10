// volumetric fog: a 3D grid of cells ("froxels") fitted to the view, x/y across the screen, z along the view ray
// out to ubo.fogDistance. volfog_inject.comp fills every froxel with how dense the fog is and how much light it
// scatters, volfog_integrate.comp sums them front to back, lighting.comp looks the result up per pixel.
// the slices are spread quadratically: thin near the camera where the detail shows, thick far away
#ifndef VOLFOG_GLSL
#define VOLFOG_GLSL

// ---- the fog itself, shared by the froxels (volfog_inject.comp) and the far march past them (volclouds.glsl)
float fogHash13(vec3 p) {
	p = fract(p * 0.1031);
	p += dot(p, p.zyx + 31.32);
	return fract((p.x + p.y) * p.z);
}
float fogValueNoise(vec3 p) {
	vec3 i = floor(p), f = fract(p);
	vec3 u = f * f * (3.0 - 2.0 * f);
	return mix(mix(mix(fogHash13(i), fogHash13(i + vec3(1, 0, 0)), u.x), mix(fogHash13(i + vec3(0, 1, 0)), fogHash13(i + vec3(1, 1, 0)), u.x), u.y),
			   mix(mix(fogHash13(i + vec3(0, 0, 1)), fogHash13(i + vec3(1, 0, 1)), u.x), mix(fogHash13(i + vec3(0, 1, 1)), fogHash13(i + vec3(1, 1, 1)), u.x), u.y), u.z);
}
float fogFbm(vec3 p) {
	return fogValueNoise(p) * 0.55 + fogValueNoise(p * 2.03 + 17.1) * 0.3 + fogValueNoise(p * 4.11 - 5.7) * 0.15;
}
// henyey-greenstein
float fogHG(float cosTheta, float g) {
	float g2 = g * g;
	return (1.0 - g2) / (4.0 * 3.14159265 * pow(max(1.0 + g2 - 2.0 * g * cosTheta, 1e-4), 1.5));
}
// water droplets scatter mostly forward (the glow around the sun, beams brightest looking into the light), a little
// back. toned down from g 0.65 at 3/4: into a low sun the whole view went under a bright veil. ~5x now, the beams read
float fogPhase(float cosTheta) {
	return mix(fogHG(cosTheta, -0.2), fogHG(cosTheta, 0.65), 0.65); // (a strong forward lobe: the shafts glow towards the sun)
}

// what drives the fog (the push constants of whoever evaluates it)
struct FogInputs {
	float density;	  // the setting, everything
	float mist;		  // the valley mist (the setting times the weather's)
	float weatherHaze; // the weather's extra haze (0 clear .. ~5 sandstorm)
	float time;		  // seconds
	vec2 cloudWindKm; // the mist drifts with the wind
	float sunHeight;  // the sun direction's y
	float wetness;	  // the ground (ubo.weather.y): wet valleys steam
};

// the fog's extinction per world unit at a point: ground = the terrain under it, around = the terrain ~300m around it
// (lower than the surroundings is a hollow, where cold air and mist collect), worldMetres = continuous world position
// in metres (the noise), metres = metres per world unit
//   - ground mist that pools in hollows and valleys, thickest around dawn and at night, a light veil by midday
//   - a thin haze near the ground everywhere, fading out with height, thickened by the weather
float fogExtinction(float height, float ground, float around, vec3 worldMetres, float metres, FogInputs f) {
	float hollow = clamp((around - ground) * metres / 25.0, 0.0, 1.0); // 25m below the surroundings: a proper valley
	float above = max(height - ground, 0.0) * metres;
	// mist: a thick layer in the valleys, only a trace elsewhere, heaviest from night through sunrise
	float dawn = 1.0 - 0.85 * smoothstep(0.03, 0.3, f.sunHeight);
	float mistHeight = mix(5.0, 40.0, hollow); // metres
	float mist = f.mist * dawn * mix(0.02, 1.0, hollow * hollow) * exp(-above / mistHeight);
	if (mist > 1e-5) {
		// broken up into banks and wisps that drift with the wind (~100m features)
		vec3 drift = vec3(f.cloudWindKm.x, 0.0, f.cloudWindKm.y) * 1000.0 * 0.4 + vec3(0.0, f.time * 0.15, 0.0);
		mist *= smoothstep(0.3, 0.7, fogFbm((worldMetres - drift) * 0.01)) * 1.8;
		mist *= 1.0 + f.wetness * 1.5; // after the rain the wet valleys steam
	}
	// haze: thin everywhere close to the ground, fading over a few hundred metres up. the weather thickens it near the
	// ground: rain and snow to a km or two of visibility, a sandstorm to a few hundred metres (its dust hugs the ground)
	float haze = exp(-above / 350.0);
	float weatherHaze = f.weatherHaze * mix(1.0, exp(-above / 120.0) * 1.6, clamp(f.weatherHaze / 5.0, 0.0, 1.0));
	// (the haze ~4.5km of mean free path near the ground: enough for the sun's shafts through trees and past ridges
	// to show on a clear day. at 3km it veiled every view, the distance haze is on top)
	return (mist * 0.025 + haze * 0.00019 * (1.0 + weatherHaze * 10.0)) * f.density * metres;
}

// distance along the view ray (world units) at slice coordinate s (0 = camera, 1 = far)
float fogSliceDistance(float s, float far) {
	return far * s * s;
}

// and back
float fogSliceCoord(float dist, float far) {
	return sqrt(clamp(dist / far, 0.0, 1.0));
}

// the fog between the camera and dist along the ray through uv: rgb = light scattered towards the camera,
// a = transmittance. every texel of the integrated volume holds the sum up to the END of its slice
vec4 sampleVolumetricFog(sampler3D fog, vec2 uv, float dist, float far) {
	float slices = float(textureSize(fog, 0).z);
	float s = fogSliceCoord(dist, far) * slices;
	vec4 integrated = texture(fog, vec3(uv, clamp(s - 0.5, 0.5, slices - 0.5) / slices));
	return s < 1.0 ? mix(vec4(0.0, 0.0, 0.0, 1.0), integrated, s) : integrated; // fades in over the first slice
}

#endif
