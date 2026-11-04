#version 450
// far trees: a camera facing (around the vertical axis) card showing the tree's baked impostor

#include "treecommon.glsl"

layout(std430, set = 0, binding = 0) readonly buffer Instances { TreeInstance instances[]; };

layout(location = 0) out vec3 outPosition;
layout(location = 1) out vec2 outUV;
layout(location = 2) out vec3 outRight;
layout(location = 3) out vec3 outToCamera;
layout(location = 7) out vec3 outUp;
layout(location = 8) out vec2 outUV2;		 // the next elevation's view
layout(location = 9) flat out float outBlend; // between the two
layout(location = 4) flat out float outDryness;
layout(location = 5) flat out float outFade;
layout(location = 6) flat out float outSeed;

const vec2 CORNERS[6] = vec2[](vec2(-0.5, -0.5), vec2(0.5, -0.5), vec2(0.5, 0.5), vec2(-0.5, -0.5), vec2(0.5, 0.5), vec2(-0.5, 0.5));
const float ELEVATIONS = 3.0;		 // Trees::IMPOSTOR_ELEVATIONS
const float ELEVATION_STEP = 0.6109; // 35 degrees

void main() {
	TreeInstance inst = instances[gl_InstanceIndex];
	// all meshes share one impostor pool, the mesh is packed into the fade (packImpostorFade)
	uint mesh;
	float fade;
	unpackImpostorFade(inst.params.w, mesh, fade);
	vec2 corner = CORNERS[gl_VertexIndex];

	// fully camera facing card around the tree's centre, the bake covered -0.6..0.6 of the tree's height
	float size = inst.posScale.w;
	vec3 center = inst.posScale.xyz + vec3(0.0, 0.5 * size, 0.0);
	vec3 toCamera = normalize(pc.campos - center);
	vec3 right = normalize(cross(vec3(0.0, 1.0, 0.0), toCamera + vec3(1e-4, 0.0, 0.0)));
	vec3 up = cross(toCamera, right);
	vec3 pos = center + (right * corner.x + up * corner.y) * 1.2 * size;

	// the two baked elevations closest to the camera's, blended in the fragment shader
	float elevation = clamp(asin(clamp(toCamera.y, -1.0, 1.0)) / ELEVATION_STEP, 0.0, ELEVATIONS - 1.0);
	float row0 = min(floor(elevation), ELEVATIONS - 2.0);
	vec2 local = vec2(corner.x + 0.5, 0.5 - corner.y);
	vec2 atlas = vec2(9.0, ELEVATIONS);
	outUV = (vec2(mesh, row0) + local) / atlas;
	outUV2 = (vec2(mesh, row0 + 1.0) + local) / atlas;
	outBlend = elevation - row0;
	outUp = up;

	outPosition = pos;
	outRight = right;
	outToCamera = toCamera;
	outDryness = inst.params.y;
	outFade = fade;
	outSeed = fract(inst.params.z);
	gl_Position = pc.viewproj * vec4(pos, 1.0);
}
