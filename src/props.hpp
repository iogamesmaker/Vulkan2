#ifndef PROPS_HPP
#define PROPS_HPP

#include <vk_types.h>
#include "foliage.hpp" // FoliagePushConstants, the prop shaders use the same layout

class Application;

class TextureLoader;

// rocks, boulders, fallen logs and stumps: photoscans (Poly Haven, assets/props), two LODs each from meshoptimizer,
// scattered on a world anchored grid around the camera every frame by props.comp, drawn per mesh / LOD with indexed
// indirect draws into the G-buffer, plus shadow casters near the camera
constexpr uint32_t PROP_MESH_COUNT = 10; // 6 rocks, 2 logs, 2 stumps (propscommon.glsl PROP_MESHES)
// per mesh albedo + normal, the moss, then the two ambientCG stones (albedo + normal each) the set rocks are textured
// with (props.frag)
constexpr uint32_t PROP_TEXTURES = PROP_MESH_COUNT * 2 + 1 + 4;

class Props {
public:
	// loads the scans and their textures. mossTexture: grown on the wet tops (the terrain's grass, owned by the application)
	void init(Application* engine, TextureLoader& textures, const AllocatedImage& heightmap, const AllocatedImage& climateMap, VkSampler heightmapSampler,
			  VkImageView mossTexture, VkSampler textureSampler, VkFormat shadowFormat);
	void destroy();

	void update(VkCommandBuffer cmd, const FoliagePushConstants& pc); // outside rendering, before the shadow pass
	void draw(VkCommandBuffer cmd);									  // inside the G-buffer pass
	void drawShadow(VkCommandBuffer cmd, const glm::mat4& lightViewProj);
	// the "GPU draw counts" window (drawstats.hpp)
	std::vector<DrawStatBuffer> drawStatBuffers() const { return { { m_Commands.buffer, BINS * sizeof(VkDrawIndexedIndirectCommand) } }; }
	void drawStats(const std::vector<const uint8_t*>& data, std::vector<DrawStat>& out) const {
		out.push_back(sumIndexed("rocks, logs, stumps", data[0], 0, MESHES * 2));
		out.push_back(sumIndexed("rocks, logs, stumps, shadow casters", data[0], MESHES * 2, MESHES));
	}

	bool m_Enabled = true;

private:
	// init's writes of the heightmap + climate map into the sets that sample them
	void setHeightmap(const AllocatedImage& heightmap, const AllocatedImage& climateMap, VkSampler heightmapSampler);
	struct Vertex {
		glm::vec4 positionMaterial; // xyz, w = the mesh (its textures, rock / wood)
		glm::vec4 normalAO;
		glm::vec4 uv;
	};
	struct Mesh { uint32_t firstIndex, indexCount; int32_t vertexOffset; };

	// must match propscommon.glsl
	static constexpr uint32_t MESHES = PROP_MESH_COUNT, BINS = MESHES * 3, CAPACITY = 4096;
	static constexpr float CELL = 0.9f, RADIUS = 160.0f;
	static constexpr uint32_t GRID_SIDE = static_cast<uint32_t>(2.0f * RADIUS / CELL);

	bool loadMesh(uint32_t mesh, std::vector<Vertex>& vertices, std::vector<uint32_t>& indices);

	VkDevice m_Device;
	VmaAllocator m_Allocator;
	TextureLoader* m_TextureLoader = nullptr;
	AllocatedImage m_Textures[MESHES * 2]{}; // per mesh albedo, normal
	AllocatedImage m_Stone[4]{};			  // ambientCG Rock030, Rock062: albedo, normal each
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
