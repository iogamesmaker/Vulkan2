#version 450
// tessellation control thing

layout(vertices = 4) out;

layout(location = 0) in vec3 inPosition[];
layout(location = 1) in vec2 inUV[];

layout(location = 0) out vec3 outPosition[];
layout(location = 1) out vec2 outUV[];
layout(location = 5) flat out int outLOD[];

layout(push_constant) uniform constants {
	mat4 proj;
	mat4 view;
	ivec2 offset;
	ivec2 screen;
	float tessellationFactor;
	float factor;
} pc;

float tessFactor(vec4 p0, vec4 p1) {
    vec3 camPos = -pc.view[3].xyz * mat3(pc.view);
    float dist = distance(0.5 * (p0.xz + p1.xz), camPos.xz);
    
    float smoothDist = (dist / 20.f);
    
    float factor = (distance(p0.xyz, p1.xyz) / smoothDist) * pc.tessellationFactor;
    
    return clamp(factor, 1.0, 64.0);
}

void main() {
	if(gl_InvocationID == 0) {		
		gl_TessLevelOuter[0] = tessFactor(gl_in[0].gl_Position, gl_in[2].gl_Position);
		gl_TessLevelOuter[1] = tessFactor(gl_in[0].gl_Position, gl_in[1].gl_Position);
		gl_TessLevelOuter[2] = tessFactor(gl_in[1].gl_Position, gl_in[3].gl_Position);
		gl_TessLevelOuter[3] = tessFactor(gl_in[2].gl_Position, gl_in[3].gl_Position);

		gl_TessLevelInner[0] = max(gl_TessLevelOuter[1], gl_TessLevelOuter[3]);
		gl_TessLevelInner[1] = max(gl_TessLevelOuter[0], gl_TessLevelOuter[2]);		
	}
	outLOD[gl_InvocationID] = int(gl_TessLevelOuter[gl_InvocationID]);
	outPosition[gl_InvocationID] = inPosition[gl_InvocationID];
	outUV[gl_InvocationID] = inUV[gl_InvocationID];
	gl_out[gl_InvocationID].gl_Position = gl_in[gl_InvocationID].gl_Position;
}