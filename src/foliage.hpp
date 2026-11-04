#ifndef FOLIAGE_HPP
#define FOLIAGE_HPP

#include <vk_types.h>

class Application;

// grass + wheat. a compute pass scatters clumps around the camera every frame (world anchored, culled by
// biome / slope / frustum) into an instance buffer, then one indirect draw builds the blade cards in the vertex
// shader. no vertex buffers, no CPU work per clump

// must match the push constants in foliagecommon.glsl
struct FoliagePushConstants {
	glm::mat4 viewproj;
	glm::vec3 campos;
	float time;
	glm::vec3 sundir; // trees_bake: direction to the viewer. otherwise x = last frame's time (wind motion vectors)
	float factor;
	glm::vec2 offsetWorld;
	glm::ivec2 offset;
	float terrainSize;
	float projScale;  // terrain LOD (see terrainheight.glsl), so things sit on the surface the terrain mesh actually has
	float lodPixels;  // 40 / tessellation factor
	float extra;	  // trees: density multiplier, foliage: how strongly grass bends away from the player (0 = not walking)
};
static_assert(sizeof(FoliagePushConstants) == 128, "push constants are only guaranteed to have 128 bytes");

class Foliage {
public:
	// atmosphere / shadow layouts are sets 1 and 2 of the graphics pipeline, same as the terrain
	// the blade atlas is owned (loaded / destroyed) by the application
	void init(Application* engine, const AllocatedImage& heightmap, const AllocatedImage& climateMap, VkSampler heightmapSampler, VkFormat shadowFormat,
			  const AllocatedImage& bladeAtlas, const AllocatedImage& flowerAtlas, const AllocatedImage& wheatAtlas,
			  const AllocatedImage& undergrowthAtlas, VkSampler atlasSampler,
			  VkDescriptorSetLayout atmosphereLayout, VkDescriptorSetLayout shadowLayout, VkBuffer interactionBuffer);
	void destroy();

	// scatter the clumps for this frame, call outside of rendering (before it)
	void update(VkCommandBuffer cmd, const FoliagePushConstants& pc);
	// inside the main rendering, after the terrain
	void draw(VkCommandBuffer cmd, VkDescriptorSet atmosphereSet, VkDescriptorSet shadowSet);
	// grass near the camera into a shadow cascade
	void drawShadow(VkCommandBuffer cmd, const glm::mat4& lightViewProj);

	void drawImGui();

	bool m_Enabled = true;
	uint32_t m_Rings = 4;	   // how far the grass reaches: 1 near (~27m), 2 + mid (~110m), 3 + far (~215m), 4 + wheat fields (~420m)
	bool m_CastShadows = true;
private:
	// init's writes of the heightmap + path map into the sets that sample them
	void setHeightmap(const AllocatedImage& heightmap, const AllocatedImage& climateMap, VkSampler heightmapSampler);
	VkDevice m_Device;
	VmaAllocator m_Allocator;

	static constexpr uint32_t RING_SIDE_NEAR = 140, RING_SIDE_MID = 200, RING_SIDE_FAR = 200, RING_SIDE_WHEAT = 200; // must match foliage.comp
	static constexpr uint32_t MAX_INSTANCES = RING_SIDE_NEAR * RING_SIDE_NEAR + RING_SIDE_MID * RING_SIDE_MID + RING_SIDE_FAR * RING_SIDE_FAR
										  + RING_SIDE_WHEAT * RING_SIDE_WHEAT; // foliagecommon.glsl
	static constexpr uint32_t MAX_SHADOW_INSTANCES = 24000; // must match foliagecommon.glsl
	static constexpr uint32_t MAX_NEAR_INSTANCES = RING_SIDE_NEAR * RING_SIDE_NEAR; // foliagecommon.glsl
	static constexpr uint32_t VERTICES_PER_INSTANCE = 10 * 6;				// BLADES_PER_CLUMP * 6, one card per blade
	static constexpr uint32_t SEGMENTED_VERTICES_PER_INSTANCE = 10 * 3 * 6; // * BLADE_SEGMENTS: the near ring, shadow casters

	AllocatedBuffer m_Instances;
	AllocatedBuffer m_Indirect;

	VkDescriptorSetLayout m_ComputeLayout;
	VkDescriptorSet m_ComputeSet;
	VkDescriptorSetLayout m_RenderLayout;
	VkDescriptorSet m_RenderSet;

	ComputeEffect m_Scatter; // pipeline destroyed by the application's deletion queue
	VkPipelineLayout m_RenderPipelineLayout;
	VkPipeline m_RenderPipeline;
	VkPipelineLayout m_ShadowPipelineLayout;
	VkPipeline m_ShadowPipeline;
	VkPipeline m_PrepassPipeline;

	FoliagePushConstants m_PushConstants{};
};

#endif
