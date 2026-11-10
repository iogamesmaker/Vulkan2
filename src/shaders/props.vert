#version 450
// rocks, boulders, logs and stumps (the photoscans, Props::loadMesh), instanced from props.comp's bins

#include "propscommon.glsl"
#include "proprotation.glsl"

layout(location = 0) in vec4 inPositionMaterial; // xyz mesh space (~unit size), w = the mesh (its textures)
layout(location = 1) in vec4 inNormalAO;		 // xyz normal, w = ambient occlusion (Props::loadMesh)
layout(location = 2) in vec4 inUV;				 // xy the scan's uv

layout(std430, set = 0, binding = 0) readonly buffer Instances { PropInstance instances[]; };

invariant gl_Position;

layout(location = 0) out vec3 outPosition;
layout(location = 1) out vec3 outNormal;
layout(location = 2) out vec2 outUV;
layout(location = 3) flat out int outMaterial;
layout(location = 4) out float outAO;
layout(location = 5) flat out float outMoss;
layout(location = 6) flat out float outSeed;

void main() {
	PropInstance inst = instances[gl_InstanceIndex];
	// laid on the ground's slope (the packed tilt), then the yaw
	mat3 rotation = propRotation(inst.params.x, inst.params.y);

	vec3 pos = inst.posScale.xyz + rotation * inPositionMaterial.xyz * inst.posScale.w;
	outPosition = pos;
	outNormal = rotation * inNormalAO.xyz;
	outUV = inUV.xy;
	outMaterial = int(inPositionMaterial.w + 0.5);
	outAO = inNormalAO.w;
	outMoss = inst.params.w;
	outSeed = inst.params.z;
	gl_Position = pc.viewproj * vec4(pos, 1.0);
}
