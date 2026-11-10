#version 450
// the ground cover plants (GroundCover::init loads them from assets/plants), instanced from groundcover.comp's bins,
// swaying in the same wind as the grass

#include "groundcovercommon.glsl"
#define INTERACTION_SET 0
#define INTERACTION_BINDING 2
#include "interaction.glsl" // trailInfo.z: the weather's wind

layout(location = 0) in vec4 inPositionSpecies; // xyz mesh space (world units), w = species
layout(location = 1) in vec4 inNormalHeight;	// xyz normal, w = height up the plant, 0 at the ground .. 1 at its top
layout(location = 2) in vec4 inUV;

layout(std430, set = 0, binding = 0) readonly buffer Instances { GroundCoverInstance instances[]; };

invariant gl_Position;

layout(location = 0) out vec3 outPosition;
layout(location = 1) out vec3 outPrevPosition;
layout(location = 2) out vec3 outNormal;
layout(location = 3) out vec2 outUV;
layout(location = 4) out float outHeight;
layout(location = 5) flat out int outSpecies;
layout(location = 6) flat out float outTint;

vec3 windOffset(float time, vec3 local, float height, float phase, float amount) {
	vec2 windDir = normalize(vec2(0.8, 0.35)); // foliage.vert's
	float gust = sin(time * 1.3 + phase) * 0.5 + 0.5;
	float flutter = sin(time * 5.3 + phase * 3.0 + dot(local, vec3(60.0, 40.0, 50.0))) * 0.25;
	// bends the tops most (height squared), by how tall the plant is
	float bend = (gust * 0.12 + 0.03 + flutter * 0.04) * amount * height * height;
	return vec3(windDir.x, -0.15, windDir.y) * bend * max(local.y, 0.0) * 1.5
		 + vec3(windDir.y, 0.3, -windDir.x) * flutter * 0.015 * amount * height * max(local.y, 0.0);
}

void main() {
	GroundCoverInstance inst = instances[gl_InstanceIndex];
	float cy = cos(inst.params.x), sy = sin(inst.params.x);
	mat3 yaw = mat3(cy, 0.0, -sy, 0.0, 1.0, 0.0, sy, 0.0, cy);
	vec3 local = yaw * inPositionSpecies.xyz * inst.posScale.w;
	float height = inNormalHeight.w;
	float phase = dot(inst.posScale.xz + pc.offsetWorld, normalize(vec2(0.8, 0.35))) * 2.5 + inst.params.z * 6.0;

	float amount = inst.params.w * mix(0.5, 2.2, interaction.trailInfo.z); // the weather's wind
	vec3 pos = inst.posScale.xyz + local + windOffset(pc.time, local, height, phase, amount);
	vec3 prev = inst.posScale.xyz + local + windOffset(pc.sundir.x, local, height, phase, amount); // last frame
	outPosition = pos;
	outPrevPosition = prev;
	outNormal = yaw * inNormalHeight.xyz;
	outUV = inUV.xy;
	outHeight = height;
	outSpecies = int(inPositionSpecies.w + 0.5);
	outTint = inst.params.y;
	gl_Position = pc.viewproj * vec4(pos, 1.0);
}
