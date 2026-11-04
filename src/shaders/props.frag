#version 450
// rocks: the terrain's rock texture projected from three sides (they're all curves), moss on the tops where it's
// wet. logs: bark around them, rings on the cut ends. deferred, lit in lighting.comp

#define GBUFFER_WRITE
#include "gbuffer.glsl"
#include "propscommon.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUV;
layout(location = 3) flat in int inMaterial;
layout(location = 4) in float inAO;
layout(location = 5) flat in float inMoss;
layout(location = 6) flat in float inSeed;

// 0 rock albedo, 1 rock normal, 2 bark albedo, 3 bark normal, 4 moss (the grass texture), 5 + 6 end grain albedo
// + normal (a photographed log cross section, assets/wood_endgrain*.jpg; the bark stands in when it's missing)
layout(set = 0, binding = 1) uniform sampler2D textures[7];
layout(constant_id = 0) const bool END_GRAIN_TEXTURE = false; // Props::init: the end grain images exist

vec3 unpackNormal(vec2 xy) {
	vec2 n = xy * 2.0 - 1.0;
	return vec3(n, sqrt(max(1.0 - dot(n, n), 0.0)));
}

// normal mapping without stored tangents (Schueler 2013), for the logs' uv mapped bark
vec3 perturbNormal(vec3 N, vec3 dp1, vec3 dp2, vec2 duv1, vec2 duv2, vec3 mapNormal) {
	vec3 dp2perp = cross(dp2, N);
	vec3 dp1perp = cross(N, dp1);
	vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
	vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
	float invmax = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-12));
	return normalize(mat3(T * invmax, B * invmax, N) * mapNormal);
}

void main() {
	vec3 normal = normalize(inNormal);
	vec3 dp1 = dFdx(inPosition), dp2 = dFdy(inPosition);
	vec2 duv1 = dFdx(inUV), duv2 = dFdy(inUV);
	vec3 albedo;
	float roughness = 1.0;
	int material = MAT_TERRAIN;

	if (inMaterial == 0) {
		// triplanar, blended by the normal (sharpened so the seams are narrow), whiteout normal blend
		vec3 w = pow(abs(normal), vec3(4.0));
		w /= w.x + w.y + w.z;
		vec3 p = inPosition / 0.8 + inSeed * 37.0; // ~5m texture repeat, a different spot of it per rock
		vec2 uvX = p.zy, uvY = p.xz, uvZ = p.xy;
		albedo = texture(textures[0], uvX).rgb * w.x + texture(textures[0], uvY).rgb * w.y + texture(textures[0], uvZ).rgb * w.z;
		vec3 nX = unpackNormal(texture(textures[1], uvX).rg), nY = unpackNormal(texture(textures[1], uvY).rg), nZ = unpackNormal(texture(textures[1], uvZ).rg);
		vec3 s = sign(normal);
		vec3 tX = vec3(nX.xy + normal.zy, abs(nX.z) * normal.x);
		vec3 tY = vec3(nY.xy + normal.xz, abs(nY.z) * normal.y);
		vec3 tZ = vec3(nZ.xy + normal.xy, abs(nZ.z) * normal.z);
		normal = normalize(tX.zyx * w.x + tY.xzy * w.y + tZ.xyz * w.z);
		albedo = pow(albedo, vec3(2.2)) * mix(0.8, 1.1, fract(inSeed * 13.7)); // a bit of variation per rock

		// moss on the tops in wet, shaded places
		float mossCover = smoothstep(0.35, 0.8, normal.y + (texture(textures[1], uvY * 0.37).r - 0.5) * 0.6) * inMoss;
		vec3 moss = pow(texture(textures[4], uvY * 2.0).rgb, vec3(2.2)) * vec3(0.8, 0.95, 0.6);
		albedo = mix(albedo, moss, mossCover);
	} else if (inMaterial == 1) {
		albedo = pow(texture(textures[2], inUV).rgb, vec3(2.2)) * 0.85; // a bit darker than the standing trees, weathered
		vec2 n = texture(textures[3], inUV).rg * 2.0 - 1.0;
		normal = perturbNormal(normal, dp1, dp2, duv1, duv2, vec3(n, sqrt(max(1.0 - dot(n, n), 0.0))));
		material = MAT_BARK;
		roughness = 0.85;
		float mossCover = smoothstep(0.4, 0.9, normal.y) * inMoss * 0.8;
		albedo = mix(albedo, pow(texture(textures[4], inUV * 3.0).rgb, vec3(2.2)) * vec3(0.8, 0.95, 0.6), mossCover);
	} else {
		// cut end (uv 0.5, 0.5 = the pith, 1 = the bark edge)
		float r = length(inUV - 0.5) * 2.0;
		if (END_GRAIN_TEXTURE) {
			// the photographed cross section, turned a different way per prop so they don't all match, weathered
			// grey and darker towards the bark, the same way the logs' bark is
			float a = inSeed * 6.2831853;
			mat2 turn = mat2(cos(a), sin(a), -sin(a), cos(a));
			vec2 uv = turn * (inUV - 0.5) * 0.94 + 0.5; // a little inside the photo's edge (its own bark ring)
			albedo = pow(texture(textures[5], uv).rgb, vec3(2.2));
			float grey = dot(albedo, vec3(0.3, 0.5, 0.2));
			albedo = mix(albedo, vec3(grey) * vec3(1.0, 0.95, 0.88), 0.35) * 0.8 * mix(1.0, 0.55, smoothstep(0.8, 1.0, r));
			vec2 n = texture(textures[6], uv).rg * 2.0 - 1.0;
			normal = perturbNormal(normal, dp1, dp2, duv1, duv2, vec3(n, sqrt(max(1.0 - dot(n, n), 0.0))));
		} else {
			// no texture: growth rings around the centre, pale wood darkened by weather
			float rings = 0.5 + 0.5 * sin(r * 40.0 + sin(atan(inUV.y - 0.5, inUV.x - 0.5) * 3.0) * 0.6);
			albedo = mix(vec3(0.32, 0.22, 0.12), vec3(0.45, 0.33, 0.2), rings) * mix(1.0, 0.5, smoothstep(0.85, 1.0, r));
		}
		material = MAT_BARK;
		roughness = 0.9;
	}
	writeGBuffer(albedo, inAO, normal, material, roughness, mix(0.7, 1.0, inAO));
}
