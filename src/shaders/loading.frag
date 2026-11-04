#version 450
// the startup screen (loading.cpp): a spinning arc on dark grey while the world loads. straight into the swapchain
// (UNORM), so these are the displayed values

layout(location = 0) in vec2 texcoords;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform constants {
	vec2 size;	// pixels
	float time; // seconds
} pc;

const float TAU = 6.2831853;

void main() {
	vec2 p = (texcoords - 0.5) * pc.size; // pixels from the centre
	float radius = min(pc.size.x, pc.size.y) * 0.04;
	float width = max(radius * 0.22, 2.0);

	// the ring, antialiased over a pixel
	float ring = 1.0 - smoothstep(width * 0.5 - 0.75, width * 0.5 + 0.75, abs(length(p) - radius));
	// three quarters of it lit: bright at the head, fading towards the tail
	float head = pc.time * 5.0;
	float behind = fract((head - atan(p.y, p.x)) / TAU); // 0 at the head, growing backwards round the ring
	float arc = (1.0 - smoothstep(0.0, 0.75, behind)) * smoothstep(1.0, 0.985, behind); // (the head's edge antialiased)

	vec3 background = vec3(0.12);
	outColor = vec4(mix(background, vec3(0.85), ring * arc), 1.0);
}
