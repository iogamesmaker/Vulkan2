// shared between foliage.comp / foliage.vert / foliage.frag
#ifndef FOLIAGE_COMMON_GLSL
#define FOLIAGE_COMMON_GLSL

#define FOLIAGE_GRASS	0
#define FOLIAGE_WHEAT	1
#define FOLIAGE_FLOWERS 2 // a few grass blades as stems with daisies on top
// (3 was the undergrowth, leaf cards on forest floors: the ground cover has real ferns and bushes now)

#define BLADES_PER_CLUMP 10 // BLADE_SEGMENTS quads each, the draw is BLADES_PER_CLUMP * BLADE_SEGMENTS * 6 vertices per instance
#define BLADE_SEGMENTS 3   // the near ring's grass blades bend through these, everything else is one card (the rest clipped)

struct FoliageInstance {
	vec4 posScale;	// xyz = position in terrain space (the same space as the terrain's fragPos), w = size
	vec4 params;	// x = yaw, y = type + 8 * ring (0 near, 1 far), z = random seed, w = moisture (dry grass is yellower)
};

layout(push_constant) uniform constants {
	mat4 viewproj;
	vec3 campos;	   // terrain space
	float time;
	vec3 sundir;	   // trees_bake: direction to the viewer. otherwise x = last frame's time (wind motion vectors)
	float factor;	   // terrain height scale
	vec2 offsetWorld;  // world units between terrain space and the continuous world (for stable placement)
	ivec2 offset;	   // heightmap texel offset (same as the terrain's)
	float terrainSize; // world units covered by the heightmap
	float projScale;   // terrain LOD, see terrainheight.glsl
	float lodPixels;
	float interaction; // how strongly blades bend away from the player, 0 when not walking
} pc;

#define MAX_FOLIAGE_INSTANCES 139600u		 // Foliage::MAX_INSTANCES
#define MAX_FOLIAGE_SHADOW_INSTANCES 24000u // Foliage::MAX_SHADOW_INSTANCES
#define MAX_FOLIAGE_NEAR_INSTANCES 19600u	 // Foliage::MAX_NEAR_INSTANCES, the near ring (140^2 cells)
// the instance buffer: [what the camera sees, outer rings] [shadow casters] [what the camera sees, near ring]. the last
// two are drawn with BLADE_SEGMENTS per blade (curved blades up close), the first with one card per blade: the extra
// segments of every far clump went through the vertex shader only to be clipped, it doubled the cost of the grass
#define FOLIAGE_SEGMENTED_FROM MAX_FOLIAGE_INSTANCES

float hash11(float p) {
	p = fract(p * 0.1031);
	p *= p + 33.33;
	p *= p + p;
	return fract(p);
}

vec4 hash42(vec2 p) {
	vec4 p4 = fract(vec4(p.xyxy) * vec4(0.1031, 0.1030, 0.0973, 0.1099));
	p4 += dot(p4, p4.wzxy + 33.33);
	return fract((p4.xxyz + p4.yzzw) * p4.zywx);
}

// terrain space position -> heightmap uv, matches the terrain patches (-size/2 .. size/2)
vec2 heightmapUV(vec2 xz, vec2 heightmapSize) {
	return (xz + 0.5 * pc.terrainSize) / pc.terrainSize + vec2(pc.offset) / heightmapSize;
}

#endif
