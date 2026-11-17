#version 450
// birds, bats and butterflies (wildlife.hpp): the shapes, placed and posed by wildlife.comp's Critter. a bird is a body,
// a tail and two wings in two segments that bend as they flap, a butterfly a body and two wings (their outline and
// pattern are the fragment shader's). no vertex buffers, BIRD_VERTICES per instance

#include "wildlifecommon.glsl"

layout(std430, set = 0, binding = 2) readonly buffer Critters { Critter critters[]; };

layout(location = 0) out vec3 outPosition;
layout(location = 1) out vec3 outPrevPosition;
layout(location = 2) flat out vec3 outColour;
layout(location = 3) flat out vec3 outColour2;
layout(location = 4) flat out vec3 outUp;
layout(location = 5) out vec2 outUV;
layout(location = 6) flat out int outKind;
layout(location = 7) flat out int outPattern;

// the bird's shape, wingspan 1, z forward, y up, w = 0 body, 1 inner wing, 2 outer wing (x > 0 one side, mirrored)
const vec4 BIRD[BIRD_VERTICES] = vec4[](
	// body: a flat and an upright diamond
	vec4(0.0, 0.0, 0.2, 0.0), vec4(0.045, 0.0, 0.02, 0.0), vec4(0.0, 0.0, -0.14, 0.0),
	vec4(0.0, 0.0, 0.2, 0.0), vec4(0.0, 0.0, -0.14, 0.0), vec4(-0.045, 0.0, 0.02, 0.0),
	vec4(0.0, 0.0, 0.2, 0.0), vec4(0.0, 0.035, 0.03, 0.0), vec4(0.0, 0.0, -0.14, 0.0),
	vec4(0.0, 0.0, 0.2, 0.0), vec4(0.0, 0.0, -0.14, 0.0), vec4(0.0, -0.04, 0.02, 0.0),
	// tail
	vec4(0.0, 0.0, -0.12, 0.0), vec4(0.06, 0.0, -0.27, 0.0), vec4(-0.06, 0.0, -0.27, 0.0),
	// one wing: shoulder to elbow, elbow to tip
	vec4(0.03, 0.0, 0.07, 1.0), vec4(0.24, 0.0, 0.06, 1.0), vec4(0.24, 0.0, -0.09, 1.0),
	vec4(0.03, 0.0, 0.07, 1.0), vec4(0.24, 0.0, -0.09, 1.0), vec4(0.03, 0.0, -0.07, 1.0),
	vec4(0.24, 0.0, 0.06, 2.0), vec4(0.42, 0.0, 0.0, 2.0), vec4(0.5, 0.0, -0.06, 2.0),
	vec4(0.24, 0.0, 0.06, 2.0), vec4(0.5, 0.0, -0.06, 2.0), vec4(0.24, 0.0, -0.09, 2.0),
	// the other
	vec4(-0.03, 0.0, 0.07, 1.0), vec4(-0.24, 0.0, 0.06, 1.0), vec4(-0.24, 0.0, -0.09, 1.0),
	vec4(-0.03, 0.0, 0.07, 1.0), vec4(-0.24, 0.0, -0.09, 1.0), vec4(-0.03, 0.0, -0.07, 1.0),
	vec4(-0.24, 0.0, 0.06, 2.0), vec4(-0.42, 0.0, 0.0, 2.0), vec4(-0.5, 0.0, -0.06, 2.0),
	vec4(-0.24, 0.0, 0.06, 2.0), vec4(-0.5, 0.0, -0.06, 2.0), vec4(-0.24, 0.0, -0.09, 2.0));

