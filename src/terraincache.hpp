#ifndef TERRAINCACHE_HPP
#define TERRAINCACHE_HPP

#include <vk_types.h>

class Application;

// far field terrain cache: the terrain's finished surface (albedo + normal, terrainmaterial.glsl) baked into a
// clipmap around the camera, LEVELS squares of SIZE texels, each level's texels twice the size of the one before.
// past START_DISTANCE terrain.frag reads it (one or two filtered lookups) instead of sampling every material,
// projection and texture scale per pixel. the levels are toroidal (a world texel always sits at its coordinates
// modulo SIZE), so as the camera moves only the strips that come into a square are baked (terraincache.comp)
class TerrainCache {
public:
	static constexpr uint32_t LEVELS = 5;		   // terrain.frag's TERRAIN_CACHE_LEVELS
	static constexpr uint32_t SIZE = 2048;		   // texels per side
	static constexpr float START_DISTANCE = 55.0f; // world units: where the direct shading's far texture scale is complete
	static constexpr int SNAP = 32;				   // a level re-centres in steps of this many texels

	// terrainLayout / atmosphereLayout: the bake reads the terrain's textures and the atmosphere UBO through
	// their sets. writes its info buffer and images into the terrain set (bindings 6, 7, 8)
	void init(Application* engine, VkDescriptorSet terrainSet, VkDescriptorSetLayout terrainLayout, VkDescriptorSetLayout atmosphereLayout);
	// bakes what came into the squares around the camera (at most one whole level a frame, terrain.frag shades
	// the rest directly meanwhile), then updates the info terrain.frag reads. outside of a render pass
	bool update(VkCommandBuffer cmd, glm::dvec2 cameraWorld, float factor, float macroWeight, VkDescriptorSet terrainSet, VkDescriptorSet atmosphereSet);
	// while the cache is switched off: tells the readers (the grass, foliage.comp) there's nothing baked, every frame
	void publishOff(VkCommandBuffer cmd);
	void invalidate(); // the terrain or its look changed: rebake everything
	void destroy();

	// the baked levels (2D arrays, LEVELS layers, SHADER_READ_ONLY outside of update), for the debug texture viewer
	VkImageView getAlbedoView() const { return m_AlbedoSampled; }
	VkImageView getNormalView() const { return m_NormalSampled; }
	uint32_t getMips() const { return m_Mips; }
	// for other readers (foliage.comp takes the grass colour from the baked ground): terrain.frag's TerrainCacheInfo
	// and the sampler the levels are read with (wraps)
	VkBuffer getInfoBuffer() const { return m_InfoBuffer.buffer; }
	VkSampler getSampler() const { return m_Sampler; }


private:
	struct Level {
		glm::ivec2 min{ 0 }; // the baked square's first texel, world texel coordinates
		bool valid = false;
	};
	struct BakePushConstants {
		glm::ivec2 bakeMin;
		glm::ivec2 keepMin;
		int level;
		int keep;
		float texel;
		float factor;
		float macroWeight;
		float pad[3];
	};
	struct Info { // terrain.frag's TerrainCacheInfo
		glm::vec4 levels[LEVELS];
		glm::vec4 params;
	};

	static float texelSize(uint32_t level);
	void uploadInfo(VkCommandBuffer cmd, bool on);
	void bake(VkCommandBuffer cmd, uint32_t level, glm::ivec2 min, const Level* keep, float factor, float macroWeight);

	VkDevice m_Device;
	VmaAllocator m_Allocator;
	Application* m_Engine;
	uint32_t m_Mips = 1;
	AllocatedImage m_Albedo{}, m_Normal{};
	VkImageView m_AlbedoSampled, m_NormalSampled;				  // every level, every mip (terrain.frag)
	VkImageView m_AlbedoStorage[LEVELS], m_NormalStorage[LEVELS]; // one level's top mip each (the bake)
	VkSampler m_Sampler;
	AllocatedBuffer m_InfoBuffer;
	Level m_Levels[LEVELS];
	float m_MacroWeight = -1.0f; // what the levels were baked with

	VkDescriptorSetLayout m_StorageLayout;
	VkDescriptorSet m_StorageSets[LEVELS]; // per level: a bake's storage images may only cover what's in GENERAL
	ComputeEffect m_Effect;
};

#endif
