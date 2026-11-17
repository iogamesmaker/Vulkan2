#ifndef NORMALMAP_GLSL
#define NORMALMAP_GLSL

// a two channel (BC5) normal map texel -> tangent space normal
vec3 unpackNormal(vec2 xy) {
	vec2 n = xy * 2.0 - 1.0;
	return vec3(n, sqrt(max(1.0 - dot(n, n), 0.0)));
}

// normal mapping without stored tangents (Schueler 2013): the tangent frame from the screen space derivatives
vec3 perturbNormal(vec3 N, vec3 dp1, vec3 dp2, vec2 duv1, vec2 duv2, vec3 mapNormal) {
	vec3 dp2perp = cross(dp2, N);
	vec3 dp1perp = cross(N, dp1);
	vec3 T = dp2perp * duv1.x + dp1perp * duv2.x;
	vec3 B = dp2perp * duv1.y + dp1perp * duv2.y;
	float invmax = inversesqrt(max(max(dot(T, T), dot(B, B)), 1e-12));
	return normalize(mat3(T * invmax, B * invmax, N) * mapNormal);
}

// the side of a surface that faces the camera, from the triangle's own orientation (its screen space derivatives):
// gl_FrontFacing can't be trusted for it here (the projection flips y, so the side facing the camera counts as the
// back face and every visible bark / leaf got its normal reversed: lit from the side away from the sun)
vec3 normalTowardsCamera(vec3 normal, vec3 dp1, vec3 dp2, vec3 toCamera) {
	vec3 face = cross(dp1, dp2);
	if (dot(face, toCamera) < 0.0) face = -face;
	return dot(normal, face) < 0.0 ? -normal : normal;
}

#endif
