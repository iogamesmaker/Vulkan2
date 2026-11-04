#include "precipitation.hpp"
#include "application.hpp"

void Precipitation::init(Application* engine, VkFormat outputFormat, VkDescriptorSetLayout atmosphereLayout) {
	m_Device = engine->m_Device;

	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // depth buffer
	m_Layout = builder.build(m_Device, VK_SHADER_STAGE_FRAGMENT_BIT);
	m_Set = engine->g_DescriptorAllocator.allocate(m_Device, m_Layout);

	VkDescriptorSetLayout layouts[] = { m_Layout, atmosphereLayout };
	VkPushConstantRange range{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PrecipitationPushConstants) };
	VkPipelineLayoutCreateInfo layoutCI = vkinit::pipeline_layout_create_info();
	layoutCI.setLayoutCount = 2;
	layoutCI.pSetLayouts = layouts;
	layoutCI.pushConstantRangeCount = 1;
	layoutCI.pPushConstantRanges = &range;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &layoutCI, nullptr, &m_PipelineLayout));

	CompiledShader vertexShader = engine->loadShader("src/shaders/bin/precipitation.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
	CompiledShader fragmentShader = engine->loadShader("src/shaders/bin/precipitation.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
	PipelineBuilder pipeline;
	pipeline._pipelineLayout = m_PipelineLayout;
	pipeline.set_shaders(vertexShader.shader, fragmentShader.shader);
	pipeline.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST); // no vertex input: 6 corners per instance
	pipeline.set_polygon_mode(VK_POLYGON_MODE_FILL);
	pipeline.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	pipeline.set_multisampling_none();
	pipeline.enable_blending_alphablend();
	pipeline._colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT;
	pipeline.disable_depthtest(); // by hand, the depth buffer is a different size
	pipeline.set_color_attachment_format(outputFormat);
	pipeline.set_depth_format(VK_FORMAT_UNDEFINED);
	m_Pipeline = pipeline.build_pipeline(m_Device);
	vkDestroyShaderModule(m_Device, vertexShader.shader, nullptr);
	vkDestroyShaderModule(m_Device, fragmentShader.shader, nullptr);
}

void Precipitation::setDepth(VkImageView depth, VkSampler nearest) {
	VkDescriptorImageInfo info{ nearest, depth, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkWriteDescriptorSet write{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	write.dstSet = m_Set;
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &info;
	vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);
}

void Precipitation::draw(VkCommandBuffer cmd, const PrecipitationPushConstants& pc, VkDescriptorSet atmosphereSet, VkImageView output, VkExtent2D extent) {
	uint32_t instances = static_cast<uint32_t>(pc.counts.x + pc.counts.y + pc.counts.z);
	if(instances == 0) return;
	VkRenderingAttachmentInfo color = vkinit::attachment_info(output, nullptr, VK_IMAGE_LAYOUT_GENERAL);
	VkRenderingInfo renderInfo = vkinit::rendering_info(extent, &color, nullptr);
	vkCmdBeginRendering(cmd, &renderInfo);
	VkViewport viewport = { 0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height), 0.0f, 1.0f };
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	VkRect2D scissor = { { 0, 0 }, extent };
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_Pipeline);
	VkDescriptorSet sets[] = { m_Set, atmosphereSet };
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_PipelineLayout, 0, 2, sets, 0, nullptr);
	vkCmdPushConstants(cmd, m_PipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	vkCmdDraw(cmd, 6, instances, 0, 0);
	vkCmdEndRendering(cmd);
}

void Precipitation::destroy() {
	vkDestroyPipeline(m_Device, m_Pipeline, nullptr);
	vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr);
	vkDestroyDescriptorSetLayout(m_Device, m_Layout, nullptr);
}
