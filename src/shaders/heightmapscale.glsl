// the heightmap is 6144 texels a side (Application::m_HeightmapSize). it used to be 4096, and everything measured in
// heightmap texels (path widths, the ground bumps, the slope measure, the horizon AO's reach) is still given in texels
// of a 4096 map, so it keeps meaning the same distance on the ground
#ifndef HEIGHTMAP_SCALE_GLSL
#define HEIGHTMAP_SCALE_GLSL

#define HEIGHTMAP_REFERENCE_SIZE 4096.0

// the terrain's slope measure: (height difference across two texels) * this, squared and summed. 256 at 4096
float heightmapSlopeScale(float size) {
	return 256.0 * size / HEIGHTMAP_REFERENCE_SIZE;
}

#endif
