#version 450
// tessellation evaluation

layout (quads, equal_spacing, cw) in;

layout(location = 0) in vec3 inPosition[];
layout(location = 1) in vec2 inUV[];
layout(location = 5) flat in int inLOD[];

layout(location = 0) out vec3 outPosition;
layout(location = 1) out vec2 outUV;
layout(location = 5) flat out int outLOD;

layout(set = 0, binding = 0) uniform sampler2D heightmap;

layout(push_constant) uniform constants {
	mat4 proj;
	mat4 view;
	ivec2 offset;
	ivec2 screen;
	float tessellationFactor;
	float factor;
} pc;

void main() {
	float u = gl_TessCoord.x;
	float v = gl_TessCoord.y;
	
	vec4 p0 = mix(gl_in[0].gl_Position, gl_in[1].gl_Position, u);
	vec4 p1 = mix(gl_in[2].gl_Position, gl_in[3].gl_Position, u);
	vec4 pos = mix(p0, p1, v);
	
	vec2 uv0 = mix(inUV[0], inUV[1], u);
	vec2 uv1 = mix(inUV[2], inUV[3], u);
	outUV = mix(uv0, uv1, v);
	
	outLOD = int(round(mix(mix(inLOD[0],inLOD[1],u),mix(inLOD[2], inLOD[3],u), v)));
	
	vec2 readCoord = outUV + (pc.offset * (1.0 / textureSize(heightmap, 0)));

	pos.y += texture(heightmap, readCoord).r * pc.factor;
	pos.y -= pc.factor * 0.5;
	
	outPosition = pos.xyz;
	//outPatchCoord = vec2(u,v);
	
	gl_Position = pc.proj * pc.view * pos;
}