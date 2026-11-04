#version 450
// terrain depth prepass: only the edge discards of terrain.frag, so the full material shader afterwards (depth
// EQUAL) runs once per visible pixel instead of on every hill drawn behind another one

layout(location = 0) in vec3 fragPos;
layout(location = 1) in vec2 fragUV;

layout(push_constant) uniform constants {
	mat4 viewproj;
	vec3 campos;
	float tessellationFactor;
	vec3 sundir;
	float factor;
	ivec2 offset;
	float projScale;
	float worldRadius;
} pc;

void main() {
	// must match terrain.frag
	if(fragUV.x > 0.984 || fragUV.y > 0.984 || fragUV.x < 0.016 || fragUV.y < 0.016) discard;
	if(distance(fragPos.xz, pc.campos.xz) > pc.worldRadius) discard;
}