// a butterfly, metres, w = 0 body, 1 wing (x > 0 one side, mirrored)
const vec4 BUTTERFLY[18] = vec4[](
	vec4(0.0, 0.0, 0.011, 0.0), vec4(0.0022, 0.0, -0.001, 0.0), vec4(0.0, 0.0, -0.013, 0.0),
	vec4(0.0, 0.0, 0.011, 0.0), vec4(0.0, 0.0, -0.013, 0.0), vec4(-0.0022, 0.0, -0.001, 0.0),
	vec4(0.0015, 0.0, 0.006, 1.0), vec4(0.03, 0.0, 0.011, 1.0), vec4(0.024, 0.0, -0.016, 1.0),
	vec4(0.0015, 0.0, 0.006, 1.0), vec4(0.024, 0.0, -0.016, 1.0), vec4(0.0015, 0.0, -0.011, 1.0),
	vec4(-0.0015, 0.0, 0.006, 1.0), vec4(-0.03, 0.0, 0.011, 1.0), vec4(-0.024, 0.0, -0.016, 1.0),
	vec4(-0.0015, 0.0, 0.006, 1.0), vec4(-0.024, 0.0, -0.016, 1.0), vec4(-0.0015, 0.0, -0.011, 1.0));

// a wing vertex (x > 0 side, mesh units) rotated up about the shoulder, the outer part further about the elbow
vec3 flapWing(vec3 p, float part, vec2 angles, float shoulder, float elbow) {
	float c1 = cos(angles.x), s1 = sin(angles.x);
	if (part < 1.5) return vec3(shoulder + (p.x - shoulder) * c1 - p.y * s1, (p.x - shoulder) * s1 + p.y * c1, p.z);
	vec2 e = vec2(shoulder + (elbow - shoulder) * c1, (elbow - shoulder) * s1);
	float c2 = cos(angles.x + angles.y), s2 = sin(angles.x + angles.y);
	return vec3(e.x + (p.x - elbow) * c2 - p.y * s2, e.y + (p.x - elbow) * s2 + p.y * c2, p.z);
}

void main() {
	Critter c = critters[gl_InstanceIndex];
	uint vertex = uint(gl_VertexIndex);
	bool butterfly = int(c.previous.w + 0.5) == CRITTER_BUTTERFLY;
	outPosition = outPrevPosition = vec3(0.0);
	outColour = outColour2 = outUp = vec3(0.0);
	outUV = vec2(0.0);
	outKind = CRITTER_BIRD;
	outPattern = 0;
	if (c.position.w <= 0.0 || (butterfly && vertex >= 18u)) {
		gl_Position = vec4(2.0, 2.0, 2.0, 1.0); // outside the view, and every vertex the same: a degenerate triangle
		return;
	}

	vec4 v;
	vec3 local;
	float shoulder, elbow;
	if (butterfly) {
		v = BUTTERFLY[vertex];
		local = v.xyz;
		shoulder = 0.0015;
		elbow = 1.0; // (no outer segment)
	} else {
		v = BIRD[vertex];
		local = v.xyz;
		if (v.w < 0.5) { if (local.z > 0.05) local.z *= c.up.w; } // a goose's long neck
		else local.z *= c.right.w;								  // broad wings
		local.yz *= c.colour2.w;								  // far away: thicker, so it still covers pixels
		shoulder = 0.03;
		elbow = 0.24;
	}
	vec3 now = local, before = local;
	if (v.w > 0.5) {
		float side = sign(local.x);
		vec3 mirrored = vec3(abs(local.x), local.yz);
		now = flapWing(mirrored, v.w, c.wings.xy, shoulder, elbow);
		before = flapWing(mirrored, v.w, c.wings.zw, shoulder, elbow);
		now.x *= side;
		before.x *= side;
	}
	mat3 frame = mat3(c.right.xyz, c.up.xyz, cross(c.right.xyz, c.up.xyz));
	outPosition = c.position.xyz + frame * now * c.position.w;
	outPrevPosition = c.previous.xyz + frame * before * c.position.w;
	outUp = c.up.xyz;
	if (butterfly) {
		outUV = vec2(abs(v.x) / 0.03, (v.z + 0.016) / 0.027);
		outColour = v.w > 0.5 ? c.colour.rgb : vec3(0.03, 0.025, 0.02); // dark bodies
		outColour2 = v.w > 0.5 ? c.colour2.rgb : outColour;
		outKind = v.w > 0.5 ? CRITTER_BUTTERFLY : CRITTER_BIRD;
		outPattern = int(c.colour.w + 0.5);
	} else {
		outColour = c.colour.rgb;
		outColour2 = c.colour2.rgb;
	}
	gl_Position = pc.viewproj * vec4(outPosition, 1.0);
}
