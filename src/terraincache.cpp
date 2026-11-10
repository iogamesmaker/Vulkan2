#include "terraincache.hpp"
#include "application.hpp"

// level k's texels: the square reaches 0.85 of its half size at START_DISTANCE * 2^(k + 1), where the next
// level takes over (terrain.frag), the rest is room for the re-centring steps and the filter footprint
float TerrainCache::texelSize(uint32_t level) {
	return START_DISTANCE * 2.0f / 0.85f / static_cast<float>(SIZE / 2) * static_cast<float>(1u << level);
}

void TerrainCache::init(Application* engine, VkDescriptorSet terrainSet, VkDescriptorSetLayout terrainLayout, VkDescriptorSetLayout atmosphereLayout) {
	m_Engine = engine;
	m_Device = engine->m_Device;
	m_Allocator = engine->getAllocator();
	m_Mips = static_cast<uint32_t>(std::floor(std::log2(static_cast<float>(SIZE)))) + 1;

	// albedo: written through a UNORM view (storage can't be sRGB), sampled through an sRGB one (filtered in linear)
	auto createImage = [&](AllocatedImage& image, VkFormat format, VkImageCreateFlags flags) {
		image.imageFormat = format;
		image.imageExtent = { SIZE, SIZE, 1 };
		VkImageCreateInfo imageCI = vkinit::image_create_info(format, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, image.imageExtent);
		imageCI.flags = flags;
		imageCI.mipLevels = m_Mips;
		imageCI.arrayLayers = LEVELS;
		VmaAllocationCreateInfo allocCI{ .usage = VMA_MEMORY_USAGE_GPU_ONLY };
		VK_CHECK(vmaCreateImage(m_Allocator, &imageCI, &allocCI, &image.image, &image.allocation, nullptr));
	};
	// sampled views: only sampled (the sRGB one would otherwise inherit the image's storage usage, which sRGB
	// formats can't have)
	auto createView = [&](VkImage image, VkFormat format, uint32_t mips, uint32_t firstLayer, uint32_t layers, VkImageUsageFlags usage) {
		VkImageViewUsageCreateInfo usageCI{ .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO };
		usageCI.usage = usage;
		VkImageViewCreateInfo viewCI = vkinit::imageview_create_info(format, image, VK_IMAGE_ASPECT_COLOR_BIT);
		viewCI.pNext = &usageCI;
		viewCI.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
		viewCI.subresourceRange.levelCount = mips;
		viewCI.subresourceRange.baseArrayLayer = firstLayer;
		viewCI.subresourceRange.layerCount = layers;
		VkImageView view;
		VK_CHECK(vkCreateImageView(m_Device, &viewCI, nullptr, &view));
		return view;
	};
	createImage(m_Albedo, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT);
	createImage(m_Normal, VK_FORMAT_R8G8_UNORM, 0);
	m_AlbedoSampled = createView(m_Albedo.image, VK_FORMAT_R8G8B8A8_SRGB, m_Mips, 0, LEVELS, VK_IMAGE_USAGE_SAMPLED_BIT);
	m_NormalSampled = createView(m_Normal.image, VK_FORMAT_R8G8_UNORM, m_Mips, 0, LEVELS, VK_IMAGE_USAGE_SAMPLED_BIT);
	// storage views per level: a bake moves only its own level to GENERAL, and every image a dispatch's descriptors
	// reach has to be in the layout they were written with (the other levels are being sampled meanwhile)
	for(uint32_t k = 0; k < LEVELS; k++) {
		m_AlbedoStorage[k] = createView(m_Albedo.image, VK_FORMAT_R8G8B8A8_UNORM, 1, k, 1, VK_IMAGE_USAGE_STORAGE_BIT);
		m_NormalStorage[k] = createView(m_Normal.image, VK_FORMAT_R8G8_UNORM, 1, k, 1, VK_IMAGE_USAGE_STORAGE_BIT);
	}

	// wraps (the levels are toroidal), trilinear + anisotropic like the terrain's own textures
	VkSamplerCreateInfo samplerCI = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	samplerCI.magFilter = VK_FILTER_LINEAR;
	samplerCI.minFilter = VK_FILTER_LINEAR;
	samplerCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	samplerCI.addressModeU = samplerCI.addressModeV = samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerCI.maxLod = static_cast<float>(m_Mips);
	float anisotropy = 8.0f;
	if(const char* level = std::getenv("VULKAN2_ANISOTROPY")) anisotropy = std::max(1.0f, static_cast<float>(std::atof(level)));
	samplerCI.anisotropyEnable = anisotropy > 1.0f ? VK_TRUE : VK_FALSE;
	samplerCI.maxAnisotropy = anisotropy;
	VK_CHECK(vkCreateSampler(m_Device, &samplerCI, nullptr, &m_Sampler));

	m_InfoBuffer = engine->createBuffer(sizeof(Info), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

	// the bake: the terrain's set, the atmosphere's, the two images
	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE); // albedo
	builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE); // normal
	m_StorageLayout = builder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
	for(uint32_t k = 0; k < LEVELS; k++) {
		m_StorageSets[k] = engine->g_DescriptorAllocator.allocate(m_Device, m_StorageLayout);
		VkDescriptorImageInfo storageInfos[2] = {
			{ VK_NULL_HANDLE, m_AlbedoStorage[k], VK_IMAGE_LAYOUT_GENERAL },
			{ VK_NULL_HANDLE, m_NormalStorage[k], VK_IMAGE_LAYOUT_GENERAL },
		};
		VkWriteDescriptorSet writes[2] = {};
		for(uint32_t b = 0; b < 2; b++) {
			writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[b].dstSet = m_StorageSets[k];
			writes[b].dstBinding = b;
			writes[b].descriptorCount = 1;
			writes[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			writes[b].pImageInfo = &storageInfos[b];
		}
		vkUpdateDescriptorSets(m_Device, 2, writes, 0, nullptr);
	}

	VkDescriptorSetLayout layouts[] = { terrainLayout, atmosphereLayout, m_StorageLayout };
	VkPushConstantRange range{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BakePushConstants) };
	VkPipelineLayoutCreateInfo layoutCI = vkinit::pipeline_layout_create_info();
	layoutCI.setLayoutCount = 3;
	layoutCI.pSetLayouts = layouts;
	layoutCI.pushConstantRangeCount = 1;
	layoutCI.pPushConstantRanges = &range;
	VkPipelineLayout layout;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &layoutCI, nullptr, &layout));
	m_Effect = engine->loadComputeShader("src/shaders/bin/terraincache.comp.spv", "terrain cache bake", layout); // pipeline destroyed by the application

	// terrain.frag's side: the info buffer and the two images
	VkDescriptorBufferInfo infoBuffer{ m_InfoBuffer.buffer, 0, sizeof(Info) };
	VkDescriptorImageInfo sampledInfos[2] = {
		{ m_Sampler, m_AlbedoSampled, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ m_Sampler, m_NormalSampled, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
	};
	VkWriteDescriptorSet terrainWrites[3] = {};
	for(uint32_t i = 0; i < 3; i++) {
		terrainWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		terrainWrites[i].dstSet = terrainSet;
		terrainWrites[i].dstBinding = 6 + i;
		terrainWrites[i].descriptorCount = 1;
	}
	terrainWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	terrainWrites[0].pBufferInfo = &infoBuffer;
	for(uint32_t i = 1; i < 3; i++) {
		terrainWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		terrainWrites[i].pImageInfo = &sampledInfos[i - 1];
	}
	vkUpdateDescriptorSets(m_Device, 3, terrainWrites, 0, nullptr);

	// everything readable from the start (nothing is marked baked, so nothing reads it yet), the info all off
	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		VkImageMemoryBarrier2 barriers[2] = {};
		AllocatedImage* images[2] = { &m_Albedo, &m_Normal };
		for(int i = 0; i < 2; i++) {
			barriers[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
			barriers[i].srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
			barriers[i].dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
			barriers[i].dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
			barriers[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
			barriers[i].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
			barriers[i].image = images[i]->image;
			barriers[i].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, m_Mips, 0, LEVELS };
		}
		VkDependencyInfo dependency{ .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
		dependency.imageMemoryBarrierCount = 2;
		dependency.pImageMemoryBarriers = barriers;
		vkCmdPipelineBarrier2(cmd, &dependency);
		Info info{};
		vkCmdUpdateBuffer(cmd, m_InfoBuffer.buffer, 0, sizeof(Info), &info);
	});
}

