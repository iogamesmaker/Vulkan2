#version 450
// outputs HDR luminance, post.comp does godrays + tonemapping

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outDirect; // nothing to shadow in the sky, see shadowresolve.comp
layout(location = 0) in vec2 texcoord;

layout(set = 0, binding = 0) uniform sampler2D transmittance;
layout(set = 0, binding = 1) uniform sampler2D multiscattering;
layout(set = 0, binding = 2) uniform sampler2D skyview;
layout(set = 0, binding = 5) uniform sampler2D moonTexture; // equirectangular colour map
layout(set = 0, binding = 6) uniform sampler2D cirrusLattice; // value noise lattice, 256 x 256 repeating (volclouds.glsl)
#define CIRRUS_NOISE_TEXTURE cirrusLattice

#define ATMOSPHERE_SET 0
#define ATMOSPHERE_BINDING 4
#include "atmosphere.glsl"
#include "clouds.glsl"
#include "volclouds.glsl"

layout(push_constant) uniform constants {
	mat4 invViewProj;	// rotation only, the camera sits at the origin
	vec3 camPos;		// km, planet centred
	float starRotation; // hour angle, the stars turn with the sky
	vec3 sunDir;
	float latitude;
	vec2 cameraWorldKm; // the camera's continuous world position (clouds)
	vec2 cloudWind;		// km the clouds have drifted
	float frame;		// counts up, the volumetric clouds' jitter
} pc;

float hash13(vec3 p) {
	p = fract(p * 0.1031);
	p += dot(p, p.zyx + 31.32);
	return fract((p.x + p.y) * p.z);
}

vec3 hash33(vec3 p) {
	p = fract(p * vec3(0.1031, 0.1030, 0.0973));
	p += dot(p, p.yxz + 33.33);
	return fract((p.xxy + p.yxx) * p.zyx);
}

vec3 rotateAxis(vec3 v, vec3 axis, float angle) { // rodrigues
	float c = cos(angle);
	float s = sin(angle);
	return v * c + cross(axis, v) * s + axis * dot(axis, v) * (1.0 - c);
}

vec3 stars(vec3 dir) {
	// undo the earth's rotation so the stars stay fixed relative to the sun's path
	vec3 pole = vec3(0.0, sin(pc.latitude), cos(pc.latitude));
	vec3 starDir = rotateAxis(dir, pole, -pc.starRotation);

	vec3 result = vec3(0.0);
	const float scale = 150.0;
	vec3 p = starDir * scale;
	vec3 cell = floor(p);

	float h = hash13(cell);
	if (h > 0.985) {
		vec3 starPos = cell + 0.5 + (hash33(cell) - 0.5) * 0.7;
		vec3 v = p - starPos;
		v -= starDir * dot(v, starDir); // distance on the sphere, not through it
		float d = length(v);

		float brightness = pow((h - 0.985) / 0.015, 4.0); // few bright stars, lots of faint ones
		vec3 tint = mix(vec3(1.0, 0.75, 0.55), vec3(0.65, 0.8, 1.0), hash13(cell + 17.0));
		result = tint * brightness * smoothstep(0.35, 0.0, d);
	}
	return result;
}


vec3 celestialDisk(vec3 dir, vec3 bodyDir, float angularRadius, float limbExponent) {
	float cosAngle = dot(dir, bodyDir);
	float cosRadius = cos(angularRadius);
	if (cosAngle < cosRadius) return vec3(0.0);

	float x = sqrt(clamp((1.0 - cosAngle) / (1.0 - cosRadius), 0.0, 1.0)); // 0 centre, 1 edge
	float limb = pow(max(1.0 - x * x, 0.0), limbExponent);
	float edge = smoothstep(1.0, 0.9, x);
	return vec3(limb * edge);
}

void main() {
	vec4 world = pc.invViewProj * vec4(texcoord * 2.0 - 1.0, 0.5, 1.0);
	vec3 dir = normalize(world.xyz / world.w);
	vec3 moonDir = -pc.sunDir;

	// below the horizon the LUT has hazy ground, the terrain fog fades into the same thing
	vec2 uv = skyviewUV(pc.camPos, dir, pc.sunDir);
	vec3 luminance = texture(skyview, uv).rgb * ubo.sunIlluminance;
	// the night sky a little darker (the moonlight is far brighter than the real thing so the land stays readable at
	// night, the sky it lit came out a pale blue grey): ~20% less once the sun is 6 degrees under, dusk untouched
	luminance *= 1.0 - 0.2 * smoothstep(-0.1, -0.2, pc.sunDir.y);

	if (uv.y > 0.5) {
		vec3 viewTransmittance = sampleTransmittance(transmittance, pc.camPos, dir);

		// sun
		luminance += celestialDisk(dir, pc.sunDir, ubo.sunAngularRadius, 0.4) * sunTransmittance(transmittance, pc.camPos, pc.sunDir) * ubo.sunIlluminance * 50.0;

		// moon: its near side mapped onto the disk (it's always opposite the sun here, so always full)
		vec3 moon = celestialDisk(dir, moonDir, ubo.moonAngularRadius, 0.0);
		if (moon.x > 0.0) {
			vec3 right = normalize(cross(abs(moonDir.y) > 0.99 ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0), moonDir));
			vec3 up = cross(moonDir, right);
			vec2 disk = vec2(dot(dir, right), dot(dir, up)) / sin(ubo.moonAngularRadius); // -1..1 across the disk
			float z = sqrt(max(1.0 - dot(disk, disk), 0.0)); // facing us at the centre
			float longitude = atan(disk.x, z), latitude = asin(clamp(disk.y, -1.0, 1.0));
			vec2 moonUV = vec2(0.5 + longitude / (2.0 * PI), 0.5 - latitude / PI);
			vec3 albedo = pow(textureLod(moonTexture, moonUV, 0.0).rgb, vec3(2.2));
			float shading = pow(z, 0.3); // lit from straight behind us, a little darker towards the limb
			// bright against the night sky, but not so far above everything that the auto exposure at night turns it
			// into a big bloom halo or clips it to a flat white disc (it was 12x this)
			luminance += moon * albedo * 4.0 * shading * sunTransmittance(transmittance, pc.camPos, moonDir) * ubo.moonIlluminance * 5.0;
		}

		luminance += stars(dir) * ubo.starBrightness * viewTransmittance;

		// the cirrus in front of all of it (it hides the sun, moon and stars)
		vec3 skyHere = texture(skyview, uv).rgb * ubo.sunIlluminance;
		vec4 high = cirrus(transmittance, pc.camPos, pc.cameraWorldKm, pc.cloudWind, dir, pc.sunDir, skyHere);
		luminance = mix(luminance, high.rgb, high.a);
	}
	// (the volumetric cumulus go on top of this in lighting.comp, from clouds.comp)

	outColor = vec4(luminance, 1.0);
	outDirect = vec4(0.0, 0.0, 0.0, 1.0);
}
