#include "hiz.hpp"
#include "application.hpp"

struct HiZPushConstants {
	glm::ivec2 sourceSize;
	int sourceMip;
};

void HiZ::init(Application* engine) {
	m_Engine = engine;
	m_Device = engine->m_Device;
	m_Allocator = engine->getAllocator();

	VkSamplerCreateInfo samplerCI = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	samplerCI.magFilter = VK_FILTER_NEAREST;
	samplerCI.minFilter = VK_FILTER_NEAREST;
	samplerCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; // every lookup is a texelFetch naming its mip
	samplerCI.addressModeU = samplerCI.addressModeV = samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerCI.maxLod = static_cast<float>(MAX_MIPS);
	VK_CHECK(vkCreateSampler(m_Device, &samplerCI, nullptr, &m_Sampler));

	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // source
	builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);		   // target mip
	m_Layout = builder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
	for(VkDescriptorSet& set : m_Sets) set = engine->g_DescriptorAllocator.allocate(m_Device, m_Layout);

	VkPushConstantRange range{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(HiZPushConstants) };
	VkPipelineLayoutCreateInfo layoutCI = vkinit::pipeline_layout_create_info();
	layoutCI.setLayoutCount = 1;
	layoutCI.pSetLayouts = &m_Layout;
	layoutCI.pushConstantRangeCount = 1;
	layoutCI.pPushConstantRanges = &range;
	VkPipelineLayout layout;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &layoutCI, nullptr, &layout));
	m_Effect = engine->loadComputeShader("src/shaders/bin/hiz.comp.spv", "depth pyramid", layout); // pipeline destroyed by the application

	setSource(engine->m_DepthBuffer);
}

void HiZ::destroyPyramid() {
	if(m_PyramidView == VK_NULL_HANDLE) return;
	vkDestroyImageView(m_Device, m_PyramidView, nullptr);
	for(uint32_t mip = 0; mip < m_Mips; mip++) vkDestroyImageView(m_Device, m_MipViews[mip], nullptr);
	vmaDestroyImage(m_Allocator, m_Pyramid.image, m_Pyramid.allocation);
	m_PyramidView = VK_NULL_HANDLE;
}

void HiZ::setSource(const AllocatedImage& depthBuffer) {
	destroyPyramid();

	m_DepthSize = { depthBuffer.imageExtent.width, depthBuffer.imageExtent.height };
	VkExtent3D extent = { std::max((m_DepthSize.width + 1) / 2, 1u), std::max((m_DepthSize.height + 1) / 2, 1u), 1 };
	m_Mips = std::min(static_cast<uint32_t>(std::floor(std::log2(static_cast<float>(std::max(extent.width, extent.height))))) + 1, MAX_MIPS);
	m_Pyramid.imageFormat = VK_FORMAT_R32_SFLOAT;
	m_Pyramid.imageExtent = extent;
	VkImageCreateInfo imageCI = vkinit::image_create_info(m_Pyramid.imageFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, extent);
	imageCI.mipLevels = m_Mips;
	VmaAllocationCreateInfo allocCI{ .usage = VMA_MEMORY_USAGE_GPU_ONLY };
	VK_CHECK(vmaCreateImage(m_Allocator, &imageCI, &allocCI, &m_Pyramid.image, &m_Pyramid.allocation, nullptr));

	VkImageViewCreateInfo viewCI = vkinit::imageview_create_info(m_Pyramid.imageFormat, m_Pyramid.image, VK_IMAGE_ASPECT_COLOR_BIT);
	viewCI.subresourceRange.levelCount = m_Mips;
	VK_CHECK(vkCreateImageView(m_Device, &viewCI, nullptr, &m_PyramidView));
	for(uint32_t mip = 0; mip < m_Mips; mip++) {
		VkImageViewCreateInfo mipCI = vkinit::imageview_create_info(m_Pyramid.imageFormat, m_Pyramid.image, VK_IMAGE_ASPECT_COLOR_BIT);
		mipCI.subresourceRange.baseMipLevel = mip;
		mipCI.subresourceRange.levelCount = 1;
		VK_CHECK(vkCreateImageView(m_Device, &mipCI, nullptr, &m_MipViews[mip]));
		m_MipSizes[mip] = { std::max(extent.width >> mip, 1u), std::max(extent.height >> mip, 1u) };
	}

	// mip i reads the depth buffer (i = 0) or the pyramid itself (mip i - 1, a different subresource)
	for(uint32_t mip = 0; mip < m_Mips; mip++) {
		VkDescriptorImageInfo source = mip == 0 ? VkDescriptorImageInfo{ m_Sampler, depthBuffer.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }
												: VkDescriptorImageInfo{ m_Sampler, m_PyramidView, VK_IMAGE_LAYOUT_GENERAL };
		VkDescriptorImageInfo destination{ VK_NULL_HANDLE, m_MipViews[mip], VK_IMAGE_LAYOUT_GENERAL };
		VkWriteDescriptorSet writes[2] = {};
		for(auto& write : writes) { write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; write.dstSet = m_Sets[mip]; write.descriptorCount = 1; }
		writes[0].dstBinding = 0; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].pImageInfo = &source;
		writes[1].dstBinding = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[1].pImageInfo = &destination;
		vkUpdateDescriptorSets(m_Device, 2, writes, 0, nullptr);
	}

	// GENERAL for good: written as storage, read sampled
	m_Engine->immediateSubmit([&](VkCommandBuffer cmd) {
		VkImageMemoryBarrier2 barrier{ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
		barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
		barrier.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_SHADER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		barrier.image = m_Pyramid.image;
		barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, m_Mips, 0, 1 };
		VkDependencyInfo dependency{ .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
		dependency.imageMemoryBarrierCount = 1;
		dependency.pImageMemoryBarriers = &barrier;
		vkCmdPipelineBarrier2(cmd, &dependency);
	});
}

void HiZ::build(VkCommandBuffer cmd) {
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Effect.pipeline);
	VkMemoryBarrier2 barrier{ .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
	VkDependencyInfo dependency{ .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	dependency.memoryBarrierCount = 1;
	dependency.pMemoryBarriers = &barrier;
	// last frame's culling read the pyramid this overwrites
	barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	barrier.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
	barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	barrier.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
	vkCmdPipelineBarrier2(cmd, &dependency);
	for(uint32_t mip = 0; mip < m_Mips; mip++) {
		HiZPushConstants pc{};
		pc.sourceSize = mip == 0 ? glm::ivec2(m_DepthSize.width, m_DepthSize.height) : glm::ivec2(m_MipSizes[mip - 1].width, m_MipSizes[mip - 1].height);
		pc.sourceMip = mip == 0 ? 0 : static_cast<int>(mip) - 1;
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Effect.layout, 0, 1, &m_Sets[mip], 0, nullptr);
		vkCmdPushConstants(cmd, m_Effect.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
		vkCmdDispatch(cmd, (m_MipSizes[mip].width + 7) / 8, (m_MipSizes[mip].height + 7) / 8, 1);
		// the next mip reads this one, the culling reads them all
		barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
		vkCmdPipelineBarrier2(cmd, &dependency);
	}
}

void HiZ::destroy() {
	destroyPyramid();
	vkDestroySampler(m_Device, m_Sampler, nullptr);
	vkDestroyDescriptorSetLayout(m_Device, m_Layout, nullptr);
	vkDestroyPipelineLayout(m_Device, m_Effect.layout, nullptr);
}