void TerrainCache::invalidate() {
	for(Level& level : m_Levels) level.valid = false;
}

bool TerrainCache::update(VkCommandBuffer cmd, glm::dvec2 cameraWorld, float factor, float macroWeight, VkDescriptorSet terrainSet, VkDescriptorSet atmosphereSet) {
	if(macroWeight != m_MacroWeight) { invalidate(); m_MacroWeight = macroWeight; } // the macro layer setting is baked in

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Effect.pipeline);
	VkDescriptorSet sets[] = { terrainSet, atmosphereSet };
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Effect.layout, 0, 2, sets, 0, nullptr); // (set 2 per level, bake)
	bool wholeLevelBaked = false;
	for(uint32_t k = 0; k < LEVELS; k++) {
		// the square around the camera, in steps of SNAP texels
		double texel = texelSize(k);
		glm::ivec2 centre = glm::ivec2(glm::round(cameraWorld / (texel * SNAP))) * SNAP;
		glm::ivec2 min = centre - glm::ivec2(SIZE / 2);
		Level& level = m_Levels[k];
		glm::ivec2 moved = glm::abs(min - level.min);
		bool overlaps = level.valid && moved.x < static_cast<int>(SIZE) && moved.y < static_cast<int>(SIZE);
		if(overlaps && moved == glm::ivec2(0)) continue;
		if(!overlaps) {
			// a whole level is a lot of work (millions of texels of the full material): one a frame
			if(wholeLevelBaked) { level.valid = false; continue; }
			wholeLevelBaked = true;
		}
		// the bake reads the shared terrain set; the strips cost little
		bake(cmd, k, min, overlaps ? &level : nullptr, factor, macroWeight);
		level.min = min;
		level.valid = true;
	}

	uploadInfo(cmd, true);
	return wholeLevelBaked; // (the stutter log)
}

