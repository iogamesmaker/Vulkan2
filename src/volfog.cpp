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
	for(FogVolumes* fog : { &m_Fog, &m_FogCompare })
		for(VkDescriptorSet& set : fog->injectSets) set = g_DescriptorAllocator.allocate(m_Device, m_FogInjectLayout);

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
	for(FogVolumes* fog : { &m_Fog, &m_FogCompare })
		for(VkDescriptorSet& set : fog->integrateSets) set = g_DescriptorAllocator.allocate(m_Device, m_FogIntegrateLayout);

	VkPushConstantRange integrateRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(glm::vec4) };
	VkPipelineLayoutCreateInfo integrateLayoutCI = vkinit::pipeline_layout_create_info();
	integrateLayoutCI.setLayoutCount = 1;
	integrateLayoutCI.pSetLayouts = &m_FogIntegrateLayout;
	integrateLayoutCI.pushConstantRangeCount = 1;
	integrateLayoutCI.pPushConstantRanges = &integrateRange;
	VkPipelineLayout integrateLayout;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &integrateLayoutCI, nullptr, &integrateLayout));
	m_FogIntegrateEffect = loadComputeShader("src/shaders/bin/volfog_integrate.comp.spv", "volumetric fog integrate", integrateLayout);

	// the render set's bindings 7 and 9 have to be valid from the first frame on, fog or not
	createFogVolumes(m_Fog, m_Fog.grid.x > 0 ? m_Fog.grid : glm::uvec3(160, 90, 16));
	updateFogCompare();

	m_DeletionQueue.push([this]() {
		destroyFogVolumes(m_Fog);
		destroyFogVolumes(m_FogCompare);
		vkDestroyPipelineLayout(m_Device, m_FogInjectEffect.layout, nullptr);
		vkDestroyPipelineLayout(m_Device, m_FogIntegrateEffect.layout, nullptr);
		vkDestroyDescriptorSetLayout(m_Device, m_FogInjectLayout, nullptr);
		vkDestroyDescriptorSetLayout(m_Device, m_FogIntegrateLayout, nullptr);
	});
}

void Application::destroyFogVolumes(FogVolumes& fog) {
	for(AllocatedImage* image : { &fog.froxels[0], &fog.froxels[1], &fog.integrated }) {
		if(image->image == VK_NULL_HANDLE) continue;
		vkDestroyImageView(m_Device, image->imageView, nullptr);
		vmaDestroyImage(m_Allocator, image->image, image->allocation);
		*image = {};
	}
	fog.grid = glm::uvec3(0);
}

