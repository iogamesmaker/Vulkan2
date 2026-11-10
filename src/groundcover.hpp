#ifndef GROUNDCOVER_HPP
#define GROUNDCOVER_HPP

#include <vk_types.h>
#include "foliage.hpp" // FoliagePushConstants, the ground cover shaders use the same layout
#include "textures.hpp"

class Application;

// ferns, bushes, nettles, weeds, clover and dead branches between the grass and the trees: real plant models (Poly
// Haven, CC0, assets/plants) scattered around the camera every frame by groundcover.comp on world anchored grids,
// drawn per mesh with indexed indirect draws into the G-buffer, plus shadow casters near the camera
class GroundCover {
public:
	void init(Application* engine, TextureLoader& textures, const AllocatedImage& heightmap, const AllocatedImage& climateMap,
			  VkSampler heightmapSampler, VkSampler textureSampler, VkFormat shadowFormat, VkBuffer interactionBuffer);
	void destroy();

	void update(VkCommandBuffer cmd, const FoliagePushConstants& pc); // outside rendering, before the shadow pass
	void draw(VkCommandBuffer cmd);									  // inside the G-buffer pass
	void drawShadow(VkCommandBuffer cmd, const glm::mat4& lightViewProj);
	// the "GPU draw counts" window (drawstats.hpp)
	std::vector<DrawStatBuffer> drawStatBuffers() const { return { { m_Commands.buffer, BINS * sizeof(VkDrawIndexedIndirectCommand) } }; }
	void drawStats(const std::vector<const uint8_t*>& data, std::vector<DrawStat>& out) const {
		uint32_t meshes = static_cast<uint32_t>(m_Meshes.size());
		out.push_back(sumIndexed("ground cover", data[0], 0, meshes));
		out.push_back(sumIndexed("ground cover, shadow casters", data[0], MAX_MESHES, meshes));
	}

	bool m_Enabled = true;
	float m_Density = 1.0f;

private:
	struct Vertex {
		glm::vec4 positionSpecies; // xyz world units, w = species
		glm::vec4 normalHeight;	   // w = 0 at the plant's base .. 1 at its top
		glm::vec4 uv;
	};
	struct Mesh { uint32_t firstIndex, indexCount; int32_t vertexOffset; };
	struct Species { glm::uvec4 meshes; glm::vec4 size; }; // groundcovercommon.glsl

	// must match groundcovercommon.glsl
	static constexpr uint32_t SPECIES = 14, MAX_MESHES = 96, BINS = MAX_MESHES * 2, CAPACITY = 4096;
	static constexpr uint32_t SIDE[3] = { 140, 133, 166 }; // the small / medium / far grids, 2 * radius / cell
	static constexpr uint32_t THREADS = SIDE[0] * SIDE[0] + SIDE[1] * SIDE[1] + SIDE[2] * SIDE[2];

	VkDevice m_Device;
	VmaAllocator m_Allocator;
	TextureLoader* m_TextureLoader = nullptr;
	std::vector<Mesh> m_Meshes;
	std::vector<AllocatedImage> m_Textures; // albedo, normal per species
	AllocatedBuffer m_VertexBuffer, m_IndexBuffer, m_Instances, m_Commands, m_SpeciesBuffer;
	std::vector<VkDrawIndexedIndirectCommand> m_CommandTemplate;

	VkDescriptorSetLayout m_ComputeLayout, m_RenderLayout;
	VkDescriptorSet m_ComputeSet, m_RenderSet;
	ComputeEffect m_Scatter;
	VkPipelineLayout m_RenderPipelineLayout;
	VkPipeline m_RenderPipeline, m_ShadowPipeline;
	FoliagePushConstants m_PushConstants{};
};

#endif
