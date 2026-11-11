#version 450
// rain, snow and blowing sand: every instance is one particle in a box around the camera, its position a hash of its
// index moved by its velocity over time and wrapped into the box (world anchored: you walk through the rain, it
// doesn't follow you). rain is a streak along its velocity (motion blur at a 1/60s shutter), snow a soft flake that
// sways as it drifts down, sand a short streak racing along with the wind near the ground. each at least ~1 pixel
// wide (thinner fades instead of flickering)

#include "precipitation.glsl"

layout(location = 0) out vec2 outUV;	   // x across, y along the particle (0..1)
layout(location = 1) flat out int outKind;
layout(location = 2) out float outAlpha;
layout(location = 3) flat out vec2 outLeaf; // leaves: x = which colour (0..1), y = which face shows (-1 back .. 1 front)

uint hashU(uint x) {
	x ^= x >> 16; x *= 0x7feb352du;
	x ^= x >> 15; x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}
vec3 hash3(uint i) {
	return vec3(hashU(i * 3u + 1u), hashU(i * 3u + 2u), hashU(i * 3u + 3u)) / 4294967295.0;
}

void main() {
	int instance = gl_InstanceIndex;
	int kind = instance < pc.counts.x ? KIND_RAIN : (instance < pc.counts.x + pc.counts.y ? KIND_SNOW : (instance < pc.counts.x + pc.counts.y + pc.counts.z ? KIND_SAND : KIND_LEAF));
	outLeaf = vec2(0.0);
	vec3 r = hash3(uint(instance) * 7919u + uint(kind) * 104729u);
	vec3 r2 = hash3(uint(instance) * 31u + 17u);

	// the box (half size, world units ~6m): the near field only, ~30m around you for rain, ~35m for snow, sand hugging
	// the ground. what's further away is the volumetric fog's rain haze (weather.cpp); spread over ~170m the drops
	// were so sparse a heavy rain showed a handful of streaks
	// (leaves: ~17m around you, from the ground up into the lower crowns: further out they were specks)
	vec3 halfSize = kind == KIND_RAIN ? vec3(5.0, 4.0, 5.0) : (kind == KIND_SNOW ? vec3(6.0, 5.0, 6.0) : (kind == KIND_SAND ? vec3(7.0, 2.0, 7.0) : vec3(3.0, 1.2, 3.0)));
	// each particle its own: drop size sets how fast rain falls (~7-11 m/s) and how much the wind carries it, snow
	// flakes and sand grains vary more. the wind itself gusts: two slow waves of strength that sweep across the scene
	// (their phase goes with the particle's place), so neighbouring drops lean together and the next patch differently.
	// the position is the exact integral of that velocity, so the streaks (along the velocity) match how they move
	vec3 worldCam = pc.campos + vec3(pc.offsetWorld.x, 0.0, pc.offsetWorld.y);
	vec3 base = (r * 2.0 - 1.0) * halfSize + vec3(r2.y, 0.0, r2.z) * 1000.0;
	float fall, drag, gustiness;
	if (kind == KIND_RAIN)		{ fall = 1.2 + r2.x * 0.6;  drag = 0.35 + r2.y * 0.35; gustiness = 0.55; }
	else if (kind == KIND_SNOW) { fall = 0.1 + r2.x * 0.14; drag = 0.6 + r2.y * 0.4;   gustiness = 0.8; }
	else if (kind == KIND_SAND) { fall = 0.0;				drag = 1.0 + r2.y * 0.6;   gustiness = 0.7; }
	else						{ fall = 0.12 + r2.x * 0.1; drag = 0.45 + r2.y * 0.4;  gustiness = 1.0; } // leaves: flutter down slowly, swept along in the gusts
	// where it is with the steady wind alone: the gusts' phase goes by that place in the world (stable as you move)
	vec3 steady = base + vec3(pc.wind.x * drag * pc.time, -fall * pc.time, pc.wind.y * drag * pc.time);
	vec3 place = worldCam + mod(steady - worldCam + halfSize, 2.0 * halfSize) - halfSize;
	vec2 windDir = normalize(pc.wind + 1e-5);
	float front = dot(place.xz, windDir) * 0.04 + dot(place.xz, vec2(-windDir.y, windDir.x)) * 0.015;
	const float w1 = 0.45, w2 = 1.3;
	float p1 = front, p2 = front * 1.7 + 2.1;
	float a1 = gustiness * 0.7, a2 = gustiness * 0.3;
	float gustSpeed = 1.0 + a1 * cos(w1 * pc.time - p1) + a2 * cos(w2 * pc.time - p2); // the wind's strength now
	float gustOffset = a1 / w1 * sin(w1 * pc.time - p1) + a2 / w2 * sin(w2 * pc.time - p2); // its integral, minus the steady part
	vec3 velocity = vec3(pc.wind.x * drag * gustSpeed, -fall, pc.wind.y * drag * gustSpeed);
	vec3 moved = steady + vec3(pc.wind.x * drag * gustOffset, 0.0, pc.wind.y * drag * gustOffset);
	if (kind != KIND_RAIN) {
		// flakes and grains tumble: a little swirl of their own on top (and its velocity)
		float s1 = 0.8 + r2.y, s2 = 0.6 + r2.z, amp = kind == KIND_SNOW ? 0.1 : (kind == KIND_LEAF ? 0.18 : 0.06);
		moved.xz += vec2(sin(pc.time * s1 + r2.z * 6.28), cos(pc.time * s2 + r2.y * 6.28)) * amp;
		velocity.xz += vec2(cos(pc.time * s1 + r2.z * 6.28) * s1, -sin(pc.time * s2 + r2.y * 6.28) * s2) * amp;
		if (kind == KIND_SAND) { moved.y += sin(pc.time * 1.7 + r2.x * 6.28) * 0.12; velocity.y += cos(pc.time * 1.7 + r2.x * 6.28) * 0.2; }
		// a leaf rocks side to side as it falls (the falling leaf's pendulum), lifted for a moment at each end of the swing
		if (kind == KIND_LEAF) { float s = 2.2 + r2.z * 1.5; moved.y += abs(sin(pc.time * s + r.x * 6.28)) * 0.08; moved.xz += vec2(sin(pc.time * s + r.x * 6.28)) * vec2(0.06, -0.04); }
	}
	// wrapped into the box around the camera
	vec3 rel = mod(moved - worldCam + halfSize, 2.0 * halfSize) - halfSize;
	if (kind == KIND_SAND) rel.y = (r.y * 2.0 - 1.0) * halfSize.y - 0.6 + (moved.y - base.y); // near the ground (the camera's ~eye height above it)
	vec3 position = pc.campos + rel;
	float dist = length(rel);

	// fades: at the box's edge (no popping as they wrap) and right at the camera (giant streaks across the screen)
	float alpha = (1.0 - smoothstep(0.65, 1.0, length(rel / halfSize))) * smoothstep(0.12, 0.6, dist);
	// leaves: from about the ground (what's under it is hidden by the depth test) up into the crowns, and right up to
	// you (they're small and opaque, no screen filling streak)
	if (kind == KIND_LEAF) {
		position.y += 0.8;
		alpha = (1.0 - smoothstep(0.7, 1.0, length(rel / halfSize))) * smoothstep(0.03, 0.08, length(position - pc.campos));
	}

	// the quad: along the motion (streaks) or facing the screen (flakes), in pixels
	vec2 corner = vec2(gl_VertexIndex == 1 || gl_VertexIndex == 2 || gl_VertexIndex == 4 ? 1.0 : 0.0,
					   gl_VertexIndex == 2 || gl_VertexIndex == 4 || gl_VertexIndex == 5 ? 1.0 : 0.0);
	outUV = corner;
	outKind = kind;
	vec4 head = pc.viewProj * vec4(position, 1.0);
	if (head.w <= 0.05) { gl_Position = vec4(0.0, 0.0, -1.0, 1.0); outAlpha = 0.0; return; } // behind: off screen
	vec2 pixel = 2.0 / pc.view.xy; // one pixel in NDC
	if (kind == KIND_LEAF) {
		// ~8cm, spinning and flipping over as it tumbles: the quad turned on screen and squashed along one side as the
		// leaf turns edge on
		float sizePx = 0.014 * (0.7 + 0.6 * r2.z) * pc.view.z / head.w;
		alpha *= clamp(sizePx / 2.0, 0.3, 1.0);
		sizePx = max(sizePx, 2.0);
		float spin = pc.time * (1.5 + 2.5 * r2.x) * (r.z > 0.5 ? 1.0 : -1.0) + r.y * 6.28;
		float flip = cos(pc.time * (2.0 + 3.0 * r2.y) + r2.z * 6.28);
		vec2 q = (corner * 2.0 - 1.0) * vec2(max(abs(flip), 0.12), 0.6) * sizePx * 0.5;
		q = mat2(cos(spin), sin(spin), -sin(spin), cos(spin)) * q;
		gl_Position = head + vec4(q * pixel * head.w, 0.0, 0.0);
		outLeaf = vec2(fract(r.x * 13.7 + r2.y * 5.1), flip);
	} else if (kind == KIND_SNOW) {
		float sizePx = 0.02 * pc.view.z / head.w; // ~12cm: bigger than real flakes, they read as flakes at a distance
		alpha *= clamp(sizePx / 1.5, 0.35, 1.0);  // smaller than a pixel and a half: fades instead (not below a third)
		sizePx = max(sizePx, 1.5);
		gl_Position = head + vec4((corner * 2.0 - 1.0) * sizePx * 0.5 * pixel * head.w, 0.0, 0.0);
	} else {
		// a streak from where it is back along its velocity: rain ~1/30s of its fall (~0.3m), sand a shorter dash
		// the particle's own velocity only: how you move doesn't bend the streaks
		float streak = kind == KIND_RAIN ? 0.035 : 0.05;
		vec4 tail = pc.viewProj * vec4(position - velocity * streak, 1.0);
		tail.w = max(tail.w, 0.05);
		vec2 a = head.xy / head.w, b = tail.xy / tail.w;
		vec2 along = (a - b) / pixel;
		float lengthPx = max(length(along), 1.0);
		vec2 dir = along / lengthPx;
		vec2 side = vec2(-dir.y, dir.x);
		float widthWorld = kind == KIND_RAIN ? 0.0025 : 0.004; // ~1.5cm drops, a few mm of sand
		float widthPx = widthWorld * pc.view.z / head.w;
		alpha *= clamp(widthPx / 1.2, 0.55, 1.0); // a sub pixel streak still shows (it was faded to nothing)
		widthPx = max(widthPx, 1.2);
		vec4 end = corner.y > 0.5 ? head : tail;
		vec2 ndc = end.xy / end.w + side * (corner.x - 0.5) * widthPx * pixel;
		gl_Position = vec4(ndc * end.w, end.z, end.w);
	}
	outAlpha = alpha;
}
