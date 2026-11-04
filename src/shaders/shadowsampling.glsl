// cascaded shadow map lookup shared by the terrain, trees and foliage. expects set 2 to be ShadowMaps' descriptor set
#ifndef SHADOW_SAMPLING_GLSL
#define SHADOW_SAMPLING_GLSL

#define SHADOW_CASCADES 4

// the pixel the per pixel noise is keyed on: the fragment's, compute shaders define their own
#ifndef SHADOW_PIXEL
#define SHADOW_PIXEL gl_FragCoord.xy
#endif // the last one is terrain only and covers the whole world

layout(set = 2, binding = 0) uniform sampler2DArrayShadow shadowMap;
layout(set = 2, binding = 1) uniform ShadowUBO {
	mat4 lightViewProj[8]; // layer = set * SHADOW_CASCADES + cascade
	vec4 splits;   // far distance of each cascade (view distance, world units)
	vec4 widths;   // world size of each cascade
	vec4 params;   // x = filter radius (world units), y = normal offset (texels), z = enabled, w = shadow distance
	vec4 blend;	   // x = weight of set B, y = shadow strength, z = set A, w = set B
	vec4 depthRange[2]; // world units covered by the depth of each layer, [set][cascade]
	vec4 cascadeBlend;  // x = fraction of each cascade's range that crossfades into the next one, y = soft shadows (PCSS) on
} shadowData;
layout(set = 2, binding = 2) uniform sampler2DArray shadowDepth; // the same maps, raw depth for the blocker search

