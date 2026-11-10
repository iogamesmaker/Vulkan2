#include "application.hpp"

// volumetric fog (volfog.glsl): two compute passes over a froxel grid. the volumes stay in GENERAL for good, they're
// written and sampled every frame, so only barriers between the passes

namespace {
struct FogInjectPushConstants {
	glm::mat4 invViewProj;
	glm::mat4 prevViewProj;
	glm::vec3 campos;
	float frame;
	glm::vec3 sundir;
	float terrainSize;
	glm::vec3 prevCampos;
	float factor;
	glm::vec2 cloudWind;
	glm::ivec2 mapOffset;
	glm::vec4 params; // x = density, y = valley mist, z = history valid, w = time
	glm::vec4 weather; // rgb = what the haze is made of (droplets white .. the sandstorm's dust), a = the weather's extra haze
};
static_assert(sizeof(FogInjectPushConstants) == 224, "must match volfog_inject.comp");

}

void Application::initVolumetricFog() {
	DescriptorLayoutBuilder injectBuilder;
	injectBuilder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // heightmap
	injectBuilder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // last frame's froxels
	injectBuilder.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);		  // this frame's froxels
	injectBuilder.add_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // cloud coverage map
	injectBuilder.add_binding(4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);		  // where the map is
	m_FogInjectLayout = injectBuilder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
	for(VkDescriptorSet& set : m_FogInjectSets) set = g_DescriptorAllocator.allocate(m_Device, m_FogInjectLayout);

	VkDescriptorSetLayout injectLayouts[] = { m_FogInjectLayout, m_AtmosphereManager.getRenderDescriptorLayout(), m_Shadows.getDescriptorLayout() };
	VkPushConstantRange injectRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FogInjectPushConstants) };
	VkPipelineLayoutCreateInfo injectLayoutCI = vkinit::pipeline_layout_create_info();
	injectLayoutCI.setLayoutCount = 3;
	injectLayoutCI.pSetLayouts = injectLayouts;
	injectLayoutCI.pushConstantRangeCount = 1;
	injectLayoutCI.pPushConstantRanges = &injectRange;
	VkPipelineLayout injectLayout;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &injectLayoutCI, nullptr, &injectLayout));
	m_FogInjectEffect = loadComputeShader("src/shaders/bin/volfog_inject.comp.spv", "volumetric fog inject", injectLayout);

	DescriptorLayoutBuilder integrateBuilder;
	integrateBuilder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // this frame's froxels
	integrateBuilder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);		 // integrated
	m_FogIntegrateLayout = integrateBuilder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
	for(VkDescriptorSet& set : m_FogIntegrateSets) set = g_DescriptorAllocator.allocate(m_Device, m_FogIntegrateLayout);

	VkPushConstantRange integrateRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(glm::vec4) };
	VkPipelineLayoutCreateInfo integrateLayoutCI = vkinit::pipeline_layout_create_info();
	integrateLayoutCI.setLayoutCount = 1;
	integrateLayoutCI.pSetLayouts = &m_FogIntegrateLayout;
	integrateLayoutCI.pushConstantRangeCount = 1;
	integrateLayoutCI.pPushConstantRanges = &integrateRange;
	VkPipelineLayout integrateLayout;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &integrateLayoutCI, nullptr, &integrateLayout));
	m_FogIntegrateEffect = loadComputeShader("src/shaders/bin/volfog_integrate.comp.spv", "volumetric fog integrate", integrateLayout);

	// the render set's binding 7 has to be valid from the first frame on, fog or not
	createFogVolumes(m_FogGrid.x > 0 ? m_FogGrid : glm::uvec3(160, 90, 16));

	m_DeletionQueue.push([this]() {
		destroyFogVolumes();
		vkDestroyPipelineLayout(m_Device, m_FogInjectEffect.layout, nullptr);
		vkDestroyPipelineLayout(m_Device, m_FogIntegrateEffect.layout, nullptr);
		vkDestroyDescriptorSetLayout(m_Device, m_FogInjectLayout, nullptr);
		vkDestroyDescriptorSetLayout(m_Device, m_FogIntegrateLayout, nullptr);
	});
}

