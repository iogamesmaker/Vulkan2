#version 450
// birds, bats and butterflies (wildlife.vert) into the G-buffer: flat shaded, lit in lighting.comp

#define GBUFFER_WRITE
#include "gbuffer.glsl"
#include "wildlifecommon.glsl"

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inPrevPosition;
layout(location = 2) flat in vec3 inColour;
layout(location = 3) flat in vec3 inColour2;
layout(location = 4) flat in vec3 inUp;
layout(location = 5) in vec2 inUV;
layout(location = 6) flat in int inKind;
layout(location = 7) flat in int inPattern;

void main() {
	// the face's own normal, turned towards the camera (the wings are seen from both sides)
	vec3 normal = normalize(cross(dFdx(inPosition), dFdy(inPosition)));
	vec3 toCamera = pc.campos - inPosition;
	if (dot(normal, toCamera) < 0.0) normal = -normal;
	float below = smoothstep(0.1, -0.3, dot(normal, inUp)); // looking at its underside

	if (inKind == CRITTER_BUTTERFLY) {
		// the wing's outline: a forewing and a hindwing lobe (uv: x out from the body, y back to front)
		vec2 uv = inUV;
		float fore = length((uv - vec2(0.5, 0.74)) / vec2(0.52, 0.3));
		float hind = length((uv - vec2(0.42, 0.3)) / vec2(0.45, 0.33));
		float inside = min(fore, hind);
		if (inside > 1.0 && uv.x > 0.12) discard;

		vec3 albedo = inColour;
		if (inPattern == 0) {
			if (uv.x > 0.72 && uv.y > 0.62) albedo = inColour2; // cabbage white: black tips
			if (distance(uv, vec2(0.55, 0.62)) < 0.07) albedo = inColour2;
		} else if (inPattern == 1) {
			if (distance(uv, vec2(0.5, 0.66)) < 0.05 || distance(uv, vec2(0.42, 0.3)) < 0.05) albedo = inColour2; // brimstone: orange dots
		} else if (inPattern == 2) {
			if (inside > 0.8 || uv.x < 0.18) albedo = inColour2; // tortoiseshell: dark margins and body side
			if (uv.y > 0.7 && fract(uv.x * 4.0) < 0.35 && uv.x < 0.8) albedo = inColour2;
		} else if (inPattern == 3) {
			if (inside > 0.86) albedo = inColour2; // common blue: a white fringe
			if (uv.x < 0.25) albedo *= 0.6;
		} else {
			float eye = distance(uv, vec2(0.78, 0.74));
			if (eye < 0.15) albedo = eye < 0.07 ? vec3(0.02) : inColour2; // peacock: eye spots
			if (distance(uv, vec2(0.62, 0.3)) < 0.1) albedo = inColour2;
		}
		// the undersides are dull, mottled brown / grey
		float luma = dot(albedo, vec3(0.3, 0.59, 0.11));
		albedo = mix(albedo, vec3(0.25, 0.22, 0.17) * (0.5 + luma), below * 0.6);
		writeGBuffer(albedo, 1.0, normal, MAT_LEAF, 0.6, 1.0);
	} else {
		writeGBuffer(mix(inColour, inColour2, below), 1.0, normal, MAT_TERRAIN, 0.9, 1.0);
	}
	writeVelocity(pc.viewproj, inPosition, inPrevPosition);
}
