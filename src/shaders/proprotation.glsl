// how a rock / log / stump is turned (props.vert draws it that way, probe.comp collides with it that way)
#ifndef PROP_ROTATION_GLSL
#define PROP_ROTATION_GLSL

// the tilt the placement stores: two angles packed in one float (PropInstance params.y), x = about the mesh's z axis
// (a log's end up or down the slope), y = about its x axis (rolled to a sideways slope). together they lay it on
// the ground's normal instead of standing it upright on a slope
// (a plain number, not bits: two halves in a float's bits can look like a denormal and get flushed on a copy.
// each angle in -2..2 radians at 1/500 steps, the pair stays an exact integer below 2^24)
float packTilt(vec2 angles) {
	vec2 q = floor(clamp(angles + 2.0, 0.0, 4.0) * 500.0 + 0.5);
	return q.x * 2048.0 + q.y;
}
vec2 unpackTilt(float packed) {
	float qx = floor(packed / 2048.0);
	return vec2(qx, packed - qx * 2048.0) / 500.0 - 2.0;
}

mat3 propRotation(float yaw, float packedTilt) {
	vec2 a = unpackTilt(packedTilt);
	float ct = cos(a.x), st = sin(a.x), cr = cos(a.y), sr = sin(a.y), cy = cos(yaw), sy = sin(yaw);
	mat3 yawM = mat3(cy, 0.0, -sy, 0.0, 1.0, 0.0, sy, 0.0, cy);
	mat3 tiltM = mat3(ct, st, 0.0, -st, ct, 0.0, 0.0, 0.0, 1.0);
	mat3 rollM = mat3(1.0, 0.0, 0.0, 0.0, cr, sr, 0.0, -sr, cr);
	return yawM * tiltM * rollM;
}

#endif
