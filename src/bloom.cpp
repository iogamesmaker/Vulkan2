#include "bloom.hpp"
#include "application.hpp"

struct BloomPushConstants {
	glm::ivec2 targetSize;
	int mode; // 0 = first downsample (from the draw image), 1 = downsample, 2 = upsample + add
	int srcMip;
};

void Bloom::init(Application* engine) {
	m_Device = engine->m_Device;
	m_Allocator = engine->getAllocator();

	VkSamplerCreateInfo samplerCI = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	samplerCI.magFilter = VK_FILTER_LINEAR;
	samplerCI.minFilter = VK_FILTER_LINEAR;
	samplerCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; // every lookup names its mip
	samplerCI.addressModeU = samplerCI.addressModeV = samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerCI.maxLod = static_cast<float>(MIPS);
	VK_CHECK(vkCreateSampler(m_Device, &samplerCI, nullptr, &m_Sampler));

	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // source
	builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);		   // target mip
	m_Layout = builder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
	for(VkDescriptorSet& set : m_Sets) set = engine->g_DescriptorAllocator.allocate(m_Device, m_Layout);

	VkPushConstantRange range{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BloomPushConstants) };
	VkPipelineLayoutCreateInfo layoutCI = vkinit::pipeline_layout_create_info();
	layoutCI.setLayoutCount = 1;
	layoutCI.pSetLayouts = &m_Layout;
	layoutCI.pushConstantRangeCount = 1;
	layoutCI.pPushConstantRanges = &range;
	VkPipelineLayout layout;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &layoutCI, nullptr, &layout));
	m_Effect = engine->loadComputeShader("src/shaders/bin/bloom.comp.spv", "bloom", layout); // pipeline destroyed by the application

	setTargets(engine->m_OutputImage); // window resolution, after the TSR
}

void Bloom::destroyChain() {
	if(m_ChainView == VK_NULL_HANDLE) return;
	vkDestroyImageView(m_Device, m_ChainView, nullptr);
	for(VkImageView view : m_MipViews) vkDestroyImageView(m_Device, view, nullptr);
	vmaDestroyImage(m_Allocator, m_Chain.image, m_Chain.allocation);
	m_ChainView = VK_NULL_HANDLE;
}

void Bloom::setTargets(const AllocatedImage& drawImage, float renderScale) {
	destroyChain();

	float toWindow = 0.5f / std::max(renderScale, 0.1f);
	VkExtent3D extent = { std::max(static_cast<uint32_t>(drawImage.imageExtent.width * toWindow), 1u), std::max(static_cast<uint32_t>(drawImage.imageExtent.height * toWindow), 1u), 1 };
	m_Chain.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
	m_Chain.imageExtent = extent;
	VkImageCreateInfo imageCI = vkinit::image_create_info(m_Chain.imageFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, extent);
	imageCI.mipLevels = MIPS;
	VmaAllocationCreateInfo allocCI{ .usage = VMA_MEMORY_USAGE_GPU_ONLY };
	VK_CHECK(vmaCreateImage(m_Allocator, &imageCI, &allocCI, &m_Chain.image, &m_Chain.allocation, nullptr));

	VkImageViewCreateInfo viewCI = vkinit::imageview_create_info(m_Chain.imageFormat, m_Chain.image, VK_IMAGE_ASPECT_COLOR_BIT);
	viewCI.subresourceRange.levelCount = MIPS;
	VK_CHECK(vkCreateImageView(m_Device, &viewCI, nullptr, &m_ChainView));
	for(uint32_t mip = 0; mip < MIPS; mip++) {
		VkImageViewCreateInfo mipCI = vkinit::imageview_create_info(m_Chain.imageFormat, m_Chain.image, VK_IMAGE_ASPECT_COLOR_BIT);
		mipCI.subresourceRange.baseMipLevel = mip;
		mipCI.subresourceRange.levelCount = 1;
		VK_CHECK(vkCreateImageView(m_Device, &mipCI, nullptr, &m_MipViews[mip]));
		m_MipSizes[mip] = { std::max(extent.width >> mip, 1u), std::max(extent.height >> mip, 1u) };
	}

	// downsample i reads the draw image (i = 0) or the chain, writes mip i; upsample j writes mip MIPS - 2 - j
	for(uint32_t pass = 0; pass < 2 * MIPS - 1; pass++) {
		bool down = pass < MIPS;
		uint32_t target = down ? pass : MIPS - 2 - (pass - MIPS);
		VkDescriptorImageInfo source{ m_Sampler, (down && pass == 0) ? drawImage.imageView : m_ChainView, VK_IMAGE_LAYOUT_GENERAL };
		VkDescriptorImageInfo destination{ VK_NULL_HANDLE, m_MipViews[target], VK_IMAGE_LAYOUT_GENERAL };
		VkWriteDescriptorSet writes[2] = {};
		for(auto& write : writes) { write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; write.dstSet = m_Sets[pass]; write.descriptorCount = 1; }
		writes[0].dstBinding = 0; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].pImageInfo = &source;
		writes[1].dstBinding = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[1].pImageInfo = &destination;
		vkUpdateDescriptorSets(m_Device, 2, writes, 0, nullptr);
	}
}

void Bloom::render(VkCommandBuffer cmd) {
	vkutil::transition_image(cmd, m_Chain.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Effect.pipeline);

	VkMemoryBarrier2 barrier{ .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
	barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
	barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	barrier.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
	VkDependencyInfo dependency{ .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	dependency.memoryBarrierCount = 1;
	dependency.pMemoryBarriers = &barrier;

	for(uint32_t pass = 0; pass < 2 * MIPS - 1; pass++) {
		bool down = pass < MIPS;
		uint32_t target = down ? pass : MIPS - 2 - (pass - MIPS);
		BloomPushConstants pc{ glm::ivec2(m_MipSizes[target].width, m_MipSizes[target].height), down ? (pass == 0 ? 0 : 1) : 2, down ? static_cast<int>(pass) - 1 : static_cast<int>(target) };
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Effect.layout, 0, 1, &m_Sets[pass], 0, nullptr);
		vkCmdPushConstants(cmd, m_Effect.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
		vkCmdDispatch(cmd, (m_MipSizes[target].width + 7) / 8, (m_MipSizes[target].height + 7) / 8, 1);
		vkCmdPipelineBarrier2(cmd, &dependency);
	}
}

void Bloom::destroy() {
	destroyChain();
	vkDestroySampler(m_Device, m_Sampler, nullptr);
	vkDestroyPipelineLayout(m_Device, m_Effect.layout, nullptr);
	vkDestroyDescriptorSetLayout(m_Device, m_Layout, nullptr);
}
