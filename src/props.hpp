#ifndef PROPS_HPP
#define PROPS_HPP

#include <vk_types.h>
#include "foliage.hpp" // FoliagePushConstants, the prop shaders use the same layout

class Application;

// rocks, boulders and fallen logs: a few procedural meshes (two LODs each), scattered on a world anchored grid around
// the camera every frame by props.comp, drawn per mesh / LOD with indexed indirect draws into the G-buffer,
// plus shadow casters near the camera
constexpr uint32_t PROP_TEXTURES = 7; // rock albedo + normal, bark albedo + normal, moss, end grain albedo + normal

class Props {
public:
	// textures: rock albedo, rock normal, bark albedo, bark normal, moss albedo (owned by the application)
	// textures: PROP_TEXTURES views (props.frag). endGrainTexture: [5] / [6] are the real cross section, not stand ins
	void init(Application* engine, const AllocatedImage& heightmap, const AllocatedImage& climateMap, VkSampler heightmapSampler,
			  const VkImageView* textures, bool endGrainTexture, VkSampler textureSampler, VkFormat shadowFormat);
	void destroy();

	void update(VkCommandBuffer cmd, const FoliagePushConstants& pc); // outside rendering, before the shadow pass
	void draw(VkCommandBuffer cmd);									  // inside the G-buffer pass
	void drawShadow(VkCommandBuffer cmd, const glm::mat4& lightViewProj);

	bool m_Enabled = true;

private:
	// init's writes of the heightmap + path map into the sets that sample them
	void setHeightmap(const AllocatedImage& heightmap, const AllocatedImage& climateMap, VkSampler heightmapSampler);
	struct Vertex {
		glm::vec4 positionMaterial; // xyz, w = 0 rock, 1 bark, 2 log end
		glm::vec4 normalAO;
		glm::vec4 uv;
	};
	struct Mesh { uint32_t firstIndex, indexCount; int32_t vertexOffset; };

	// must match propscommon.glsl
	static constexpr uint32_t MESHES = 8, BINS = MESHES * 3, CAPACITY = 4096; // 4 rocks, 2 logs, 2 stumps
	static constexpr float CELL = 0.9f, RADIUS = 160.0f;
	static constexpr uint32_t GRID_SIDE = static_cast<uint32_t>(2.0f * RADIUS / CELL);

	void buildRock(uint32_t subdivisions, float seed, std::vector<Vertex>& vertices, std::vector<uint32_t>& indices);
	void buildLog(uint32_t segments, uint32_t rings, float seed, std::vector<Vertex>& vertices, std::vector<uint32_t>& indices);
	void buildStump(uint32_t segments, bool broken, float seed, std::vector<Vertex>& vertices, std::vector<uint32_t>& indices);

	VkDevice m_Device;
	VmaAllocator m_Allocator;
	Mesh m_Meshes[MESHES][2];
	AllocatedBuffer m_VertexBuffer, m_IndexBuffer, m_Instances, m_Commands;
	std::vector<VkDrawIndexedIndirectCommand> m_CommandTemplate;

	VkDescriptorSetLayout m_ComputeLayout, m_RenderLayout;
	VkDescriptorSet m_ComputeSet, m_RenderSet;
	ComputeEffect m_Scatter; // pipeline destroyed by the application
	VkPipelineLayout m_RenderPipelineLayout;
	VkPipeline m_RenderPipeline, m_ShadowPipeline;
	FoliagePushConstants m_PushConstants{};
};

#endif
