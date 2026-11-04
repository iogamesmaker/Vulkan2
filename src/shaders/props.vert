#version 450
// rocks, boulders and logs (Props::buildMeshes), instanced from props.comp's bins

#include "propscommon.glsl"

layout(location = 0) in vec4 inPositionMaterial; // xyz mesh space (~unit size), w = 0 rock, 1 bark, 2 log end
layout(location = 1) in vec4 inNormalAO;		 // xyz normal, w = ambient occlusion baked into the mesh
layout(location = 2) in vec4 inUV;				 // xy uv (logs)

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
	// tilt about the mesh's z axis first (logs run along x, so they follow the slope), then the yaw
	float ct = cos(inst.params.y), st = sin(inst.params.y);
	float cy = cos(inst.params.x), sy = sin(inst.params.x);
	mat3 tilt = mat3(ct, st, 0.0, -st, ct, 0.0, 0.0, 0.0, 1.0);
	mat3 yaw = mat3(cy, 0.0, -sy, 0.0, 1.0, 0.0, sy, 0.0, cy);
	mat3 rotation = yaw * tilt;

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