void TerrainCache::publishOff(VkCommandBuffer cmd) { uploadInfo(cmd, false); }

void TerrainCache::uploadInfo(VkCommandBuffer cmd, bool on) {
	// what terrain.frag and foliage.comp read (last frame's reads are done with the buffer by the time this runs: the barrier)
	Info info{};
	for(uint32_t k = 0; k < LEVELS; k++) info.levels[k] = glm::vec4(glm::vec2(m_Levels[k].min), texelSize(k), on && m_Levels[k].valid ? 1.0f : 0.0f);
	info.params = glm::vec4(START_DISTANCE, on ? 1.0f : 0.0f, static_cast<float>(SIZE), 0.0f);
	VkMemoryBarrier2 barrier{ .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
	barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	barrier.srcAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
	barrier.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
	barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	VkDependencyInfo dependency{ .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	dependency.memoryBarrierCount = 1;
	dependency.pMemoryBarriers = &barrier;
	vkCmdPipelineBarrier2(cmd, &dependency);
	vkCmdUpdateBuffer(cmd, m_InfoBuffer.buffer, 0, sizeof(Info), &info);
	barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
	barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	barrier.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
	vkCmdPipelineBarrier2(cmd, &dependency);
}

void TerrainCache::bake(VkCommandBuffer cmd, uint32_t k, glm::ivec2 min, const Level* keep, float factor, float macroWeight) {
	AllocatedImage* images[2] = { &m_Albedo, &m_Normal };
	VkImageMemoryBarrier2 barriers[2] = {};
	VkDependencyInfo dependency{ .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	dependency.imageMemoryBarrierCount = 2;
	dependency.pImageMemoryBarriers = barriers;
	auto transition = [&](uint32_t baseMip, uint32_t mips, VkImageLayout from, VkImageLayout to, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
		for(int i = 0; i < 2; i++) {
			barriers[i] = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
			barriers[i].srcStageMask = srcStage;
			barriers[i].srcAccessMask = srcAccess;
			barriers[i].dstStageMask = dstStage;
			barriers[i].dstAccessMask = dstAccess;
			barriers[i].oldLayout = from;
			barriers[i].newLayout = to;
			barriers[i].image = images[i]->image;
			barriers[i].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, baseMip, mips, k, 1 };
		}
		vkCmdPipelineBarrier2(cmd, &dependency);
	};

	// the top mip keeps what it has (only the new strips are baked), last frame's terrain may still be reading it
	transition(0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
			   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
	BakePushConstants pc{};
	pc.bakeMin = min;
	pc.keepMin = keep ? keep->min : glm::ivec2(0);
	pc.level = static_cast<int>(k);
	pc.keep = keep ? 1 : 0;
	pc.texel = texelSize(k);
	pc.factor = factor;
	// the macro texture layer takes over around 180-260 units (terrain.frag): the levels from there on have it
	pc.macroWeight = START_DISTANCE * static_cast<float>(1u << k) >= 200.0f ? macroWeight : 0.0f;
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Effect.layout, 2, 1, &m_StorageSets[k], 0, nullptr); // this level's
	vkCmdPushConstants(cmd, m_Effect.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	vkCmdDispatch(cmd, SIZE / 8, SIZE / 8, 1);

	// the mip chain, all of it (a strip changes every mip under it, the whole chain costs about as much as tracking that)
	transition(0, 1, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	transition(1, m_Mips - 1, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			   VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
	for(uint32_t mip = 1; mip < m_Mips; mip++) {
		int32_t from = static_cast<int32_t>(SIZE >> (mip - 1)), to = static_cast<int32_t>(SIZE >> mip);
		for(AllocatedImage* image : images) {
			VkImageBlit2 region{ .sType = VK_STRUCTURE_TYPE_IMAGE_BLIT_2 };
			region.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip - 1, k, 1 };
			region.srcOffsets[1] = { from, from, 1 };
			region.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, mip, k, 1 };
			region.dstOffsets[1] = { to, to, 1 };
			VkBlitImageInfo2 blit{ .sType = VK_STRUCTURE_TYPE_BLIT_IMAGE_INFO_2 };
			blit.srcImage = image->image;
			blit.srcImageLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
			blit.dstImage = image->image;
			blit.dstImageLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
			blit.regionCount = 1;
			blit.pRegions = &region;
			blit.filter = VK_FILTER_LINEAR;
			vkCmdBlitImage2(cmd, &blit);
		}
		transition(mip, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
				   VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	}
	transition(0, m_Mips, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			   VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
}

void TerrainCache::destroy() {
	vkDestroyImageView(m_Device, m_AlbedoSampled, nullptr);
	for(uint32_t k = 0; k < LEVELS; k++) vkDestroyImageView(m_Device, m_AlbedoStorage[k], nullptr);
	vkDestroyImageView(m_Device, m_NormalSampled, nullptr);
	for(uint32_t k = 0; k < LEVELS; k++) vkDestroyImageView(m_Device, m_NormalStorage[k], nullptr);
	vmaDestroyImage(m_Allocator, m_Albedo.image, m_Albedo.allocation);
	vmaDestroyImage(m_Allocator, m_Normal.image, m_Normal.allocation);
	vkDestroySampler(m_Device, m_Sampler, nullptr);
	m_Engine->destroyBuffer(m_InfoBuffer);
	vkDestroyDescriptorSetLayout(m_Device, m_StorageLayout, nullptr);
	vkDestroyPipelineLayout(m_Device, m_Effect.layout, nullptr);
}
