#version 450

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec2 inUV;

layout(location = 0) out vec3 outPosition;
layout(location = 1) out vec2 outUV;

void main(void) {
	outPosition = inPosition;
	outUV = inUV;
	
	gl_Position = vec4(inPosition, 1.0);
}