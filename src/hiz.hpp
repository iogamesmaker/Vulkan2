#ifndef HIZ_HPP
#define HIZ_HPP

#include <vk_types.h>

class Application;

// depth pyramid (hiz.comp) for occlusion culling: built from the depth buffer once the G-buffer is done, the next
// frame's culling (trees.comp) tests what it's about to draw against it, with the camera this frame had
class HiZ {
public:
	void init(Application* engine);
	void setSource(const AllocatedImage& depthBuffer); // (re)creates the pyramid, after every resize
	void build(VkCommandBuffer cmd);					// depth buffer in SHADER_READ_ONLY, pyramid left in GENERAL
	void destroy();

	VkImageView getView() const { return m_PyramidView; }
	VkSampler getSampler() const { return m_Sampler; }
	VkExtent2D getSize() const { return m_MipSizes[0]; }
	uint32_t getMips() const { return m_Mips; }

private:
	static constexpr uint32_t MAX_MIPS = 13;
	void destroyPyramid();

	VkDevice m_Device;
	VmaAllocator m_Allocator;
	Application* m_Engine;
	AllocatedImage m_Pyramid{};
	VkImageView m_PyramidView = VK_NULL_HANDLE; // all mips, sampled
	VkImageView m_MipViews[MAX_MIPS] = {};		// one mip each, written
	VkExtent2D m_MipSizes[MAX_MIPS] = {};
	VkExtent2D m_DepthSize{};
	uint32_t m_Mips = 0;

	VkSampler m_Sampler;
	VkDescriptorSetLayout m_Layout;
	VkDescriptorSet m_Sets[MAX_MIPS];
	ComputeEffect m_Effect;
};

#endif
