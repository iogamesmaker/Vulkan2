#ifndef WILDLIFE_HPP
#define WILDLIFE_HPP

#include <vk_types.h>

class Application;

// life in the sky and over the meadows: flocks of small birds, crows, geese in a V and soaring buzzards by day, bats at
// dusk, butterflies over the flowers on sunny days. all procedural: every flock / butterfly belongs to a world anchored
// cell around the camera and flies a path that's a pure function of time, so there's nothing to simulate or keep.
// a compute pass (wildlife.comp) poses each one for the frame, one instanced draw (wildlife.vert) builds the shapes
// into the G-buffer, lit like everything else

// must match wildlife.vert / .frag
struct WildlifePushConstants {
	glm::mat4 viewproj;
	glm::vec3 campos; // terrain space
	float time;
	glm::vec2 offsetWorld; // world units between terrain space and the continuous world
	glm::ivec2 offset;	   // heightmap texel offset
	float terrainSize;
	float factor;		 // terrain height scale
	float unitsPerMeter; // 1 / Camera::metersPerUnit
	float projScale;	 // pixels per unit of size at distance 1
	float previousTime;	 // last frame's time (the motion vectors)
	float sunHeight;	 // the sun direction's y: day, dusk, night
	float calm;			 // 1 = fine weather, 0 = storm / heavy rain (everything takes cover)
	float pad;
};
static_assert(sizeof(WildlifePushConstants) == 128, "push constants are only guaranteed to have 128 bytes");

class Wildlife {
public:
	void init(Application* engine, const AllocatedImage& heightmap, const AllocatedImage& climateMap, VkSampler heightmapSampler);
	void destroy();
	// outside rendering, before the G-buffer pass
	void update(VkCommandBuffer cmd, const WildlifePushConstants& pc);
	// inside the G-buffer pass
	void draw(VkCommandBuffer cmd);

	bool m_Enabled = true;

	// must match wildlife.vert
	static constexpr uint32_t FLOCK_GRID = 9, BIRDS_PER_FLOCK = 24, BIRD_VERTICES = 39;
	static constexpr uint32_t BUTTERFLY_GRID = 14; // (drawn with BIRD_VERTICES too: the same cells hold bats at dusk)
	static constexpr uint32_t BIRDS = FLOCK_GRID * FLOCK_GRID * BIRDS_PER_FLOCK, BUTTERFLIES = BUTTERFLY_GRID * BUTTERFLY_GRID;

private:
	// must match wildlifecommon.glsl's Critter
	static constexpr uint32_t CRITTER_BYTES = 7 * 16;
	VkDevice m_Device = VK_NULL_HANDLE;
	VmaAllocator m_Allocator = VK_NULL_HANDLE;
	AllocatedBuffer m_Critters{};
	ComputeEffect m_Pose{}; // pipeline destroyed by the application's deletion queue
	WildlifePushConstants m_PushConstants{};
	VkDescriptorSetLayout m_Layout = VK_NULL_HANDLE;
	VkDescriptorSet m_Set = VK_NULL_HANDLE;
	VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
	VkPipeline m_Pipeline = VK_NULL_HANDLE;
};

#endif
