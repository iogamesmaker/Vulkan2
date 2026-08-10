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

float tessFactor(vec4 p0, vec4 p1) { // thank you sacha willems my goat
	vec4 middle = 0.5 * (p0 + p1);
	float radius = distance(p0, p1) / 2.0;
	
	// Extract camera position and flatten the forward vector to the horizon
	mat4 invView = inverse(pc.view);
	vec3 camPos = invView[3].xyz;
	vec3 f = normalize(vec3(pc.view[0][2], 0.0, pc.view[2][2]));
	vec3 r = normalize(cross(vec3(0.0, 1.0, 0.0), f));
	vec3 u = cross(f, r);
	
	mat4 horizonView = mat4(
		vec4(r.x, u.x, f.x, 0.0),
		vec4(r.y, u.y, f.y, 0.0),
		vec4(r.z, u.z, f.z, 0.0),
		vec4(-dot(r, camPos), -dot(u, camPos), -dot(f, camPos), 1.0)
	);

	vec4 v0 = horizonView * middle;
	
	vec4 clip0 = (pc.proj * (v0 - vec4(radius, vec3(0.0))));
	vec4 clip1 = (pc.proj * (v0 + vec4(radius, vec3(0.0))));
	
	clip0 /= clip0.w;
	clip1 /= clip1.w;
	
	clip0.xy *= pc.screen;
	clip1.xy *= pc.screen;
	
	return clamp(distance(clip0, clip1) / 20.f * pc.tessellationFactor, 1.0, 64.0);
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