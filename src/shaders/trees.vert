#version 450
// the imported tree meshes (tools/import_trees.py, Trees::loadMeshes), instanced from the scatter pass, swaying in the wind

#include "treecommon.glsl"
#define INTERACTION_SET 0
#define INTERACTION_BINDING 2
#include "interaction.glsl"
#include "windfield.glsl"

layout(location = 0) in vec4 inPositionWind; // xyz in tree space (height 1), w = how much it sways
layout(location = 1) in vec4 inNormalMaterial; // xyz normal, w = material: pack * 2 + 1 for the leaves
layout(location = 2) in vec4 inUVAO;			 // xy uv, z = ambient occlusion

layout(std430, set = 0, binding = 0) readonly buffer Instances { TreeInstance instances[]; };

invariant gl_Position; // the prepass and the shading pass have to land on exactly the same depth

layout(location = 0) out vec3 outPosition;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) flat out int outMaterial;
layout(location = 4) out float outAO;
layout(location = 5) flat out float outDryness;
layout(location = 6) flat out float outFade;
layout(location = 7) flat out float outSeed;
layout(location = 8) out vec3 outPrevPosition; // with last frame's wind (motion vectors)
layout(location = 9) flat out float outTint;	// unused (was the procedural trees' colouring)

void main() {
	TreeInstance inst = instances[gl_InstanceIndex];
	float yaw;
	vec2 lean;
	unpackYawLean(inst.params.x, yaw, lean);
	float c = cos(yaw), s = sin(yaw);
	mat2 rotation = mat2(c, s, -s, c);

	vec3 local = inPositionWind.xyz;
	local.xz = rotation * local.xz;
	vec3 normal = inNormalMaterial.xyz;
	normal.xz = rotation * normal.xz;
	// the lean (trees_place.comp): the whole tree turned about its foot towards the lean's direction (a rotation,
	// not a shear: sheared, the trunk slid sideways with height and the crown stretched, it looked skewed)
	float leanAmount = length(lean);
	if (leanAmount > 1e-4) {
		vec3 axis = vec3(lean.y, 0.0, -lean.x) / leanAmount; // up x the lean's direction
		float angle = atan(leanAmount), c = cos(angle), s = sin(angle);
		local = local * c + cross(axis, local) * s + axis * dot(axis, local) * (1.0 - c);
		normal = normal * c + cross(axis, normal) * s + axis * dot(axis, normal) * (1.0 - c);
	}

	float treeHeight = inst.posScale.w;
	vec3 pos = inst.posScale.xyz + local * treeHeight;

	// wind: the whole tree leans as the gusts roll through the forest (windfield.glsl, the same ones as over the
	// grass: one passes through tree after tree downwind), rocking a little on its own, leaves flutter on top
	vec2 windDir = WIND_DIR;
	vec2 worldXZ = inst.posScale.xz + pc.offsetWorld;
	float phase = dot(worldXZ, windDir) * 0.15 + inst.params.z;
	float travel = interaction.trailInfo.w, travelBefore = travel - (pc.time - pc.sundir.x) * windSpeed(interaction.trailInfo.z);
	// (the crown catches it a moment after the grass at its foot)
	float gust = windGust(worldXZ, travel - 0.3), gustBefore = windGust(worldXZ, travelBefore - 0.3);
	float sway = 0.2 + gust * 0.85 + sin(pc.time * 0.7 + phase) * 0.12 + sin(pc.time * 1.9 + phase * 1.7) * (0.08 + 0.15 * gust);
	float swayBefore = 0.2 + gustBefore * 0.85 + sin(pc.sundir.x * 0.7 + phase) * 0.12 + sin(pc.sundir.x * 1.9 + phase * 1.7) * (0.08 + 0.15 * gustBefore); // last frame
	float bend = inPositionWind.w * inPositionWind.w;
	float gale = mix(0.5, 2.4, interaction.trailInfo.z); // the weather's wind (interaction.glsl)
	sway *= gale;
	swayBefore *= gale;
	pos.xz += windDir * sway * bend * treeHeight * 0.012;
	vec3 windBefore = vec3(0.0); // last frame's position = pos + windBefore
	windBefore.xz = windDir * (swayBefore - sway) * bend * treeHeight * 0.012;
	// walking into a bush or low branches pushes the leaves aside (and they swing back after you, see
	// interaction.glsl), the loose tips (more wind weight) the most
	float disturbed = 0.0;
	bool leaves = (int(inNormalMaterial.w + 0.5) & 1) == 1;
	if (leaves && interaction.trailInfo.x > 0.5) {
		const float reach = 0.17; // ~1m
		vec4 touch = interactionPush(pos, reach, interaction.trailInfo.y + 0.06);
		float give = 0.3 + 0.7 * inPositionWind.w;
		vec3 push = vec3(touch.x, 0.25 * touch.y, touch.z) * reach * give;
		pos += push;
		disturbed = touch.w * give;
	}
	if (leaves) {
		// leaf flutter, the phase from the position before it so last frame's is the same function. brushing past
		// shakes them harder for a moment
		float flutterScale = inPositionWind.w * treeHeight * 0.003 * (0.5 + gust) + disturbed * 0.012;
		float px = dot(pos, vec3(7.0)), pz = dot(pos, vec3(5.0));
		vec3 flutter = vec3(sin(pc.time * 3.1 + px), sin(pc.time * 2.7 + pz), 0.0).xzy * flutterScale;
		windBefore += vec3(sin(pc.sundir.x * 3.1 + px), sin(pc.sundir.x * 2.7 + pz), 0.0).xzy * flutterScale - flutter;
		pos += flutter;
	}

	outPosition = pos;
	outPrevPosition = pos + windBefore;
	outNormal = normal;
	outUV = inUVAO.xy;
	outMaterial = int(inNormalMaterial.w + 0.5);
	outAO = inUVAO.z;
	outDryness = inst.params.y;
	outFade = inst.params.w;
	outSeed = fract(inst.params.z);
	outTint = inUVAO.w;

	gl_Position = pc.viewproj * vec4(pos, 1.0);
}
