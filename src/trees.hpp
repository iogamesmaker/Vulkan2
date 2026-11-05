#ifndef TREES_HPP
#define TREES_HPP

#include <vk_types.h>
#include "foliage.hpp"
#include <functional>
#include <vk_pipelines.h> // FoliagePushConstants, the tree shaders use the same layout

class Application;

constexpr uint32_t TREE_TEXTURES = 10; // must match treeTextures[] in the tree shaders

// procedural conifers and broadleaf trees (young ones too, smaller). the meshes are generated once at startup (a few variants per
// species, each with a cheaper LOD), a compute pass scatters them per biome around the camera every frame into
// per mesh bins, and every bin is an indexed indirect draw: LOD0, LOD1, then shadow casters near the camera (one bin
// per mesh each), the impostors have their own draw. bushes are part of the ground cover (groundcover.hpp)

struct TreeVertex {
	glm::vec4 positionWind;	  // xyz in tree space (height 1), w = how much it sways (0 at the base)
	glm::vec4 normalMaterial; // xyz normal, w = material: 0 conifer bark, 1 broadleaf bark, 2 conifer branch, 3 broadleaf cluster
	glm::vec4 uvAO;			  // xy uv, z = ambient occlusion, w = the tree type's colouring (treecommon.glsl treeTint)
};

class Trees {
public:
	// TREE_TEXTURES textures: bark conifer, bark broadleaf, conifer branch card, broadleaf cluster card, the barks'
	// normal maps, roughness maps and height maps (owned by the application)
	void init(Application* engine, const AllocatedImage& heightmap, const AllocatedImage& climateMap, VkSampler heightmapSampler,
			  const AllocatedImage* textures, VkSampler textureSampler,
			  VkDescriptorSetLayout atmosphereLayout, VkDescriptorSetLayout shadowLayout, VkFormat shadowFormat, VkBuffer interactionBuffer);
	void destroy();

	// scatter for this frame, outside of rendering and before the shadow pass
	void update(VkCommandBuffer cmd, const FoliagePushConstants& pc);
	// split (optional) is called after the depth prepass and after the full trees, for the GPU timers
	void draw(VkCommandBuffer cmd, VkDescriptorSet atmosphereSet, VkDescriptorSet shadowSet, const std::function<void()>& split = {});
	void drawShadow(VkCommandBuffer cmd, const glm::mat4& lightViewProj);

	void drawImGui();
	// occlusion culling against last frame's depth pyramid (hiz.hpp)
	struct OcclusionData { glm::mat4 viewProj; glm::vec4 size; }; // size: pyramid mip 0, mips, w = on
	void setOcclusionSource(VkImageView pyramid, VkSampler sampler);
	OcclusionData m_Occlusion{};


	bool m_Enabled = true;
	float m_Density = 1.0f;
private:
	// init's writes of the heightmap + climate map into the sets that sample them
	void setHeightmap(const AllocatedImage& heightmap, const AllocatedImage& climateMap, VkSampler heightmapSampler);
	struct Mesh { uint32_t firstIndex, indexCount; };
	
	static constexpr uint32_t SPECIES = 2, VARIANTS = 3, MESHES = SPECIES * VARIANTS; // treecommon.glsl TREE_MESHES
	static constexpr uint32_t BINS = MESHES * 3; // indexed draws: LOD0, LOD1, shadow (impostors have their own commands)
	// must match treecommon.glsl
	static constexpr uint32_t MAX_PLACED = 1048576; // more than the placement circle holds (~994k cells), can't overflow
	// per mesh (sized for wall to wall conifers seen from above), impostors are one pool for all meshes
	static constexpr uint32_t CAPACITY_LOD0 = 4096, CAPACITY_LOD1 = 32768, CAPACITY_SHADOW = 16384, CAPACITY_IMPOSTOR = MAX_PLACED;
	static constexpr uint32_t MAX_INSTANCES = MESHES * (CAPACITY_LOD0 + CAPACITY_LOD1 + CAPACITY_SHADOW) + CAPACITY_IMPOSTOR;
	static constexpr uint32_t PLACE_GRID_SIDE = 1125;
	static constexpr float PLACE_STEP = 25.0f; // world units the camera moves before trees get placed again
	static constexpr uint32_t IMPOSTOR_CELL = 192;
	static constexpr uint32_t IMPOSTOR_ELEVATIONS = 3;		// rows in the atlas, must match impostor.vert
	static constexpr float IMPOSTOR_ELEVATION_STEP = 35.0f; // degrees
	
	void buildMeshes(std::vector<TreeVertex>& vertices, std::vector<uint32_t>& indices);
	void bakeImpostors(Application* engine, const std::function<PipelineBuilder(VkPipelineLayout, VkShaderModule)>& meshPipeline, VkShaderModule bakeShader);
	static uint32_t binOffset(uint32_t bin);
	
	VkDevice m_Device;
	VmaAllocator m_Allocator;
	
	Mesh m_Meshes[MESHES][2]; // [mesh][lod]
AllocatedBuffer m_VertexBuffer, m_IndexBuffer;
	AllocatedBuffer m_Placed, m_PlacedCount;		   // persistent placement
	AllocatedBuffer m_Instances, m_Commands, m_ImpostorCommands; // per frame culling
	AllocatedBuffer m_OcclusionBuffer;							   // m_Occlusion, updated every frame
	std::vector<VkDrawIndexedIndirectCommand> m_CommandTemplate; // instance counts reset to 0 every frame
	std::vector<VkDrawIndirectCommand> m_ImpostorTemplate;
	glm::vec2 m_LastPlaceCenter{0.0f};
	float m_LastDensity = -1.0f;
	bool m_ForcePlace = true;
	
	AllocatedImage m_ImpostorAlbedo, m_ImpostorNormal; // one column per mesh, one row per elevation
	
	VkDescriptorSetLayout m_PlaceLayout, m_ComputeLayout, m_RenderLayout, m_ImpostorLayout;
	VkDescriptorSet m_PlaceSet, m_ComputeSet, m_RenderSet, m_ImpostorSet;
	ComputeEffect m_Place, m_Scatter; // pipelines destroyed by the application's deletion queue
	VkPipelineLayout m_RenderPipelineLayout, m_ShadowPipelineLayout, m_ImpostorPipelineLayout;
	VkPipeline m_RenderPipeline, m_PrepassPipeline, m_ShadowPipeline, m_ImpostorPipeline;
	
	FoliagePushConstants m_PushConstants{};
};

#endif
