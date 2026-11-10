#include "trees.hpp"
#include "gbuffer.hpp"
#include "application.hpp"
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <fstream>
#include <functional>

// ------------------------------------------------------------------ meshes

// assets/trees/trees.bin (tools/import_trees.py): a header, every mesh's LODs (first index + index count), the
// vertices (TreeVertex) and the indices. the meshes go in TREE_MESH_FILE_INDEX order (per pack, the young ones first)
bool Trees::loadMeshes(std::vector<TreeVertex>& vertices, std::vector<uint32_t>& indices) {
	std::ifstream file(util::getpath("assets\\trees\\trees.bin"), std::ios::binary);
	struct Header { char magic[4]; uint32_t meshes, lods, vertices, indices; } header{};
	if(!file.read(reinterpret_cast<char*>(&header), sizeof(header)) || std::string(header.magic, 4) != "TRE1" || header.meshes != MESHES || header.lods != LODS) {
		fmt::print("trees: assets/trees/trees.bin is missing or doesn't match treemeshes.h (re-run tools/import_trees.py)\n");
		return false;
	}
	std::vector<Mesh> table(size_t(MESHES) * LODS);
	file.read(reinterpret_cast<char*>(table.data()), table.size() * sizeof(Mesh));
	vertices.resize(header.vertices);
	indices.resize(header.indices);
	file.read(reinterpret_cast<char*>(vertices.data()), vertices.size() * sizeof(TreeVertex));
	file.read(reinterpret_cast<char*>(indices.data()), indices.size() * sizeof(uint32_t));
	if(!file) {
		fmt::print("trees: assets/trees/trees.bin is truncated\n");
		return false;
	}
	for(uint32_t mesh = 0; mesh < MESHES; mesh++)
		for(uint32_t lod = 0; lod < LODS; lod++) m_Meshes[mesh][lod] = table[size_t(TREE_MESH_FILE_INDEX[mesh]) * LODS + lod];
	return true;
}

uint32_t Trees::binOffset(uint32_t bin) {
	// LOD0, LOD1, LOD2, shadow: MESHES bins each, then the impostor pool (treecommon.glsl binOffset)
	uint32_t offset = 0;
	for(uint32_t group = 0; group <= LODS; group++) {
		if(bin < (group + 1) * MESHES) return offset + (bin - group * MESHES) * CAPACITY[group];
		offset += MESHES * CAPACITY[group];
	}
	return offset;
}

// ------------------------------------------------------------------ vulkan

namespace {
	VkDescriptorSetLayout storageLayout(VkDevice device, std::initializer_list<VkDescriptorType> types, VkShaderStageFlags stages) {
		DescriptorLayoutBuilder builder;
		uint32_t binding = 0;
		for(VkDescriptorType type : types) builder.add_binding(binding++, type);
		return builder.build(device, stages);
	}