float interleavedGradientNoise(vec2 p) {
	return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

// vogel disk: a golden angle spiral, covers the disk evenly for any tap count (the per pixel rotation is applied
// on top, the trig here is constant folded since the loops have fixed counts)
vec2 vogelDisk(int i, int count) {
	float r = sqrt((float(i) + 0.5) / float(count));
	float theta = float(i) * 2.39996323;
	return r * vec2(cos(theta), sin(theta));
}

struct ShadowLookup {
	vec3 coord;		  // xy = uv, z = depth
	float layer;
	float texelWorld; // world size of a shadow map texel
	float width;	  // world size of the cascade
	float depthRange; // world units covered by the depth
};

ShadowLookup shadowLookup(int set, int cascade, vec3 worldPos, vec3 normal, float NdotL) {
	ShadowLookup s;
	int layer = set * SHADOW_CASCADES + cascade;
	s.layer = float(layer);
	s.width = shadowData.widths[cascade];
	s.texelWorld = s.width / float(textureSize(shadowMap, 0).x);
	s.depthRange = shadowData.depthRange[set][cascade];

	// push the lookup off the surface (less acne on slopes facing away from the light)
	vec3 offsetPos = worldPos + normal * s.texelWorld * shadowData.params.y * (1.0 - NdotL * 0.5);
	vec4 lightSpace = shadowData.lightViewProj[layer] * vec4(offsetPos, 1.0);
	s.coord = lightSpace.xyz / lightSpace.w;
	s.coord.xy = s.coord.xy * 0.5 + 0.5;
	return s;
}

// PCSS blocker search: the average depth of whatever is between us and the light, over a fixed area. gathers 4
// texels per tap, 32 depths in total, so the estimate (and the penumbra size) doesn't flicker between pixels.
// returns the penumbra width in world units (sun's angular size * blocker distance), < 0 when nothing's in the way
float penumbraWidth(ShadowLookup s, mat2 rotation) {
	const float searchWorld = 2.5;
	float searchUV = max(searchWorld, s.texelWorld * 3.0) / s.width;
	float bias = 2.0 * s.texelWorld / s.depthRange;
	float sum = 0.0, count = 0.0;
	for (int i = 0; i < 8; i++) {
		vec4 d = textureGather(shadowDepth, vec3(s.coord.xy + rotation * vogelDisk(i, 8) * searchUV, s.layer), 0);
		vec4 blocking = step(d, vec4(s.coord.z - bias));
		sum += dot(d, blocking);
		count += dot(blocking, vec4(1.0));
	}
	if (count == 0.0) return -1.0;
	float blockerDistance = (s.coord.z - sum / count) * s.depthRange; // world units
	const float sunRadius = 0.0093; // the sun's angular radius for the penumbras (~2x real), the sky draws it bigger
	return blockerDistance * tan(sunRadius) * 2.0 * shadowData.params.x;
}

// every tap is a hardware 2x2 bilinear comparison, so 10 taps already blend 40 texels
float filterShadow(ShadowLookup s, float penumbra, float minRadius, mat2 rotation) {
	float radiusUV = clamp(max(penumbra, minRadius), s.texelWorld * 1.5, 6.0) / s.width;
	float lit = 0.0;
	for (int i = 0; i < 10; i++) {
		lit += texture(shadowMap, vec4(s.coord.xy + rotation * vogelDisk(i, 10) * radiusUV, s.layer, s.coord.z));
	}
	return lit / 10.0;
}

// one cascade, crossfaded between the two keyframe sets by how far the sun has travelled between them. dithered:
// each pixel looks up one set (filtering both doubled the cost of every shadow lookup), the screen space shadow
// filter (shadowresolve.comp) blurs the dither into the crossfade
// minRadius: the filter radius (world units) the crossfade into the next cascade widens this one's to, so both
// sides of the seam are about as soft and only the texel density changes
float sampleCascade(int cascade, vec3 worldPos, vec3 normal, float NdotL, mat2 rotation, float minRadius) {
	bool useB = shadowData.blend.x > interleavedGradientNoise(SHADOW_PIXEL + 41.0);
	ShadowLookup s = shadowLookup(int(useB ? shadowData.blend.w : shadowData.blend.z), cascade, worldPos, normal, NdotL);

	// past the first cascade (~650m) a contact hardening penumbra is a pixel or two at most: skip the blocker
	// search and take 4 rotated hardware PCF taps over a fixed radius. most of the terrain on screen is out
	// there, the full lookup cost ~4ms for nothing visible
	if (cascade > 0 || shadowData.cascadeBlend.y < 0.5) {
		float radiusUV = max(s.texelWorld * 1.5, minRadius) / s.width;
		float lit = 0.0;
		for (int i = 0; i < 4; i++) lit += texture(shadowMap, vec4(s.coord.xy + rotation * vogelDisk(i, 4) * radiusUV, s.layer, s.coord.z));
		return lit * 0.25;
	}

	float penumbra = penumbraWidth(s, rotation);
	if (penumbra < 0.0) return 1.0; // nothing in the way
	return filterShadow(s, penumbra, minRadius, rotation);
}

// the fixed filter radius past the first cascade (world units)
float cascadeRadius(int cascade) {
	return shadowData.widths[cascade] / float(textureSize(shadowMap, 0).x) * 1.5;
}

// cascaded shadow maps rendered for the keyframes just behind and just ahead of the sun, crossfaded by how far
// the sun has travelled between them, so the shadows move smoothly instead of re-rasterising every frame.
// the noise rotation is per pixel, the screen space shadow filter (shadowresolve.comp) smooths it out
float sampleShadow(vec3 worldPos, vec3 normal, vec3 lightDir, float viewDistance) {
	float strength = shadowData.blend.y;
	if (shadowData.params.z < 0.5 || viewDistance > shadowData.params.w || strength <= 0.0) return 1.0;

	int cascade = viewDistance < shadowData.splits.x ? 0 : (viewDistance < shadowData.splits.y ? 1 : (viewDistance < shadowData.splits.z ? 2 : 3));

	float NdotL = clamp(dot(normal, lightDir), 0.0, 1.0);
	float angle = interleavedGradientNoise(SHADOW_PIXEL) * 6.2831853;
	mat2 rotation = mat2(cos(angle), sin(angle), -sin(angle), cos(angle));

	// crossfade into the next cascade over the last part of this one's range (half by default), the cascades
	// differ in resolution and in what casts (grass only in the first), a short blend shows as a seam. dithered:
	// each pixel looks up one cascade (sampling both doubled the shadow cost over half the screen), the screen
	// space shadow filter (shadowresolve.comp) blurs the dither into a smooth gradient
	float minRadius = 0.0;
	if (cascade < SHADOW_CASCADES - 1) {
		float splitEnd = shadowData.splits[cascade];
		float blendStart = mix(cascade == 0 ? 0.0 : shadowData.splits[cascade - 1], splitEnd, 1.0 - shadowData.cascadeBlend.x);
		float blend = smoothstep(blendStart, splitEnd, viewDistance);
		// widen the filter towards the next cascade's over the whole crossfade (a bit before it, so the softness
		// already matches where the dither starts mixing in the coarser map)
		float soften = smoothstep(blendStart - (splitEnd - blendStart) * 0.5, splitEnd, viewDistance);
		minRadius = cascadeRadius(cascade + 1) * soften;
		if (blend > interleavedGradientNoise(SHADOW_PIXEL + 17.0)) { cascade++; minRadius = 0.0; }
	}
	float lit = sampleCascade(cascade, worldPos, normal, NdotL, rotation, minRadius);

	// fade out towards the edge of the world, and when the light is right at the horizon
	float fade = smoothstep(shadowData.params.w * 0.9, shadowData.params.w, viewDistance);
	return mix(1.0, lit, strength * (1.0 - fade));
}

// one hardware 2x2 PCF tap per keyframe set in one cascade, no blocker search and no cascade blend. for small
// distant things (far grass / wheat) where the full lookup's cost is wasted
float sampleShadowFast(vec3 worldPos, vec3 normal, vec3 lightDir, float viewDistance) {
	float strength = shadowData.blend.y;
	if (shadowData.params.z < 0.5 || viewDistance > shadowData.params.w || strength <= 0.0) return 1.0;

	int cascade = viewDistance < shadowData.splits.x ? 0 : (viewDistance < shadowData.splits.y ? 1 : (viewDistance < shadowData.splits.z ? 2 : 3));
	float NdotL = clamp(dot(normal, lightDir), 0.0, 1.0);
	ShadowLookup a = shadowLookup(int(shadowData.blend.z), cascade, worldPos, normal, NdotL);
	ShadowLookup b = shadowLookup(int(shadowData.blend.w), cascade, worldPos, normal, NdotL);
	float lit = mix(texture(shadowMap, vec4(a.coord.xy, a.layer, a.coord.z)), texture(shadowMap, vec4(b.coord.xy, b.layer, b.coord.z)), shadowData.blend.x);

	float fade = smoothstep(shadowData.params.w * 0.9, shadowData.params.w, viewDistance);
	return mix(1.0, lit, strength * (1.0 - fade));
}

#endif