void Application::destroyFogVolumes() {
	for(AllocatedImage* image : { &m_FogFroxels[0], &m_FogFroxels[1], &m_FogIntegrated }) {
		if(image->image == VK_NULL_HANDLE) continue;
		vkDestroyImageView(m_Device, image->imageView, nullptr);
		vmaDestroyImage(m_Allocator, image->image, image->allocation);
		*image = {};
	}
}

void Application::createFogVolumes(glm::uvec3 grid) {
	destroyFogVolumes();
	m_FogGrid = grid;
	VmaAllocationCreateInfo allocCI{ .usage = VMA_MEMORY_USAGE_GPU_ONLY };
	for(AllocatedImage* image : { &m_FogFroxels[0], &m_FogFroxels[1], &m_FogIntegrated }) {
		image->imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
		image->imageExtent = { grid.x, grid.y, grid.z };
		VkImageCreateInfo imageCI = vkinit::image_create_info(image->imageFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, image->imageExtent);
		imageCI.imageType = VK_IMAGE_TYPE_3D;
		VK_CHECK(vmaCreateImage(m_Allocator, &imageCI, &allocCI, &image->image, &image->allocation, nullptr));
		VkImageViewCreateInfo viewCI = vkinit::imageview_create_info(image->imageFormat, image->image, VK_IMAGE_ASPECT_COLOR_BIT);
		viewCI.viewType = VK_IMAGE_VIEW_TYPE_3D;
		VK_CHECK(vkCreateImageView(m_Device, &viewCI, nullptr, &image->imageView));
	}
	immediateSubmit([&](VkCommandBuffer cmd) {
		// no fog until the first frame writes some (transmittance 1)
		VkClearColorValue clear{ { 0.0f, 0.0f, 0.0f, 1.0f } };
		VkImageSubresourceRange range = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);
		for(AllocatedImage* image : { &m_FogFroxels[0], &m_FogFroxels[1], &m_FogIntegrated }) {
			vkutil::transition_image(cmd, image->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
			vkCmdClearColorImage(cmd, image->image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
		}
	});

	const VkImageLayout read = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	for(int i = 0; i < 2; i++) {
		VkDescriptorImageInfo injectInfos[4] = {
			{ m_HeightmapSampler, m_Heightmap.imageView, read },
			{ m_AtmosphereSampler, m_FogFroxels[1 - i].imageView, VK_IMAGE_LAYOUT_GENERAL }, // linear, clamped
			{ VK_NULL_HANDLE, m_FogFroxels[i].imageView, VK_IMAGE_LAYOUT_GENERAL },
			{ m_HeightmapSampler, m_CloudMap.imageView, VK_IMAGE_LAYOUT_GENERAL }, // repeating: it wraps
		};
		VkDescriptorBufferInfo mapWhere{ m_CloudMapInfo.buffer, 0, sizeof(glm::vec4) };
		VkWriteDescriptorSet writes[5] = {};
		for(uint32_t b = 0; b < 5; b++) {
			writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[b].dstSet = m_FogInjectSets[i];
			writes[b].dstBinding = b;
			writes[b].descriptorCount = 1;
			writes[b].descriptorType = b == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : (b == 4 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
			if(b < 4) writes[b].pImageInfo = &injectInfos[b];
			else writes[b].pBufferInfo = &mapWhere;
		}
		vkUpdateDescriptorSets(m_Device, 5, writes, 0, nullptr);

		VkDescriptorImageInfo integrateInfos[2] = {
			{ m_NearestSampler, m_FogFroxels[i].imageView, VK_IMAGE_LAYOUT_GENERAL },
			{ VK_NULL_HANDLE, m_FogIntegrated.imageView, VK_IMAGE_LAYOUT_GENERAL },
		};
		VkWriteDescriptorSet integrateWrites[2] = {};
		for(uint32_t b = 0; b < 2; b++) {
			integrateWrites[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			integrateWrites[b].dstSet = m_FogIntegrateSets[i];
			integrateWrites[b].dstBinding = b;
			integrateWrites[b].descriptorCount = 1;
			integrateWrites[b].descriptorType = b == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			integrateWrites[b].pImageInfo = &integrateInfos[b];
		}
		vkUpdateDescriptorSets(m_Device, 2, integrateWrites, 0, nullptr);
	}
	m_AtmosphereManager.setVolumetricFog(m_FogIntegrated.imageView, m_AtmosphereSampler);
	m_FogHistoryValid = false;
}

// before the SSAO in drawLighting (its GPU time shows in that pass' timer)
void Application::drawVolumetricFog(VkCommandBuffer cmd) {
	if(!m_VolumetricFog) { m_FogHistoryValid = false; return; }
	const uint32_t slot = m_FrameNumber % 2;
	const glm::mat4 viewProj = m_UnjitteredProjection * m_View;

	// last frame's lookups (lighting) are done with the integrated volume, its injection with these froxels
	vkutil::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
				  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

	FogInjectPushConstants pc{};
	pc.invViewProj = glm::inverse(viewProj);
	pc.prevViewProj = m_FogPrevViewProj;
	pc.campos = m_CameraPos;
	pc.frame = static_cast<float>(m_FrameNumber % 1024);
	pc.sundir = m_SunDir;
	pc.terrainSize = m_WorldSize * m_TerrainScale;
	pc.prevCampos = m_FogPrevCampos;
	pc.factor = m_TerrainPC.factor;
	pc.cloudWind = m_AtmosphereManager.getCloudWind();
	pc.mapOffset = m_MapOffset;
	bool valid = m_FogHistoryValid && m_MapOffset == m_FogPrevMapOffset; // re-centred: last frame's space is gone
	pc.params = glm::vec4(m_FogDensity, m_FogMist, valid ? 1.0f : 0.0f, m_Time);
	const WeatherState& weather = m_Weather.state();
	glm::vec3 dust = glm::mix(glm::vec3(0.95f), glm::vec3(0.85f, 0.6f, 0.36f), glm::clamp(weather.sand, 0.0f, 1.0f));
	pc.weather = glm::vec4(dust, weather.haze);
	pc.params.y = m_FogMist * weather.mist; // the weather's mist (a foggy morning) on top of the setting
	VkDescriptorSet injectSets[] = { m_FogInjectSets[slot], m_AtmosphereManager.getRenderDescriptor(), m_Shadows.getDescriptor(m_FrameNumber) };
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FogInjectEffect.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FogInjectEffect.layout, 0, 3, injectSets, 0, nullptr);
	vkCmdPushConstants(cmd, m_FogInjectEffect.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	vkCmdDispatch(cmd, (m_FogGrid.x + 7) / 8, (m_FogGrid.y + 7) / 8, m_FogGrid.z);

	vkutil::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

	glm::vec4 far(m_AtmosphereManager.m_AtmosphereData.fogDistance, 0.0f, 0.0f, 0.0f);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FogIntegrateEffect.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FogIntegrateEffect.layout, 0, 1, &m_FogIntegrateSets[slot], 0, nullptr);
	vkCmdPushConstants(cmd, m_FogIntegrateEffect.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(far), &far);
	vkCmdDispatch(cmd, (m_FogGrid.x + 7) / 8, (m_FogGrid.y + 7) / 8, 1);

	// lighting.comp
	vkutil::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
				  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

	m_FogPrevViewProj = viewProj;
	m_FogPrevCampos = m_CameraPos;
	m_FogPrevMapOffset = m_MapOffset;
	m_FogHistoryValid = true;
}
