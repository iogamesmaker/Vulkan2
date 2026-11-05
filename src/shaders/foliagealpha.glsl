// the alpha test of the grass / wheat / flower cards, shared by foliage.frag, its depth prepass and the shadow pass
// so all of them cut out exactly the same pixels. expects bladeAtlas, flowerAtlas and wheatAtlas
// to be declared
#ifndef FOLIAGE_ALPHA_GLSL
#define FOLIAGE_ALPHA_GLSL

// kind: 0 grass blade, 1 wheat (an oat plant card), 3 flower head (foliage.vert)
// grass blades are the photographed ones in bladeAtlas (ambientCG Foliage001), each card one blade's rect
bool foliageCovered(int kind, vec2 uv, out vec4 texel) {
	texel = kind == 3 ? texture(flowerAtlas, uv) : (kind == 1 ? texture(wheatAtlas, uv) : texture(bladeAtlas, uv));
	// sharpen the alpha by its screen space rate of change, keeps alpha tested blades from thinning out in the
	// mip chain (and keeps the edges crisp up close)
	float alpha = (texel.a - 0.5) / max(fwidth(texel.a), 1e-4) + 0.5;
	return alpha >= 0.5;
}

#endif
