#version 450
// a raindrop streak, a snowflake or a grain of sand: hidden behind the scene (the render resolution depth buffer,
// tested by hand: this draws onto the window resolution output), lit by the sky (grey when it's overcast), the sun
// and the lightning. rain is mostly see through (drops refract what's behind), snow nearly opaque

#include "precipitation.glsl"

layout(location = 0) in vec2 inUV;
layout(location = 1) flat in int inKind;
layout(location = 2) in float inAlpha;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D depthBuffer;

layout(set = 1, binding = 0) uniform sampler2D transmittanceLUT;
layout(set = 1, binding = 2) uniform sampler2D skyviewLUT;
#define ATMOSPHERE_SET 1
#define ATMOSPHERE_BINDING 4
#include "atmosphere.glsl"

void main() {
	if (inAlpha <= 0.002) discard;
	float scene = textureLod(depthBuffer, gl_FragCoord.xy / pc.view.xy, 0.0).r;
	if (scene > gl_FragCoord.z) discard; // reversed z: something's in front

	float shape;
	if (inKind == KIND_SNOW) {
		shape = 1.0 - smoothstep(0.55, 1.0, length(inUV * 2.0 - 1.0));
	} else {
		float across = 1.0 - abs(inUV.x * 2.0 - 1.0);
		shape = across * smoothstep(0.0, 0.35, inUV.y); // brightest at the head, fading along the tail
	}

	// the light: the (overcast) sky from above, some sun, the lightning
	vec2 skySize = vec2(textureSize(skyviewLUT, 0));
	vec3 zenith = overcastSky(texture(skyviewLUT, vec2(0.5, 1.0 - 0.5 / skySize.y)).rgb) * ubo.sunIlluminance;
	vec3 camKm = vec3(0.0, ubo.radiusbottom + max(ubo.baseAltitude + pc.campos.y * ubo.worldScale, PLANET_RADIUS_OFFSET), 0.0);
	vec3 sun = sunTransmittance(transmittanceLUT, camKm, pc.sundir) * ubo.sunIlluminance * max(pc.sundir.y, 0.0) * (1.0 - ubo.weather.x * 0.8);
	vec3 light = zenith * 3.0 + sun * 0.3 + vec3(0.75, 0.8, 1.0) * ubo.weather.z * ubo.sunIlluminance * 0.15;

	vec3 color;
	float opacity;
	if (inKind == KIND_RAIN) { color = vec3(0.6, 0.65, 0.7) * light * 0.45; opacity = 0.5; }
	else if (inKind == KIND_SNOW) { color = vec3(0.9, 0.92, 0.95) * light * 0.3; opacity = 0.85; }
	else { color = vec3(0.75, 0.55, 0.33) * light * 0.3; opacity = 0.5; }

	float a = shape * opacity * inAlpha;
	outColor = vec4(color, a);
}
