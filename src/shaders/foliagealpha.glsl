// the alpha test of the grass / wheat / flower cards, shared by foliage.frag, its depth prepass and the shadow pass
// so all of them cut out exactly the same pixels. expects bladeAtlas, flowerAtlas and wheatAtlas
// to be declared
#ifndef FOLIAGE_ALPHA_GLSL
#define FOLIAGE_ALPHA_GLSL

// kind: 0 grass blade, 1 wheat (an oat plant card), 3 flower head (foliage.vert)
bool foliageCovered(int kind, vec2 uv, out vec4 texel) {
	if (kind == 0) {
		// a procedural grass blade: wide at the root, curving in to a sharp tip (uv.y 0 root .. 1 tip), plus a
		// soft midrib line the fragment shader darkens. the far ring's wider cards get a few blades side by side
		texel = vec4(0.0);
		float t = uv.y;
		float halfWidth = 0.5 * pow(max(1.0 - t, 0.0), 0.65);
		float x = abs(uv.x - 0.5);
		float edge = halfWidth - x;
		return edge / max(fwidth(x), 1e-4) > -0.5; // the same screen space sharpening as the textured alpha
	}
	texel = kind == 3 ? texture(flowerAtlas, uv) : (kind == 1 ? texture(wheatAtlas, uv) : texture(bladeAtlas, uv));
	// sharpen the alpha by its screen space rate of change, keeps alpha tested blades from thinning out in the
	// mip chain (and keeps the edges crisp up close)
	float alpha = (texel.a - 0.5) / max(fwidth(texel.a), 1e-4) + 0.5;
	return alpha >= 0.5;
}

#endif