void Application::createFogVolumes(FogVolumes& fog, glm::uvec3 grid) {
	destroyFogVolumes(fog);
	fog.grid = grid;
	VmaAllocationCreateInfo allocCI{ .usage = VMA_MEMORY_USAGE_GPU_ONLY };
	for(AllocatedImage* image : { &fog.froxels[0], &fog.froxels[1], &fog.integrated }) {
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
		for(AllocatedImage* image : { &fog.froxels[0], &fog.froxels[1], &fog.integrated }) {
			vkutil::transition_image(cmd, image->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
			vkCmdClearColorImage(cmd, image->image, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
		}
	});

	const VkImageLayout read = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	for(int i = 0; i < 2; i++) {
		VkDescriptorImageInfo injectInfos[4] = {
			{ m_HeightmapSampler, m_Heightmap.imageView, read },
			{ m_AtmosphereSampler, fog.froxels[1 - i].imageView, VK_IMAGE_LAYOUT_GENERAL }, // linear, clamped
			{ VK_NULL_HANDLE, fog.froxels[i].imageView, VK_IMAGE_LAYOUT_GENERAL },
			{ m_HeightmapSampler, m_CloudMap.imageView, VK_IMAGE_LAYOUT_GENERAL }, // repeating: it wraps
		};
		VkDescriptorBufferInfo mapWhere{ m_CloudMapInfo.buffer, 0, sizeof(glm::vec4) };
		VkWriteDescriptorSet writes[5] = {};
		for(uint32_t b = 0; b < 5; b++) {
			writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[b].dstSet = fog.injectSets[i];
			writes[b].dstBinding = b;
			writes[b].descriptorCount = 1;
			writes[b].descriptorType = b == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : (b == 4 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
			if(b < 4) writes[b].pImageInfo = &injectInfos[b];
			else writes[b].pBufferInfo = &mapWhere;
		}
		vkUpdateDescriptorSets(m_Device, 5, writes, 0, nullptr);

		VkDescriptorImageInfo integrateInfos[2] = {
			{ m_NearestSampler, fog.froxels[i].imageView, VK_IMAGE_LAYOUT_GENERAL },
			{ VK_NULL_HANDLE, fog.integrated.imageView, VK_IMAGE_LAYOUT_GENERAL },
		};
		VkWriteDescriptorSet integrateWrites[2] = {};
		for(uint32_t b = 0; b < 2; b++) {
			integrateWrites[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			integrateWrites[b].dstSet = fog.integrateSets[i];
			integrateWrites[b].dstBinding = b;
			integrateWrites[b].descriptorCount = 1;
			integrateWrites[b].descriptorType = b == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			integrateWrites[b].pImageInfo = &integrateInfos[b];
		}
		vkUpdateDescriptorSets(m_Device, 2, integrateWrites, 0, nullptr);
	}
	fog.historyValid = false;
	if(&fog == &m_Fog) m_AtmosphereManager.setVolumetricFog(m_Fog.integrated.imageView, m_AtmosphereSampler);
}

// TEMPORARY slices comparison: the second volume (the base grid's slices times m_FogSliceMultiplier) exists only
// while the multiplier isn't 1, lighting.comp shows only the difference to the base fog then (binding 9 and the
// UBO's fogCompare flag). otherwise binding 9 is the base volume again. the GPU has to be idle
void Application::updateFogCompare() {
	bool compare = std::abs(m_FogSliceMultiplier - 1.0f) > 1e-3f && m_Fog.grid.x > 0;
	if(compare) {
		glm::uvec3 grid(m_Fog.grid.x, m_Fog.grid.y, std::max(1u, static_cast<uint32_t>(std::lround(m_Fog.grid.z * m_FogSliceMultiplier))));
		if(grid != m_FogCompare.grid) createFogVolumes(m_FogCompare, grid);
	} else if(m_FogCompare.grid.x > 0) destroyFogVolumes(m_FogCompare);
	m_AtmosphereManager.setVolumetricFogCompare(compare ? m_FogCompare.integrated.imageView : m_Fog.integrated.imageView, m_AtmosphereSampler);
	m_AtmosphereManager.m_AtmosphereData.fogCompare = glm::vec4(compare ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
	m_ShadowFilter.m_AbsoluteResult = compare;
}

// before the SSAO in drawLighting (its GPU time shows in that pass' timer)
void Application::drawVolumetricFog(VkCommandBuffer cmd) {
	if(!m_VolumetricFog) { m_Fog.historyValid = m_FogCompare.historyValid = false; return; }
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
	const WeatherState& weather = m_Weather.state();
	glm::vec3 dust = glm::mix(glm::vec3(0.95f), glm::vec3(0.85f, 0.6f, 0.36f), glm::clamp(weather.sand, 0.0f, 1.0f));
	pc.weather = glm::vec4(dust, weather.haze);
	glm::vec4 far(m_AtmosphereManager.m_AtmosphereData.fogDistance, 0.0f, 0.0f, 0.0f);

	// the base volume always, the comparison one (TEMPORARY, updateFogCompare) when it exists: the same fog, its own grid
	for(FogVolumes* fog : { &m_Fog, &m_FogCompare }) {
		if(fog->grid.x == 0) continue;
		bool valid = fog->historyValid && m_MapOffset == m_FogPrevMapOffset; // re-centred: last frame's space is gone
		pc.params = glm::vec4(m_FogDensity, m_FogMist, valid ? 1.0f : 0.0f, m_Time);
		VkDescriptorSet injectSets[] = { fog->injectSets[slot], m_AtmosphereManager.getRenderDescriptor(), m_Shadows.getDescriptor(m_FrameNumber) };
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FogInjectEffect.pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FogInjectEffect.layout, 0, 3, injectSets, 0, nullptr);
		vkCmdPushConstants(cmd, m_FogInjectEffect.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
		vkCmdDispatch(cmd, (fog->grid.x + 7) / 8, (fog->grid.y + 7) / 8, fog->grid.z);

		vkutil::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FogIntegrateEffect.pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_FogIntegrateEffect.layout, 0, 1, &fog->integrateSets[slot], 0, nullptr);
		vkCmdPushConstants(cmd, m_FogIntegrateEffect.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(far), &far);
		vkCmdDispatch(cmd, (fog->grid.x + 7) / 8, (fog->grid.y + 7) / 8, 1);
		fog->historyValid = true;
	}

	// lighting.comp
	vkutil::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
				  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);

	m_FogPrevViewProj = viewProj;
	m_FogPrevCampos = m_CameraPos;
	m_FogPrevMapOffset = m_MapOffset;
}
