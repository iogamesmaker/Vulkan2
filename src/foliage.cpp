#include "foliage.hpp"
#include "gbuffer.hpp"
#include "application.hpp"

void Foliage::init(Application* engine, const AllocatedImage& heightmap, const AllocatedImage& climateMap, VkSampler heightmapSampler, VkFormat shadowFormat,
				   const AllocatedImage& bladeAtlas, const AllocatedImage& flowerAtlas, const AllocatedImage& wheatAtlas,
				   VkSampler atlasSampler,
				   VkDescriptorSetLayout atmosphereLayout, VkDescriptorSetLayout shadowLayout, VkBuffer interactionBuffer) {
	m_Device = engine->m_Device;
	m_Allocator = engine->getAllocator();

	// buffers: instances written by the compute pass and read by the vertex shader, the indirect draw arguments
	// get their instance count from the compute pass too
	m_Instances = engine->createBuffer((MAX_INSTANCES + MAX_SHADOW_INSTANCES + MAX_NEAR_INSTANCES) * sizeof(glm::vec4) * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	m_Indirect = engine->createBuffer(sizeof(VkDrawIndirectCommand) * 3,  VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

	// compute set
	DescriptorLayoutBuilder computeBuilder;
	computeBuilder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // heightmap
	computeBuilder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);		  // instances
	computeBuilder.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);		  // indirect args
	computeBuilder.add_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // climate map

	m_ComputeLayout = computeBuilder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
	m_ComputeSet = engine->g_DescriptorAllocator.allocate(m_Device, m_ComputeLayout);

	// render set
	DescriptorLayoutBuilder renderBuilder;
	renderBuilder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);		 // instances
	renderBuilder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // blade atlas
	renderBuilder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // flower atlas
	renderBuilder.add_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // wheat (oat) atlas
	renderBuilder.add_binding(5, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);		 // the player's trail (interaction.glsl)
	m_RenderLayout = renderBuilder.build(m_Device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	m_RenderSet = engine->g_DescriptorAllocator.allocate(m_Device, m_RenderLayout);

	VkDescriptorImageInfo atlasInfo{ atlasSampler, bladeAtlas.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkDescriptorImageInfo flowerInfo{ atlasSampler, flowerAtlas.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkDescriptorImageInfo wheatInfo{ atlasSampler, wheatAtlas.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkDescriptorBufferInfo instanceInfo{ m_Instances.buffer, 0, VK_WHOLE_SIZE };
	VkDescriptorBufferInfo indirectInfo{ m_Indirect.buffer, 0, VK_WHOLE_SIZE };

	VkDescriptorBufferInfo interactionInfo{ interactionBuffer, 0, VK_WHOLE_SIZE };

	VkWriteDescriptorSet writes[7] = {};
	for(auto& write : writes) {
		write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		write.descriptorCount = 1;
	}
	writes[0].dstSet = m_ComputeSet; writes[0].dstBinding = 1; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[0].pBufferInfo = &instanceInfo;
	writes[1].dstSet = m_ComputeSet; writes[1].dstBinding = 2; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[1].pBufferInfo = &indirectInfo;
	writes[2].dstSet = m_RenderSet;	 writes[2].dstBinding = 0; writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[2].pBufferInfo = &instanceInfo;
	writes[3].dstSet = m_RenderSet;	 writes[3].dstBinding = 1; writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[3].pImageInfo = &atlasInfo;
	writes[4].dstSet = m_RenderSet;	 writes[4].dstBinding = 2; writes[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[4].pImageInfo = &flowerInfo;
	writes[5].dstSet = m_RenderSet;	 writes[5].dstBinding = 3; writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[5].pImageInfo = &wheatInfo;
	writes[6].dstSet = m_RenderSet;	 writes[6].dstBinding = 5; writes[6].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; writes[6].pBufferInfo = &interactionInfo;
	vkUpdateDescriptorSets(m_Device, 7, writes, 0, nullptr);
	setHeightmap(heightmap, climateMap, heightmapSampler); // compute set bindings 0 and 3

	// scatter pipeline
	VkPushConstantRange computeRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FoliagePushConstants) };
	VkPipelineLayoutCreateInfo computeLayoutCI = vkinit::pipeline_layout_create_info();
	computeLayoutCI.setLayoutCount = 1;
	computeLayoutCI.pSetLayouts = &m_ComputeLayout;
	computeLayoutCI.pushConstantRangeCount = 1;
	computeLayoutCI.pPushConstantRanges = &computeRange;
	VkPipelineLayout scatterLayout;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &computeLayoutCI, nullptr, &scatterLayout));
	m_Scatter = engine->loadComputeShader("src/shaders/bin/foliage.comp.spv", "foliage scatter", scatterLayout);

	// render pipeline, sets 1 / 2 are the same atmosphere + shadow sets the terrain uses
	VkDescriptorSetLayout renderLayouts[] = { m_RenderLayout, atmosphereLayout, shadowLayout };
	VkPushConstantRange renderRange{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FoliagePushConstants) };
	VkPipelineLayoutCreateInfo renderLayoutCI = vkinit::pipeline_layout_create_info();
	renderLayoutCI.setLayoutCount = 3;
	renderLayoutCI.pSetLayouts = renderLayouts;
	renderLayoutCI.pushConstantRangeCount = 1;
	renderLayoutCI.pPushConstantRanges = &renderRange;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &renderLayoutCI, nullptr, &m_RenderPipelineLayout));

	CompiledShader vertexShader = engine->loadShader("src/shaders/bin/foliage.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
	CompiledShader fragmentShader = engine->loadShader("src/shaders/bin/foliage.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);

	PipelineBuilder builder;
	builder._pipelineLayout = m_RenderPipelineLayout;
	builder.set_shaders(vertexShader.shader, fragmentShader.shader);
	builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST); // no vertex input, everything comes from the instance buffer
	builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
	builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE); // blades are two sided
	builder.set_multisampling_none();
	builder.disable_blending();
	builder.enable_depthtest(false, VK_COMPARE_OP_EQUAL); // after the prepass, shades only the visible blade
	setGBufferTargets(builder, true); // deferred, lit in lighting.comp
	builder.set_depth_format(engine->m_DepthBuffer.imageFormat);
	m_RenderPipeline = builder.build_pipeline(m_Device);

	// depth only version for the shadow maps, set 0 only
	VkPipelineLayoutCreateInfo shadowLayoutCI = renderLayoutCI;
	shadowLayoutCI.setLayoutCount = 1;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &shadowLayoutCI, nullptr, &m_ShadowPipelineLayout));

	CompiledShader shadowShader = engine->loadShader("src/shaders/bin/foliage_shadow.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
	PipelineBuilder shadowBuilder;
	shadowBuilder._pipelineLayout = m_ShadowPipelineLayout;
	shadowBuilder.set_shaders(vertexShader.shader, shadowShader.shader);
	shadowBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	shadowBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
	shadowBuilder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	shadowBuilder._rasterizer.depthBiasEnable = VK_TRUE;
	shadowBuilder._rasterizer.depthBiasConstantFactor = 2.0f;
	shadowBuilder._rasterizer.depthBiasSlopeFactor = 2.5f;
	shadowBuilder.set_multisampling_none();
	shadowBuilder.disable_blending();
	shadowBuilder.enable_depthtest(true, VK_COMPARE_OP_LESS); // shadow maps are regular z
	shadowBuilder.set_depth_format(shadowFormat);
	m_ShadowPipeline = shadowBuilder.build_pipeline(m_Device);

	// depth prepass: the alpha test only, no colour (same layout as the shadow pass, set 0 only)
	CompiledShader prepassShader = engine->loadShader("src/shaders/bin/foliage_prepass.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
	PipelineBuilder prepassBuilder;
	prepassBuilder._pipelineLayout = m_ShadowPipelineLayout;
	prepassBuilder.set_shaders(vertexShader.shader, prepassShader.shader);
	prepassBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	prepassBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
	prepassBuilder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	prepassBuilder.set_multisampling_none();
	prepassBuilder.disable_blending();
	prepassBuilder.enable_depthtest(true, VK_COMPARE_OP_GREATER); // reversed z
	setGBufferTargets(prepassBuilder, false);
	prepassBuilder._colorBlendAttachment.colorWriteMask = 0;
	prepassBuilder.set_depth_format(engine->m_DepthBuffer.imageFormat);
	m_PrepassPipeline = prepassBuilder.build_pipeline(m_Device);

	vkDestroyShaderModule(m_Device, vertexShader.shader, nullptr);
	vkDestroyShaderModule(m_Device, fragmentShader.shader, nullptr);
	vkDestroyShaderModule(m_Device, shadowShader.shader, nullptr);
	vkDestroyShaderModule(m_Device, prepassShader.shader, nullptr);
}

void Foliage::setHeightmap(const AllocatedImage& heightmap, const AllocatedImage& climateMap, VkSampler heightmapSampler) {
	VkDescriptorImageInfo infos[2] = {
		{ heightmapSampler, heightmap.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ heightmapSampler, climateMap.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
	};
	VkWriteDescriptorSet writes[2] = {};
	for(uint32_t i = 0; i < 2; i++) {
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = m_ComputeSet;
		writes[i].dstBinding = i == 0 ? 0 : 3;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[i].pImageInfo = &infos[i];
	}
	vkUpdateDescriptorSets(m_Device, 2, writes, 0, nullptr);
}

void Foliage::destroy() {
	vkDestroyPipeline(m_Device, m_RenderPipeline, nullptr);
	vkDestroyPipeline(m_Device, m_ShadowPipeline, nullptr);
	vkDestroyPipeline(m_Device, m_PrepassPipeline, nullptr);
	vkDestroyPipelineLayout(m_Device, m_ShadowPipelineLayout, nullptr);
	vkDestroyPipelineLayout(m_Device, m_RenderPipelineLayout, nullptr);
	vkDestroyPipelineLayout(m_Device, m_Scatter.layout, nullptr);
	vkDestroyDescriptorSetLayout(m_Device, m_ComputeLayout, nullptr);
	vkDestroyDescriptorSetLayout(m_Device, m_RenderLayout, nullptr);
	vmaDestroyBuffer(m_Allocator, m_Instances.buffer, m_Instances.allocation);
	vmaDestroyBuffer(m_Allocator, m_Indirect.buffer, m_Indirect.allocation);
}

void Foliage::update(VkCommandBuffer cmd, const FoliagePushConstants& pc) {
	m_PushConstants = pc;
	if(!m_Enabled) return;

	// the previous frame might still be drawing from these buffers
	vkutil::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
				  VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

	// (outer rings, shadow casters, near ring: see foliagecommon.glsl for the instance ranges)
	VkDrawIndirectCommand reset[3] = { { VERTICES_PER_INSTANCE, 0, 0, 0 }, { SEGMENTED_VERTICES_PER_INSTANCE, 0, 0, MAX_INSTANCES },
									   { SEGMENTED_VERTICES_PER_INSTANCE, 0, 0, MAX_INSTANCES + MAX_SHADOW_INSTANCES } };
	vkCmdUpdateBuffer(cmd, m_Indirect.buffer, 0, sizeof(reset), reset);

	vkutil::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
				  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Scatter.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Scatter.layout, 0, 1, &m_ComputeSet, 0, nullptr);
	vkCmdPushConstants(cmd, m_Scatter.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FoliagePushConstants), &m_PushConstants);
	// the rings are consecutive thread ranges (foliage.comp), fewer rings = fewer threads
	uint32_t cells = RING_SIDE_NEAR * RING_SIDE_NEAR;
	if(m_Rings > 1) cells += RING_SIDE_MID * RING_SIDE_MID;
	if(m_Rings > 2) cells += RING_SIDE_FAR * RING_SIDE_FAR;
	if(m_Rings > 3) cells += RING_SIDE_WHEAT * RING_SIDE_WHEAT;
	vkCmdDispatch(cmd, (cells + 63) / 64, 1, 1);

	vkutil::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
				  VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

void Foliage::draw(VkCommandBuffer cmd, VkDescriptorSet atmosphereSet, VkDescriptorSet shadowSet) {
	if(!m_Enabled) return;

	// depth prepass: wheat fields and meadows are layers upon layers of cards, this way each pixel gets the
	// expensive shading (shadows, atmosphere) once
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_PrepassPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ShadowPipelineLayout, 0, 1, &m_RenderSet, 0, nullptr);
	vkCmdPushConstants(cmd, m_ShadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FoliagePushConstants), &m_PushConstants);
	vkCmdDrawIndirect(cmd, m_Indirect.buffer, 0, 1, sizeof(VkDrawIndirectCommand));
	vkCmdDrawIndirect(cmd, m_Indirect.buffer, sizeof(VkDrawIndirectCommand) * 2, 1, sizeof(VkDrawIndirectCommand)); // the near ring

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_RenderPipeline);
	VkDescriptorSet sets[] = { m_RenderSet, atmosphereSet, shadowSet };
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_RenderPipelineLayout, 0, 3, sets, 0, nullptr);
	vkCmdPushConstants(cmd, m_RenderPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FoliagePushConstants), &m_PushConstants);
	vkCmdDrawIndirect(cmd, m_Indirect.buffer, 0, 1, sizeof(VkDrawIndirectCommand));
	vkCmdDrawIndirect(cmd, m_Indirect.buffer, sizeof(VkDrawIndirectCommand) * 2, 1, sizeof(VkDrawIndirectCommand)); // the near ring
}

void Foliage::drawShadow(VkCommandBuffer cmd, const glm::mat4& lightViewProj) {
	if(!m_Enabled || !m_CastShadows) return;
	FoliagePushConstants pc = m_PushConstants;
	pc.viewproj = lightViewProj;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ShadowPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ShadowPipelineLayout, 0, 1, &m_RenderSet, 0, nullptr);
	vkCmdPushConstants(cmd, m_ShadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FoliagePushConstants), &pc);
	vkCmdDrawIndirect(cmd, m_Indirect.buffer, sizeof(VkDrawIndirectCommand), 1, sizeof(VkDrawIndirectCommand));
}

void Foliage::drawImGui() {
	ImGui::Checkbox("Foliage", &m_Enabled);
}
