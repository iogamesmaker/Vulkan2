// the ground cover's alpha test, shared by groundcover.frag and its shadow pass so both cut out the same pixels.
// textures: per species albedo (a = cutout) then normal map
#ifndef GROUNDCOVER_ALPHA_GLSL
#define GROUNDCOVER_ALPHA_GLSL

layout(set = 0, binding = 1) uniform sampler2D textures[GC_SPECIES * 2];

bool groundCoverCovered(int species, vec2 uv, out vec4 texel) {
	texel = texture(textures[species * 2], uv);
	// sharpened by its screen space rate of change (foliagealpha.glsl), the leaves don't thin out in the mips
	float alpha = (texel.a - 0.5) / max(fwidth(texel.a), 1e-4) + 0.5;
	return alpha >= 0.5;
}

#endif