	void writeSet(VkDevice device, VkDescriptorSet set, uint32_t binding, VkDescriptorType type, const VkDescriptorImageInfo* image, const VkDescriptorBufferInfo* buffer, uint32_t count = 1) {
		VkWriteDescriptorSet write{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		write.dstSet = set;
		write.dstBinding = binding;
		write.descriptorCount = count;
		write.descriptorType = type;
		write.pImageInfo = image;
		write.pBufferInfo = buffer;
		vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
	}

	VkPipelineLayout pipelineLayout(VkDevice device, std::initializer_list<VkDescriptorSetLayout> sets, VkShaderStageFlags pushStages) {
		std::vector<VkDescriptorSetLayout> layouts(sets);
		VkPushConstantRange range{ pushStages, 0, sizeof(FoliagePushConstants) };
		VkPipelineLayoutCreateInfo layoutCI = vkinit::pipeline_layout_create_info();
		layoutCI.setLayoutCount = static_cast<uint32_t>(layouts.size());
		layoutCI.pSetLayouts = layouts.data();
		layoutCI.pushConstantRangeCount = 1;
		layoutCI.pPushConstantRanges = &range;
		VkPipelineLayout layout;
		VK_CHECK(vkCreatePipelineLayout(device, &layoutCI, nullptr, &layout));
		return layout;
	}

	AllocatedImage createAtlas(Application* engine, VkFormat format, VkImageUsageFlags usage, uint32_t width, uint32_t height, VkImageAspectFlags aspect) {
		AllocatedImage image;
		image.imageFormat = format;
		image.imageExtent = { width, height, 1 };
		VkImageCreateInfo imageCI = vkinit::image_create_info(format, usage, image.imageExtent);
		VmaAllocationCreateInfo allocCI = {};
		allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		allocCI.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
		VK_CHECK(vmaCreateImage(engine->getAllocator(), &imageCI, &allocCI, &image.image, &image.allocation, nullptr));
		VkImageViewCreateInfo viewCI = vkinit::imageview_create_info(format, image.image, aspect);
		VK_CHECK(vkCreateImageView(engine->m_Device, &viewCI, nullptr, &image.imageView));
		return image;
	}
}

void Trees::init(Application* engine, TextureLoader& textures, const AllocatedImage& heightmap, const AllocatedImage& climateMap, VkSampler heightmapSampler,
				 VkSampler textureSampler, VkDescriptorSetLayout atmosphereLayout, VkDescriptorSetLayout shadowLayout, VkFormat shadowFormat, VkBuffer interactionBuffer) {
	m_Device = engine->m_Device;
	m_Allocator = engine->getAllocator();
	m_TextureLoader = &textures;

	// meshes
	std::vector<TreeVertex> vertices;
	std::vector<uint32_t> indices;
	if(!loadMeshes(vertices, indices)) {
		// nothing to draw (keep the buffers valid): the bins' index counts stay 0
		vertices.assign(3, TreeVertex{});
		indices = { 0, 1, 2 };
		m_Enabled = false;
	}
	// every pack's bark (colour, normal) and leaf atlas (colour + coverage, normal), TREE_TEXTURES in the shaders
	for(uint32_t pack = 0; pack < TREE_PACKS; pack++) {
		std::string base = std::string("assets\\trees\\") + TREE_PACK_FOLDERS[pack] + "\\";
		m_Textures[pack * 4 + 0] = textures.loadCompressed(base + "bark.png", engine, TextureKind::Albedo);
		m_Textures[pack * 4 + 1] = textures.loadCompressed(base + "bark_normal.png", engine, TextureKind::Normal);
		m_Textures[pack * 4 + 2] = textures.loadCompressed(base + "leaves.png", engine, TextureKind::Atlas);
		m_Textures[pack * 4 + 3] = textures.loadCompressed(base + "leaves_normal.png", engine, TextureKind::Normal);
	}

	size_t vertexBytes = vertices.size() * sizeof(TreeVertex), indexBytes = indices.size() * sizeof(uint32_t);
	m_VertexBuffer = engine->createBuffer(vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	m_IndexBuffer = engine->createBuffer(indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	AllocatedBuffer staging = engine->createBuffer(vertexBytes + indexBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	memcpy(staging.info.pMappedData, vertices.data(), vertexBytes);
	memcpy(static_cast<uint8_t*>(staging.info.pMappedData) + vertexBytes, indices.data(), indexBytes);
	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		VkBufferCopy vertexCopy{ 0, 0, vertexBytes };
		VkBufferCopy indexCopy{ vertexBytes, 0, indexBytes };
		vkCmdCopyBuffer(cmd, staging.buffer, m_VertexBuffer.buffer, 1, &vertexCopy);
		vkCmdCopyBuffer(cmd, staging.buffer, m_IndexBuffer.buffer, 1, &indexCopy);
	});
	engine->destroyBuffer(staging);
	fmt::print("trees: {} vertices, {} triangles\n", vertices.size(), indices.size() / 3);

	// buffers: placed trees (persistent), their count, the per frame bins + their draw commands
	VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	m_Placed = engine->createBuffer(MAX_PLACED * sizeof(glm::vec4) * 2, storage, VMA_MEMORY_USAGE_GPU_ONLY);
	m_PlacedCount = engine->createBuffer(sizeof(uint32_t), storage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	m_OcclusionBuffer = engine->createBuffer(sizeof(OcclusionData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	m_Instances = engine->createBuffer((MAX_INSTANCES + 1) * sizeof(glm::vec4) * 2, storage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY); // + the impostor bake
	m_Commands = engine->createBuffer(BINS * sizeof(VkDrawIndexedIndirectCommand), storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	m_ImpostorCommands = engine->createBuffer(sizeof(VkDrawIndirectCommand), storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

	m_CommandTemplate.resize(BINS);
	for(uint32_t bin = 0; bin < BINS; bin++) {
		uint32_t mesh = bin % MESHES;
		uint32_t lod = std::min(bin / MESHES, LODS - 1); // the shadow casters use the last LOD
		m_CommandTemplate[bin] = { m_Meshes[mesh][lod].indexCount, 0, m_Meshes[mesh][lod].firstIndex, 0, binOffset(bin) };
	}
	m_ImpostorTemplate = { { 6, 0, 0, binOffset(BINS) } }; // one draw, every mesh's impostors

	// descriptor sets
	const VkDescriptorType SAMPLER = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SSBO = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	m_PlaceLayout = storageLayout(m_Device, { SAMPLER, SSBO, SSBO, SAMPLER }, VK_SHADER_STAGE_COMPUTE_BIT); // + the climate map
	m_ComputeLayout = storageLayout(m_Device, { SSBO, SSBO, SSBO, SSBO, SSBO, SAMPLER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER }, VK_SHADER_STAGE_COMPUTE_BIT); // + depth pyramid, occlusion
	DescriptorLayoutBuilder renderBuilder;
	renderBuilder.add_binding(0, SSBO);
	renderBuilder.add_binding(1, SAMPLER, TREE_TEXTURES);
	renderBuilder.add_binding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER); // the player's trail (interaction.glsl)
	m_RenderLayout = renderBuilder.build(m_Device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	m_ImpostorLayout = storageLayout(m_Device, { SSBO, SAMPLER, SAMPLER }, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

	m_PlaceSet = engine->g_DescriptorAllocator.allocate(m_Device, m_PlaceLayout);
	m_ComputeSet = engine->g_DescriptorAllocator.allocate(m_Device, m_ComputeLayout);
	m_RenderSet = engine->g_DescriptorAllocator.allocate(m_Device, m_RenderLayout);
	m_ImpostorSet = engine->g_DescriptorAllocator.allocate(m_Device, m_ImpostorLayout);

	setHeightmap(heightmap, climateMap, heightmapSampler); // place set bindings 0 and 3
	VkDescriptorBufferInfo placedInfo{ m_Placed.buffer, 0, VK_WHOLE_SIZE }, countInfo{ m_PlacedCount.buffer, 0, VK_WHOLE_SIZE };
	VkDescriptorBufferInfo instanceInfo{ m_Instances.buffer, 0, VK_WHOLE_SIZE }, commandInfo{ m_Commands.buffer, 0, VK_WHOLE_SIZE };
	VkDescriptorBufferInfo impostorCommandInfo{ m_ImpostorCommands.buffer, 0, VK_WHOLE_SIZE };
	writeSet(m_Device, m_PlaceSet, 1, SSBO, nullptr, &placedInfo);
	writeSet(m_Device, m_PlaceSet, 2, SSBO, nullptr, &countInfo);
	writeSet(m_Device, m_ComputeSet, 0, SSBO, nullptr, &instanceInfo);
	writeSet(m_Device, m_ComputeSet, 1, SSBO, nullptr, &commandInfo);
	writeSet(m_Device, m_ComputeSet, 2, SSBO, nullptr, &placedInfo);
	writeSet(m_Device, m_ComputeSet, 3, SSBO, nullptr, &countInfo);
	writeSet(m_Device, m_ComputeSet, 4, SSBO, nullptr, &impostorCommandInfo);
	VkDescriptorBufferInfo occlusionInfo{ m_OcclusionBuffer.buffer, 0, VK_WHOLE_SIZE };
	writeSet(m_Device, m_ComputeSet, 6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &occlusionInfo);
	VkDescriptorImageInfo textureInfos[TREE_TEXTURES];
	for(uint32_t i = 0; i < TREE_TEXTURES; i++) textureInfos[i] = { textureSampler, m_Textures[i].imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	writeSet(m_Device, m_RenderSet, 0, SSBO, nullptr, &instanceInfo);
	writeSet(m_Device, m_RenderSet, 1, SAMPLER, textureInfos, nullptr, TREE_TEXTURES);
	VkDescriptorBufferInfo interactionInfo{ interactionBuffer, 0, VK_WHOLE_SIZE };
	writeSet(m_Device, m_RenderSet, 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &interactionInfo);

	// pipelines
	m_Place = engine->loadComputeShader("src/shaders/bin/trees_place.comp.spv", "tree placement", pipelineLayout(m_Device, { m_PlaceLayout }, VK_SHADER_STAGE_COMPUTE_BIT));
	m_Scatter = engine->loadComputeShader("src/shaders/bin/trees.comp.spv", "tree culling", pipelineLayout(m_Device, { m_ComputeLayout }, VK_SHADER_STAGE_COMPUTE_BIT));
	const VkShaderStageFlags graphicsStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	m_RenderPipelineLayout = pipelineLayout(m_Device, { m_RenderLayout, atmosphereLayout, shadowLayout }, graphicsStages);
	m_ShadowPipelineLayout = pipelineLayout(m_Device, { m_RenderLayout }, graphicsStages);
	m_ImpostorPipelineLayout = pipelineLayout(m_Device, { m_ImpostorLayout, atmosphereLayout, shadowLayout }, graphicsStages);

	VkVertexInputBindingDescription binding{ 0, sizeof(TreeVertex), VK_VERTEX_INPUT_RATE_VERTEX };
	std::vector<VkVertexInputAttributeDescription> attributes = {
		{ 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TreeVertex, positionWind) },
		{ 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TreeVertex, normalMaterial) },
		{ 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TreeVertex, uvAO) },
	};

	CompiledShader vertexShader = engine->loadShader("src/shaders/bin/trees.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
	CompiledShader fragmentShader = engine->loadShader("src/shaders/bin/trees.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
	CompiledShader shadowShader = engine->loadShader("src/shaders/bin/trees_shadow.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
	CompiledShader prepassShader = engine->loadShader("src/shaders/bin/trees_prepass.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
	CompiledShader bakeShader = engine->loadShader("src/shaders/bin/trees_bake.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
	CompiledShader impostorVertex = engine->loadShader("src/shaders/bin/impostor.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
	CompiledShader impostorFragment = engine->loadShader("src/shaders/bin/impostor.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
	CompiledShader impostorPrepass = engine->loadShader("src/shaders/bin/impostor_prepass.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);

	auto meshPipeline = [&](VkPipelineLayout layout, VkShaderModule fragment) {
		PipelineBuilder builder;
		builder._pipelineLayout = layout;
		builder.set_shaders(vertexShader.shader, fragment);
		builder.set_vertex_input({ binding }, attributes);
		builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
		builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
		builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE); // leaf cards are two sided
		builder.set_multisampling_none();
		builder.disable_blending();
		return builder;
	};

	// depth prepass (alpha test only, no colour), then the shading only where its depth matches exactly
	PipelineBuilder prepassBuilder = meshPipeline(m_ShadowPipelineLayout, prepassShader.shader);
	prepassBuilder.enable_depthtest(true, VK_COMPARE_OP_GREATER); // reversed z
	setGBufferTargets(prepassBuilder, false);
	prepassBuilder._colorBlendAttachment.colorWriteMask = 0;
	prepassBuilder.set_depth_format(engine->m_DepthBuffer.imageFormat);
	m_PrepassPipeline = prepassBuilder.build_pipeline(m_Device);

	PipelineBuilder renderBuilderPipeline = meshPipeline(m_RenderPipelineLayout, fragmentShader.shader);
	renderBuilderPipeline.enable_depthtest(false, VK_COMPARE_OP_EQUAL); // after the prepass
	setGBufferTargets(renderBuilderPipeline, true); // deferred, lit in lighting.comp
	renderBuilderPipeline.set_depth_format(engine->m_DepthBuffer.imageFormat);
	m_RenderPipeline = renderBuilderPipeline.build_pipeline(m_Device);

	PipelineBuilder shadowBuilder = meshPipeline(m_ShadowPipelineLayout, shadowShader.shader);
	shadowBuilder._rasterizer.depthBiasEnable = VK_TRUE;
	shadowBuilder._rasterizer.depthBiasConstantFactor = 2.0f;
	shadowBuilder._rasterizer.depthBiasSlopeFactor = 2.5f;
	shadowBuilder.enable_depthtest(true, VK_COMPARE_OP_LESS); // shadow maps are regular z
	shadowBuilder.set_depth_format(shadowFormat);
	m_ShadowPipeline = shadowBuilder.build_pipeline(m_Device);

	// depth + alpha test first (with the trees' prepass), then the shading only where its depth matches
	auto impostorPipeline = [&](VkShaderModule fragment, bool prepass) {
		PipelineBuilder builder;
		builder._pipelineLayout = m_ImpostorPipelineLayout;
		builder.set_shaders(impostorVertex.shader, fragment);
		builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
		builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
		builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
		builder.set_multisampling_none();
		builder.disable_blending();
		builder.enable_depthtest(prepass, prepass ? VK_COMPARE_OP_GREATER : VK_COMPARE_OP_EQUAL);
		setGBufferTargets(builder, !prepass);
		builder.set_depth_format(engine->m_DepthBuffer.imageFormat);
		return builder.build_pipeline(m_Device);
	};
	m_ImpostorPrepassPipeline = impostorPipeline(impostorPrepass.shader, true);
	m_ImpostorPipeline = impostorPipeline(impostorFragment.shader, false);

	bakeImpostors(engine, meshPipeline, bakeShader.shader);

	VkDescriptorImageInfo albedoInfo{ textureSampler, m_ImpostorAlbedo.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkDescriptorImageInfo normalInfo{ textureSampler, m_ImpostorNormal.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	writeSet(m_Device, m_ImpostorSet, 0, SSBO, nullptr, &instanceInfo);
	writeSet(m_Device, m_ImpostorSet, 1, SAMPLER, &albedoInfo, nullptr);
	writeSet(m_Device, m_ImpostorSet, 2, SAMPLER, &normalInfo, nullptr);

	for(CompiledShader* shader : { &vertexShader, &fragmentShader, &shadowShader, &bakeShader, &impostorVertex, &impostorFragment, &impostorPrepass })
		vkDestroyShaderModule(m_Device, shader->shader, nullptr);
}

void Trees::setHeightmap(const AllocatedImage& heightmap, const AllocatedImage& climateMap, VkSampler heightmapSampler) {
	VkDescriptorImageInfo heightmapInfo{ heightmapSampler, heightmap.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

	VkDescriptorImageInfo climateInfo{ heightmapSampler, climateMap.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	writeSet(m_Device, m_PlaceSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &heightmapInfo, nullptr);
	writeSet(m_Device, m_PlaceSet, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &climateInfo, nullptr);

}

// renders every mesh from 3 elevations (0, 35 and 70 degrees) into an atlas: a grid of IMPOSTOR_COLUMNS meshes
// across, a column of the elevations each. colour + coverage and normals (in the bake view's space) + AO
void Trees::bakeImpostors(Application* engine, const std::function<PipelineBuilder(VkPipelineLayout, VkShaderModule)>& meshPipeline, VkShaderModule bakeShader) {
	const uint32_t cell = IMPOSTOR_CELL, width = cell * IMPOSTOR_COLUMNS, height = cell * IMPOSTOR_ELEVATIONS * IMPOSTOR_GROUPS;
	VkExtent2D size = { width, height };
	m_ImpostorAlbedo = createAtlas(engine, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, width, height, VK_IMAGE_ASPECT_COLOR_BIT);
	m_ImpostorNormal = createAtlas(engine, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, width, height, VK_IMAGE_ASPECT_COLOR_BIT);
	AllocatedImage depth = createAtlas(engine, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, width, height, VK_IMAGE_ASPECT_DEPTH_BIT);

	PipelineBuilder builder = meshPipeline(m_ShadowPipelineLayout, bakeShader); // set 0 only, like the shadows
	VkFormat formats[2] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
	builder._renderInfo.colorAttachmentCount = 2;
	builder._renderInfo.pColorAttachmentFormats = formats;
	builder.enable_depthtest(true, VK_COMPARE_OP_LESS);
	builder.set_depth_format(VK_FORMAT_D32_SFLOAT);
	// two attachments, so two blend states (the builder only has one)
	VkPipelineColorBlendAttachmentState blends[2] = { builder._colorBlendAttachment, builder._colorBlendAttachment };
	VkPipeline bakePipeline;
	{
		// build_pipeline uses one blend attachment, so build this one by hand from the builder's state
		VkPipelineViewportStateCreateInfo viewportState{ .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
		viewportState.viewportCount = 1;
		viewportState.scissorCount = 1;
		VkPipelineColorBlendStateCreateInfo colorBlending{ .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
		colorBlending.attachmentCount = 2;
		colorBlending.pAttachments = blends;
		VkDynamicState dynamics[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamicInfo{ .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
		dynamicInfo.dynamicStateCount = 2;
		dynamicInfo.pDynamicStates = dynamics;
		VkGraphicsPipelineCreateInfo pipelineInfo{ .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
		pipelineInfo.pNext = &builder._renderInfo;
		pipelineInfo.stageCount = static_cast<uint32_t>(builder._shaderStages.size());
		pipelineInfo.pStages = builder._shaderStages.data();
		builder._vertexInputInfo.vertexBindingDescriptionCount = static_cast<uint32_t>(builder._vertexBindings.size());
		builder._vertexInputInfo.pVertexBindingDescriptions = builder._vertexBindings.data();
		builder._vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(builder._vertexAttributes.size());
		builder._vertexInputInfo.pVertexAttributeDescriptions = builder._vertexAttributes.data();
		pipelineInfo.pVertexInputState = &builder._vertexInputInfo;
		pipelineInfo.pInputAssemblyState = &builder._inputAssembly;
		pipelineInfo.pViewportState = &viewportState;
		pipelineInfo.pRasterizationState = &builder._rasterizer;
		pipelineInfo.pMultisampleState = &builder._multisampling;
		pipelineInfo.pColorBlendState = &colorBlending;
		pipelineInfo.pDepthStencilState = &builder._depthStencil;
		pipelineInfo.pDynamicState = &dynamicInfo;
		pipelineInfo.layout = m_ShadowPipelineLayout;
		VK_CHECK(vkCreateGraphicsPipelines(m_Device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &bakePipeline));
	}

	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		// the tree to bake: at the origin, height 1, no rotation (the slot after the bins)
		glm::vec4 instance[2] = { glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f) };
		vkCmdUpdateBuffer(cmd, m_Instances.buffer, MAX_INSTANCES * sizeof(glm::vec4) * 2, sizeof(instance), instance);
		VkMemoryBarrier2 barrier{ .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
		barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
		barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
		barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
		VkDependencyInfo dependency{ .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
		dependency.memoryBarrierCount = 1;
		dependency.pMemoryBarriers = &barrier;
		vkCmdPipelineBarrier2(cmd, &dependency);

		vkutil::transition_image(cmd, m_ImpostorAlbedo.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		vkutil::transition_image(cmd, m_ImpostorNormal.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		vkutil::transition_image(cmd, depth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

		VkClearValue clear{}; // transparent black, coverage 0
		VkClearValue depthClear{}; depthClear.depthStencil.depth = 1.0f;
		VkRenderingAttachmentInfo colorAttachments[2] = {
			vkinit::attachment_info(m_ImpostorAlbedo.imageView, &clear, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
			vkinit::attachment_info(m_ImpostorNormal.imageView, &clear, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
		};
		VkRenderingAttachmentInfo depthAttachment = vkinit::attachment_info(depth.imageView, &depthClear, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
		VkRenderingInfo renderInfo = vkinit::rendering_info(size, colorAttachments, &depthAttachment);
		renderInfo.colorAttachmentCount = 2;
		vkCmdBeginRendering(cmd, &renderInfo);

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bakePipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ShadowPipelineLayout, 0, 1, &m_RenderSet, 0, nullptr);
		VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &m_VertexBuffer.buffer, &offset);
		vkCmdBindIndexBuffer(cmd, m_IndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

		// orthographic views of the unit tree (height 0..1) around its centre, from the side up to 70 degrees
		glm::vec3 center(0.0f, 0.5f, 0.0f);
		for(uint32_t row = 0; row < IMPOSTOR_ELEVATIONS; row++) {
			float elevation = glm::radians(IMPOSTOR_ELEVATION_STEP * row);
			glm::vec3 toViewer(0.0f, std::sin(elevation), std::cos(elevation));
			FoliagePushConstants pc{};
			pc.viewproj = glm::ortho(-0.6f, 0.6f, -0.6f, 0.6f, 0.0f, 6.0f) * glm::lookAt(center + toViewer * 3.0f, center, glm::vec3(0.0f, 1.0f, 0.0f));
			pc.viewproj = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, -1.0f, 1.0f)) * pc.viewproj; // atlas rows go down, tree tops at the top
			pc.campos = center + toViewer * 1000.0f;
			pc.sundir = toViewer; // trees_bake.frag stores normals relative to this view
			vkCmdPushConstants(cmd, m_ShadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FoliagePushConstants), &pc);

			for(uint32_t mesh = 0; mesh < MESHES; mesh++) {
				// treecommon.glsl impostorCell: IMPOSTOR_COLUMNS meshes across, each a column of the elevations
				uint32_t x = (mesh % IMPOSTOR_COLUMNS) * cell, y = ((mesh / IMPOSTOR_COLUMNS) * IMPOSTOR_ELEVATIONS + row) * cell;
				VkViewport viewport = { static_cast<float>(x), static_cast<float>(y), static_cast<float>(cell), static_cast<float>(cell), 0.0f, 1.0f };
				VkRect2D scissor = { { static_cast<int32_t>(x), static_cast<int32_t>(y) }, { cell, cell } };
				vkCmdSetViewport(cmd, 0, 1, &viewport);
				vkCmdSetScissor(cmd, 0, 1, &scissor);
				vkCmdDrawIndexed(cmd, m_Meshes[mesh][0].indexCount, 1, m_Meshes[mesh][0].firstIndex, 0, MAX_INSTANCES);
			}
		}
		vkCmdEndRendering(cmd);

		vkutil::transition_image(cmd, m_ImpostorAlbedo.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		vkutil::transition_image(cmd, m_ImpostorNormal.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	});

	vkDestroyPipeline(m_Device, bakePipeline, nullptr);
	vkDestroyImageView(m_Device, depth.imageView, nullptr);
	vmaDestroyImage(m_Allocator, depth.image, depth.allocation);
}

void Trees::destroy() {
	vkDestroyPipeline(m_Device, m_RenderPipeline, nullptr);
	vkDestroyPipeline(m_Device, m_ShadowPipeline, nullptr);
	vkDestroyPipeline(m_Device, m_PrepassPipeline, nullptr);
	vkDestroyPipeline(m_Device, m_ImpostorPipeline, nullptr);
	vkDestroyPipeline(m_Device, m_ImpostorPrepassPipeline, nullptr);
	for(VkPipelineLayout layout : { m_RenderPipelineLayout, m_ShadowPipelineLayout, m_ImpostorPipelineLayout, m_Scatter.layout, m_Place.layout })
		vkDestroyPipelineLayout(m_Device, layout, nullptr);
	for(VkDescriptorSetLayout layout : { m_PlaceLayout, m_ComputeLayout, m_RenderLayout, m_ImpostorLayout })
		vkDestroyDescriptorSetLayout(m_Device, layout, nullptr);
	for(AllocatedBuffer* buffer : { &m_VertexBuffer, &m_IndexBuffer, &m_Instances, &m_Commands, &m_ImpostorCommands, &m_Placed, &m_PlacedCount, &m_OcclusionBuffer })
		vmaDestroyBuffer(m_Allocator, buffer->buffer, buffer->allocation);
	for(const AllocatedImage& image : m_Textures) m_TextureLoader->destroyImage(image);
	for(AllocatedImage* image : { &m_ImpostorAlbedo, &m_ImpostorNormal }) {
		vkDestroyImageView(m_Device, image->imageView, nullptr);
		vmaDestroyImage(m_Allocator, image->image, image->allocation);
	}
}

void Trees::setOcclusionSource(VkImageView pyramid, VkSampler sampler) {
	VkDescriptorImageInfo info{ sampler, pyramid, VK_IMAGE_LAYOUT_GENERAL };
	writeSet(m_Device, m_ComputeSet, 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &info, nullptr);
}

void Trees::update(VkCommandBuffer cmd, const FoliagePushConstants& pc) {
	m_PushConstants = pc;
	m_PushConstants.extra = m_Density;
	m_PushConstants.sundir.y = m_DetailDistance; // trees.comp: the impostor distance (the draws only read .x)
	if(!m_Enabled) return;

	const VkPipelineStageFlags2 drawStages = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
	const VkAccessFlags2 drawReads = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
	const VkAccessFlags2 computeAccess = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

	// last frame may still be drawing from these
	vkutil::memoryBarrier(cmd, drawStages | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, drawReads | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
				VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT | computeAccess);

	// re-place once the camera moved far enough (placement covers a much bigger area than what's needed)
	glm::vec2 camWorld = glm::vec2(pc.campos.x, pc.campos.z) + pc.offsetWorld;
	bool replace = m_ForcePlace || glm::distance(camWorld, m_LastPlaceCenter) > PLACE_STEP || m_Density != m_LastDensity;
	if(replace) {
		m_ForcePlace = false;
		m_LastPlaceCenter = camWorld;
		m_LastDensity = m_Density;
		uint32_t zero = 0;
		vkCmdUpdateBuffer(cmd, m_PlacedCount.buffer, 0, sizeof(zero), &zero);
	}

	vkCmdUpdateBuffer(cmd, m_Commands.buffer, 0, m_CommandTemplate.size() * sizeof(VkDrawIndexedIndirectCommand), m_CommandTemplate.data());
	vkCmdUpdateBuffer(cmd, m_ImpostorCommands.buffer, 0, m_ImpostorTemplate.size() * sizeof(VkDrawIndirectCommand), m_ImpostorTemplate.data());
	vkCmdUpdateBuffer(cmd, m_OcclusionBuffer.buffer, 0, sizeof(OcclusionData), &m_Occlusion);
vkutil::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | drawStages, computeAccess | drawReads);

	if(replace) {
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Place.pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Place.layout, 0, 1, &m_PlaceSet, 0, nullptr);
		vkCmdPushConstants(cmd, m_Place.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FoliagePushConstants), &m_PushConstants);
		vkCmdDispatch(cmd, (PLACE_GRID_SIDE * PLACE_GRID_SIDE + 63) / 64, 1, 1);
		vkutil::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
	}

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Scatter.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Scatter.layout, 0, 1, &m_ComputeSet, 0, nullptr);
	vkCmdPushConstants(cmd, m_Scatter.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FoliagePushConstants), &m_PushConstants);
	vkCmdDispatch(cmd, (MAX_PLACED + 63) / 64, 1, 1); // threads past the placed count exit straight away

	vkutil::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, drawStages, drawReads);
}

// depth prepass: leaf cards overlap a lot, this way each pixel gets shaded once. recorded before the terrain's
// material pass so the ground behind the trunks and leaves is rejected by the depth test instead of shaded
void Trees::drawPrepass(VkCommandBuffer cmd) {
	if(!m_Enabled) return;
	VkDeviceSize vertexOffset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &m_VertexBuffer.buffer, &vertexOffset);
	vkCmdBindIndexBuffer(cmd, m_IndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_PrepassPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ShadowPipelineLayout, 0, 1, &m_RenderSet, 0, nullptr);
	vkCmdPushConstants(cmd, m_ShadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FoliagePushConstants), &m_PushConstants);
	vkCmdDrawIndexedIndirect(cmd, m_Commands.buffer, 0, LODS * MESHES, sizeof(VkDrawIndexedIndirectCommand)); // every LOD's bins (multiDrawIndirect)

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ImpostorPrepassPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ImpostorPipelineLayout, 0, 1, &m_ImpostorSet, 0, nullptr);
	vkCmdPushConstants(cmd, m_ImpostorPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FoliagePushConstants), &m_PushConstants);
	vkCmdDrawIndirect(cmd, m_ImpostorCommands.buffer, 0, 1, sizeof(VkDrawIndirectCommand));
}

void Trees::draw(VkCommandBuffer cmd, VkDescriptorSet atmosphereSet, VkDescriptorSet shadowSet, const std::function<void()>& split) {
	if(!m_Enabled) {
		if(split) split(); // the timers still need their boundary
		return;
	}
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_RenderPipeline);
	VkDescriptorSet sets[] = { m_RenderSet, atmosphereSet, shadowSet };
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_RenderPipelineLayout, 0, 3, sets, 0, nullptr);
	vkCmdPushConstants(cmd, m_RenderPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FoliagePushConstants), &m_PushConstants);
	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &m_VertexBuffer.buffer, &offset);
	vkCmdBindIndexBuffer(cmd, m_IndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
	vkCmdDrawIndexedIndirect(cmd, m_Commands.buffer, 0, LODS * MESHES, sizeof(VkDrawIndexedIndirectCommand));
	if(split) split();

	// far trees as impostors
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ImpostorPipeline);
	VkDescriptorSet impostorSets[] = { m_ImpostorSet, atmosphereSet, shadowSet };
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ImpostorPipelineLayout, 0, 3, impostorSets, 0, nullptr);
	vkCmdPushConstants(cmd, m_ImpostorPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FoliagePushConstants), &m_PushConstants);
	vkCmdDrawIndirect(cmd, m_ImpostorCommands.buffer, 0, 1, sizeof(VkDrawIndirectCommand));
}

void Trees::drawShadow(VkCommandBuffer cmd, const glm::mat4& lightViewProj) {
	if(!m_Enabled) return;
	FoliagePushConstants pc = m_PushConstants;
	pc.viewproj = lightViewProj;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ShadowPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ShadowPipelineLayout, 0, 1, &m_RenderSet, 0, nullptr);
	vkCmdPushConstants(cmd, m_ShadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FoliagePushConstants), &pc);
	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &m_VertexBuffer.buffer, &offset);
	vkCmdBindIndexBuffer(cmd, m_IndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
	vkCmdDrawIndexedIndirect(cmd, m_Commands.buffer, LODS * MESHES * sizeof(VkDrawIndexedIndirectCommand), MESHES, sizeof(VkDrawIndexedIndirectCommand));
}

void Trees::drawImGui() {
	ImGui::Checkbox("Trees", &m_Enabled);
	ImGui::SliderFloat("Tree density", &m_Density, 0.0f, 3.0f);
}
