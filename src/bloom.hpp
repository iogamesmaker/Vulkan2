#ifndef BLOOM_HPP
#define BLOOM_HPP

#include <vk_types.h>

class Application;

// physically based bloom (bloom.comp): the HDR draw image is downsampled into a chain of MIPS levels starting at
// half resolution, then upsampled back up, each level added onto the next bigger one. post.comp adds mip 0
class Bloom {
public:
	void init(Application* engine);
	// (re)creates the chain, after every resize. its first mip is half the WINDOW's size (draw image / render scale),
	// so the glow covers the same part of the screen whatever the render scale (it got wider / narrower with it)
	void setTargets(const AllocatedImage& drawImage, float renderScale = 1.0f);
	void render(VkCommandBuffer cmd);				  // draw image in GENERAL, leaves the chain in GENERAL
	void destroy();

	VkImageView getView() const { return m_ChainView; }
	VkSampler getSampler() const { return m_Sampler; }

	static constexpr uint32_t MIPS = 6;
private:
	void destroyChain();

	VkDevice m_Device;
	VmaAllocator m_Allocator;
	AllocatedImage m_Chain{};
	VkImageView m_ChainView = VK_NULL_HANDLE; // all mips, sampled
	VkImageView m_MipViews[MIPS] = {};		  // one mip each, written
	VkExtent2D m_MipSizes[MIPS];

	VkSampler m_Sampler;
	VkDescriptorSetLayout m_Layout;
	VkDescriptorSet m_Sets[2 * MIPS - 1]; // MIPS downsamples, then MIPS - 1 upsamples
	ComputeEffect m_Effect;
};

#endif
