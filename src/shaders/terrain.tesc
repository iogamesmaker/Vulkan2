#version 450
// tessellation control thing

layout(vertices = 4) out;

layout(location = 0) in vec3 inPosition[];
layout(location = 1) in vec2 inUV[];

layout(location = 0) out vec3 outPosition[];
layout(location = 1) out vec2 outUV[];
layout(location = 5) out int outLOD[];

layout(push_constant) uniform constants {
	mat4 proj;
	mat4 view;
	ivec2 offset;
	ivec2 screen;
	float tessellationFactor;
	float factor;
} pc;

float tessFactor(vec4 p0, vec4 p1) { 
	vec4 middle = 0.5 * (p0 + p1);
	float radius = distance(p0, p1) / 1.0;
	
	vec4 viewMiddle = pc.view * middle;

	float dist = length(viewMiddle.xyz);

	float zSign = sign(viewMiddle.z) == 0.0 ? 1.0 : sign(viewMiddle.z);
	vec4 virtualPos = vec4(0.0, 0.0, zSign * dist, 1.0);
	
	vec4 clip0 = pc.proj * (virtualPos - vec4(radius, 0.0, 0.0, 0.0));
	vec4 clip1 = pc.proj * (virtualPos + vec4(radius, 0.0, 0.0, 0.0));
	
	clip0 /= clip0.w;
	clip1 /= clip1.w;
	
	//clip0.xy *= vec2(pc.screen);
	//clip1.xy *= vec2(pc.screen);
	clip0.xy *= vec2(1920, 1080);
	clip1.xy *= vec2(1920, 1080);
	
	float pixelSize = distance(clip0.xy, clip1.xy);
	
	return clamp((pixelSize / 20.0) * pc.tessellationFactor, 1.0, 64.0);
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