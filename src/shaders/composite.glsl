// everything between the camera and a surface, composited the same way for every surface: the deferred lighting
// (lighting.comp): the haze, edge of the world fog, cumulus and volumetric fog
// needs atmosphere.glsl + volfog.glsl included and transmittanceLUT, skyviewLUT, aerialLUT, aerialSunLUT, volumetricFog declared
#ifndef COMPOSITE_GLSL
#define COMPOSITE_GLSL

// a lightning flash lighting the clouds from inside (weather.cpp's flash, in multiples of the sun)
vec3 lightningGlow() {
	return vec3(0.8, 0.85, 1.0) * ubo.weather.z * ubo.sunIlluminance * 0.15;
}

struct AtmosphereView {
	vec2 screenUV;
	vec3 position;		 // the surface, terrain space
	vec3 campos;
	vec3 sunDir;
	vec4 cloud;			 // the cumulus in front of the surface (clouds.comp): rgb in-scattered, a = transmittance
	bool clouds;		 // the volumetric cumulus ran this frame
	float airLit;		 // how much of the air between the camera and the surface the sun / moon reaches (0..1)
};

// aerial perspective, the fog at the edge of the world, the cumulus, the volumetric fog,
// in that order (far to near). direct: the sun / moon light the shadow filter adds later, scaled the same
vec3 compositeAtmosphere(vec3 color, inout vec3 direct, AtmosphereView v) {
	vec3 viewDir = normalize(v.position - v.campos);
	float dist = distance(v.position, v.campos);
	vec3 camKm = vec3(0.0, ubo.radiusbottom + max(ubo.baseAltitude + v.campos.y * ubo.worldScale, PLANET_RADIUS_OFFSET), 0.0);

	// aerial perspective (not sampled at all when it's off: the LUT isn't updated then)
	float slices = float(textureSize(aerialLUT, 0).z);
	float w = dist * ubo.worldScale / ubo.aerialDistance;
	float weight = clamp(w * slices, 0.0, 1.0) * ubo.aerialPerspective; // fade in over the first slice
	vec3 lutCoord = vec3(v.screenUV, clamp(w * slices - 0.5, 0.5, slices - 0.5) / slices); // slice i stores the end of [i, i + 1]
	vec4 aerial = ubo.aerialPerspective > 0.5 ? texture(aerialLUT, lutCoord) : vec4(0.0, 0.0, 0.0, 1.0);
	// the LUT lights all of its air: with the sun just behind a ridge its haze glowed in front of the mountain as if
	// the sun shone through it. the sun's own single scattering (aerialSunLUT) only where the air is lit (ssao.comp
	// measures how much of this ray's air is), the multiple scattering everywhere
	vec3 aerialSun = ubo.aerialPerspective > 0.5 ? texture(aerialSunLUT, lutCoord).rgb : vec3(0.0);
	aerial.rgb -= aerialSun * (1.0 - v.airLit);
	aerial.rgb = max(aerial.rgb, vec3(0.0)) * weight;
	aerial.a = mix(1.0, aerial.a, weight);
	color = color * aerial.a + aerial.rgb * ubo.sunIlluminance;
	direct *= aerial.a;

	// the edge of the world fades into what the sky LUT sees in that direction, which is also what the sky shader
	// draws past the edge (seamless). above the horizon (ridges against the sky): the haze at the horizon
	float fog = fogAmount(distance(v.position.xz, v.campos.xz));
	if (fog > 0.0) {
		vec3 fogDir = normalize(vec3(viewDir.x, min(viewDir.y, -0.002), viewDir.z));
		vec3 fogColor = texture(skyviewLUT, skyviewUV(camKm, fogDir, v.sunDir)).rgb * ubo.sunIlluminance;
		color = mix(color, fogColor, fog);
		direct *= 1.0 - fog;
	}

	// the volumetric cumulus between the camera and the surface: from above you see them over the land
	if (v.clouds) {
		color = color * v.cloud.a + v.cloud.rgb + (1.0 - v.cloud.a) * lightningGlow();
		direct *= v.cloud.a;
	}

	// the volumetric fog (mist, haze, the beams through it), nearest of all
	if (ubo.volumetricFog > 0.5) {
		vec4 volume = sampleVolumetricFog(volumetricFog, v.screenUV, dist, ubo.fogDistance);
		color = color * volume.a + volume.rgb;
		direct *= volume.a;
	}
	return color;
}

#endif
