#include "wildlife.hpp"
#include "application.hpp"
#include "gbuffer.hpp"

void Wildlife::init(Application* engine, const AllocatedImage& heightmap, const AllocatedImage& climateMap, VkSampler heightmapSampler) {
	m_Device = engine->m_Device;
	m_Allocator = engine->getAllocator();
	m_Critters = engine->createBuffer(static_cast<size_t>(BIRDS + BUTTERFLIES) * CRITTER_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // heightmap: how high to fly, where the ground is
	builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // climate map: meadows, forests, mountains
	builder.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);		   // the critters, posed for this frame
	m_Layout = builder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT);
	m_Set = engine->g_DescriptorAllocator.allocate(m_Device, m_Layout);

	VkDescriptorImageInfo infos[2] = {
		{ heightmapSampler, heightmap.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ heightmapSampler, climateMap.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
	};
	VkDescriptorBufferInfo critterInfo{ m_Critters.buffer, 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet writes[3] = {};
	for(uint32_t i = 0; i < 3; i++) {
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = m_Set;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		if(i < 2) writes[i].pImageInfo = &infos[i];
		else writes[i].pBufferInfo = &critterInfo;
	}
	vkUpdateDescriptorSets(m_Device, 3, writes, 0, nullptr);

	VkPushConstantRange range{ VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(WildlifePushConstants) };
	VkPipelineLayoutCreateInfo layoutCI = vkinit::pipeline_layout_create_info();
	layoutCI.setLayoutCount = 1;
	layoutCI.pSetLayouts = &m_Layout;
	layoutCI.pushConstantRangeCount = 1;
	layoutCI.pPushConstantRanges = &range;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &layoutCI, nullptr, &m_PipelineLayout));
	m_Pose = engine->loadComputeShader("src/shaders/bin/wildlife.comp.spv", "wildlife", m_PipelineLayout);

	CompiledShader vertexShader = engine->loadShader("src/shaders/bin/wildlife.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
	CompiledShader fragmentShader = engine->loadShader("src/shaders/bin/wildlife.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
	PipelineBuilder pipeline;
	pipeline._pipelineLayout = m_PipelineLayout;
	pipeline.set_shaders(vertexShader.shader, fragmentShader.shader);
	pipeline.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST); // no vertex input, the shapes are built in the shader
	pipeline.set_polygon_mode(VK_POLYGON_MODE_FILL);
	pipeline.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE); // wings are seen from both sides
	pipeline.set_multisampling_none();
	pipeline.disable_blending();
	pipeline.enable_depthtest(true, VK_COMPARE_OP_GREATER); // reversed z
	setGBufferTargets(pipeline, true);
	pipeline.set_depth_format(engine->m_DepthBuffer.imageFormat);
	m_Pipeline = pipeline.build_pipeline(m_Device);
	vkDestroyShaderModule(m_Device, vertexShader.shader, nullptr);
	vkDestroyShaderModule(m_Device, fragmentShader.shader, nullptr);
}

void Wildlife::update(VkCommandBuffer cmd, const WildlifePushConstants& pc) {
	m_PushConstants = pc;
	if(!m_Enabled) return;
	// the previous frame's draw may still be reading them
	vkutil::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Pose.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout, 0, 1, &m_Set, 0, nullptr);
	vkCmdPushConstants(cmd, m_PipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	vkCmdDispatch(cmd, (BIRDS + BUTTERFLIES + 63) / 64, 1, 1);
	vkutil::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
}

void Wildlife::draw(VkCommandBuffer cmd) {
	if(!m_Enabled) return;
	vkutil::bindGraphics(cmd, m_Pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_PipelineLayout, 0, 1, &m_Set, 0, nullptr);
	vkCmdPushConstants(cmd, m_PipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(m_PushConstants), &m_PushConstants);
	vkCmdDraw(cmd, BIRD_VERTICES, BIRDS + BUTTERFLIES, 0, 0); // the butterflies' instances after the birds'
}

void Wildlife::destroy() {
	vkDestroyPipeline(m_Device, m_Pipeline, nullptr);
	vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr);
	vkDestroyDescriptorSetLayout(m_Device, m_Layout, nullptr);
	vmaDestroyBuffer(m_Allocator, m_Critters.buffer, m_Critters.allocation);
}
