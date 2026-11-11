// the player's trail through the vegetation (PlayerTrail, interaction.cpp): where the feet were over the last few
// seconds. grass, bushes and low leaves bend away from it and spring back with a damped wobble once you've passed
// define INTERACTION_SET and INTERACTION_BINDING before including this
#ifndef INTERACTION_GLSL
#define INTERACTION_GLSL

#define INTERACTION_POINTS 16 // PlayerTrail::POINTS

layout(std140, set = INTERACTION_SET, binding = INTERACTION_BINDING) uniform Interaction {
	vec4 trailPoints[INTERACTION_POINTS]; // xyz = feet (terrain space), w = seconds since the player was there
	vec4 trailInfo;						  // x = points in use, y = body height (feet -> eye, world units), z = the weather's wind 0..1, w = how far the gusts have blown (windfield.glsl)
} interaction;

// how a point at p reacts to the trail. reach = horizontal radius of influence (world units), above = how far above
// the feet it still gets touched (below the feet a little too, for slopes)
//   xz = push direction * amount (the amount goes negative for a moment as it swings back past rest)
//   y  = how flattened it is (0..1, only while it's being pushed)
//   w  = how disturbed it is (the wobble's magnitude, for extra flutter)
vec4 interactionPush(vec3 p, float reach, float above) {
	vec2 push = vec2(0.0);
	float flatten = 0.0, disturb = 0.0;
	int count = int(interaction.trailInfo.x);
	for (int i = 0; i < count; i++) {
		vec4 t = interaction.trailPoints[i];
		vec2 away = p.xz - t.xz;
		float d = length(away);
		float dy = p.y - t.y;
		if (d >= reach || dy < -0.35 * above || dy > above) continue;
		float w = 1.0 - d / reach;
		w *= w;
		// pushed while you're there, then back: a damped spring (overshoots past rest once, settles)
		float age = t.w;
		float spring = exp(-age * 2.4) * cos(age * 5.5);
		push += away / max(d, 1e-4) * w * spring;
		flatten = max(flatten, w * max(spring, 0.0));
		disturb = max(disturb, w * exp(-age * 1.5));
	}
	float len = length(push);
	if (len > 1.0) push /= len;
	return vec4(push.x, flatten, push.y, disturb);
}

#endif
