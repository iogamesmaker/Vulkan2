#include "groundcover.hpp"
#include "application.hpp"
#include "gbuffer.hpp"
#include <algorithm>
#include <filesystem>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <glm/gtc/quaternion.hpp>

namespace {
	// in GC_* order (groundcovercommon.glsl). opaque: no cutout map, the plain albedo
	struct SpeciesFile { const char* name; bool opaque; };
	constexpr SpeciesFile SPECIES_FILES[] = {
		{ "fern_02", false }, { "dry_branches_medium_01", true }, { "shrub_02", false }, { "shrub_03", false }, { "nettle_plant", false },
		{ "periwinkle_plant", true }, { "shrub_sorrel_01", false }, { "weed_plant_02", false }, { "dandelion_01", false },
		{ "flower_gazania", false }, { "flower_ursinia", false }, { "flower_empodium", false }, { "flower_stinkkruid", false },
		{ "wild_rooibos_bush", false },
	};
	constexpr size_t MAX_TRIANGLES = 9600; // the dense variants (some dandelions, a periwinkle) cost more than they show

	glm::mat4 nodeMatrix(const fastgltf::Node& node) {
		// the rotation / scale only: the variants are laid out side by side in the file, each one goes to its origin
		glm::mat4 m(1.0f);
		if(const auto* trs = std::get_if<fastgltf::TRS>(&node.transform)) {
			glm::quat q(trs->rotation[3], trs->rotation[0], trs->rotation[1], trs->rotation[2]); // w, x, y, z
			m = glm::mat4_cast(q) * glm::scale(glm::mat4(1.0f), glm::vec3(trs->scale[0], trs->scale[1], trs->scale[2]));
		} else if(const auto* mat = std::get_if<fastgltf::math::fmat4x4>(&node.transform)) {
			for(int c = 0; c < 3; c++)
				for(int r = 0; r < 4; r++) m[c][r] = (*mat)[c][r];
		}
		return m;
	}
}

void GroundCover::init(Application* engine, TextureLoader& textures, const AllocatedImage& heightmap, const AllocatedImage& climateMap,
					   VkSampler heightmapSampler, VkSampler textureSampler, VkFormat shadowFormat, VkBuffer interactionBuffer) {
	m_Device = engine->m_Device;
	m_Allocator = engine->getAllocator();
	m_TextureLoader = &textures;
	static_assert(std::size(SPECIES_FILES) == SPECIES);

	// ---------------------------------------------------------------- the models
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
	Species species[SPECIES]{};
	const float toWorld = 1.0f / 6.0f; // metres -> world units (Camera::metersPerUnit)
	for(uint32_t s = 0; s < SPECIES; s++) {
		std::string base = std::string("assets\\plants\\") + SPECIES_FILES[s].name + "\\";
		species[s].meshes = glm::uvec4(static_cast<uint32_t>(m_Meshes.size()), 0, 0, 0);
		// textures: the cutout version of the albedo (its alpha merged in, *_diff_alpha_1k.png) or the plain one
		std::string albedo = base + "textures\\" + SPECIES_FILES[s].name + (SPECIES_FILES[s].opaque ? "_diff_1k.jpg" : "_diff_alpha_1k.png");
		m_Textures.push_back(textures.loadCompressed(albedo, engine, SPECIES_FILES[s].opaque ? TextureKind::Albedo : TextureKind::Atlas));
		m_Textures.push_back(textures.loadCompressed(base + "textures\\" + SPECIES_FILES[s].name + "_nor_gl_1k.jpg", engine, TextureKind::Normal));

		std::filesystem::path path = util::getpath(base + SPECIES_FILES[s].name + "_1k.gltf");
		auto data = fastgltf::GltfDataBuffer::FromPath(path);
		if(data.error() != fastgltf::Error::None) {
			fmt::print("ground cover: couldn't read {}\n", path.string());
			continue;
		}
		fastgltf::Parser parser{};
		auto asset = parser.loadGltf(data.get(), path.parent_path(), fastgltf::Options::LoadExternalBuffers);
		if(!asset) {
			fmt::print("ground cover: couldn't load {}\n", path.string());
			continue;
		}
		fastgltf::Asset& gltf = asset.get();
		for(const fastgltf::Node& node : gltf.nodes) {
			if(!node.meshIndex || m_Meshes.size() >= MAX_MESHES) continue;
			// every primitive of the node into one mesh (some plants are split into twigs / leaves / stems, all on
			// the same texture)
			const fastgltf::Mesh& gltfMesh = gltf.meshes[*node.meshIndex];
			size_t triangles = 0;
			bool usable = true;
			for(const fastgltf::Primitive& p : gltfMesh.primitives) {
				usable &= p.indicesAccessor && p.findAttribute("POSITION") != p.attributes.end() && p.findAttribute("NORMAL") != p.attributes.end()
						  && p.findAttribute("TEXCOORD_0") != p.attributes.end();
				if(p.indicesAccessor) triangles += gltf.accessors[*p.indicesAccessor].count / 3;
			}
			if(!usable || triangles > MAX_TRIANGLES) continue;

			glm::mat4 transform = nodeMatrix(node);
			size_t first = vertices.size();
			Mesh mesh;
			mesh.firstIndex = static_cast<uint32_t>(indices.size());
			mesh.vertexOffset = static_cast<int32_t>(first);
			for(const fastgltf::Primitive& p : gltfMesh.primitives) {
				size_t base = vertices.size();
				const fastgltf::Accessor& positions = gltf.accessors[p.findAttribute("POSITION")->accessorIndex];
				vertices.resize(base + positions.count);
				fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, positions, [&](glm::vec3 v, size_t i) {
					vertices[base + i].positionSpecies = glm::vec4(glm::vec3(transform * glm::vec4(v, 1.0f)) * toWorld, static_cast<float>(s));
				});
				fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, gltf.accessors[p.findAttribute("NORMAL")->accessorIndex], [&](glm::vec3 v, size_t i) {
					vertices[base + i].normalHeight = glm::vec4(glm::normalize(glm::mat3(transform) * v), 0.0f);
				});
				fastgltf::iterateAccessorWithIndex<glm::vec2>(gltf, gltf.accessors[p.findAttribute("TEXCOORD_0")->accessorIndex], [&](glm::vec2 v, size_t i) {
					vertices[base + i].uv = glm::vec4(v, 0.0f, 0.0f);
				});
				uint32_t offset = static_cast<uint32_t>(base - first); // indices are relative to the mesh's vertexOffset
				fastgltf::iterateAccessor<uint32_t>(gltf, gltf.accessors[*p.indicesAccessor], [&](uint32_t index) { indices.push_back(index + offset); });
			}
			mesh.indexCount = static_cast<uint32_t>(indices.size()) - mesh.firstIndex;
			// standing on its lowest point, the height up the plant for the wind and the AO
			float bottom = 1e9f, top = -1e9f, radius = 0.0f;
			for(size_t i = first; i < vertices.size(); i++) {
				bottom = std::min(bottom, vertices[i].positionSpecies.y);
				top = std::max(top, vertices[i].positionSpecies.y);
			}
			float ground = std::max(bottom, -0.05f * toWorld); // a few roots below 0 are fine, the variants stand at 0
			for(size_t i = first; i < vertices.size(); i++) {
				Vertex& v = vertices[i];
				v.normalHeight.w = glm::clamp((v.positionSpecies.y - ground) / std::max(top - ground, 1e-5f), 0.0f, 1.0f);
				radius = std::max(radius, glm::length(glm::vec3(v.positionSpecies)));
			}

			m_Meshes.push_back(mesh);
			species[s].meshes.y++;
			species[s].size.x = std::max(species[s].size.x, radius);
		}
		// lightest first: groundcover.comp lets the dense upper half reach less far
		std::sort(m_Meshes.begin() + species[s].meshes.x, m_Meshes.end(), [](const Mesh& a, const Mesh& b) { return a.indexCount < b.indexCount; });

	}
	if(vertices.empty()) vertices.resize(1), indices.resize(3, 0); // nothing found: keep the buffers valid

	size_t vertexBytes = vertices.size() * sizeof(Vertex), indexBytes = indices.size() * sizeof(uint32_t);
	m_VertexBuffer = engine->createBuffer(vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	m_IndexBuffer = engine->createBuffer(indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	m_SpeciesBuffer = engine->createBuffer(sizeof(species), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	AllocatedBuffer staging = engine->createBuffer(vertexBytes + indexBytes + sizeof(species), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	memcpy(staging.info.pMappedData, vertices.data(), vertexBytes);
	memcpy(static_cast<uint8_t*>(staging.info.pMappedData) + vertexBytes, indices.data(), indexBytes);
	memcpy(static_cast<uint8_t*>(staging.info.pMappedData) + vertexBytes + indexBytes, species, sizeof(species));
	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		VkBufferCopy vertexCopy{ 0, 0, vertexBytes };
		VkBufferCopy indexCopy{ vertexBytes, 0, indexBytes };
		VkBufferCopy speciesCopy{ vertexBytes + indexBytes, 0, sizeof(species) };
		vkCmdCopyBuffer(cmd, staging.buffer, m_VertexBuffer.buffer, 1, &vertexCopy);
		vkCmdCopyBuffer(cmd, staging.buffer, m_IndexBuffer.buffer, 1, &indexCopy);
		vkCmdCopyBuffer(cmd, staging.buffer, m_SpeciesBuffer.buffer, 1, &speciesCopy);
	});
	engine->destroyBuffer(staging);
	fmt::print("ground cover: {} meshes, {} vertices, {} triangles\n", m_Meshes.size(), vertices.size(), indices.size() / 3);

	// bins: one per mesh for the camera, then one per mesh for the shadow casters
	m_Instances = engine->createBuffer(BINS * CAPACITY * sizeof(glm::vec4) * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	m_Commands = engine->createBuffer(BINS * sizeof(VkDrawIndexedIndirectCommand), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	m_CommandTemplate.assign(BINS, VkDrawIndexedIndirectCommand{});
	for(uint32_t bin = 0; bin < BINS; bin++) {
		uint32_t mesh = bin % MAX_MESHES;
		m_CommandTemplate[bin].firstInstance = bin * CAPACITY;
		if(mesh >= m_Meshes.size()) continue;
		m_CommandTemplate[bin].indexCount = m_Meshes[mesh].indexCount;
		m_CommandTemplate[bin].firstIndex = m_Meshes[mesh].firstIndex;
		m_CommandTemplate[bin].vertexOffset = m_Meshes[mesh].vertexOffset;
	}

	// ---------------------------------------------------------------- descriptors
	DescriptorLayoutBuilder computeBuilder;
	computeBuilder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // heightmap
	computeBuilder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // climate map
	computeBuilder.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);		  // instances
	computeBuilder.add_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);		  // draw commands
	computeBuilder.add_binding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);		  // species
	m_ComputeLayout = computeBuilder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
	m_ComputeSet = engine->g_DescriptorAllocator.allocate(m_Device, m_ComputeLayout);

	DescriptorLayoutBuilder renderBuilder;
	renderBuilder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);							   // instances
	renderBuilder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SPECIES * 2); // albedo, normal per species
	renderBuilder.add_binding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER); // the player's trail + the weather's wind (interaction.glsl)
	m_RenderLayout = renderBuilder.build(m_Device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	m_RenderSet = engine->g_DescriptorAllocator.allocate(m_Device, m_RenderLayout);

	VkDescriptorImageInfo mapInfos[2] = {
		{ heightmapSampler, heightmap.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ heightmapSampler, climateMap.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
	};
	VkDescriptorBufferInfo instanceInfo{ m_Instances.buffer, 0, VK_WHOLE_SIZE };
	VkDescriptorBufferInfo commandInfo{ m_Commands.buffer, 0, VK_WHOLE_SIZE };
	VkDescriptorBufferInfo speciesInfo{ m_SpeciesBuffer.buffer, 0, VK_WHOLE_SIZE };
	VkDescriptorImageInfo textureInfos[SPECIES * 2];
	for(uint32_t i = 0; i < SPECIES * 2; i++) textureInfos[i] = { textureSampler, m_Textures[i].imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkDescriptorBufferInfo interactionInfo{ interactionBuffer, 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet writes[8] = {};
	for(auto& write : writes) { write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; write.descriptorCount = 1; write.dstSet = m_ComputeSet; write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; }
	writes[0].dstBinding = 0; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].pImageInfo = &mapInfos[0];
	writes[1].dstBinding = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[1].pImageInfo = &mapInfos[1];
	writes[2].dstBinding = 2; writes[2].pBufferInfo = &instanceInfo;
	writes[3].dstBinding = 3; writes[3].pBufferInfo = &commandInfo;
	writes[4].dstBinding = 4; writes[4].pBufferInfo = &speciesInfo;
	writes[5].dstSet = m_RenderSet; writes[5].dstBinding = 0; writes[5].pBufferInfo = &instanceInfo;
	writes[6].dstSet = m_RenderSet; writes[6].dstBinding = 1; writes[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[6].descriptorCount = SPECIES * 2; writes[6].pImageInfo = textureInfos;
	writes[7].dstSet = m_RenderSet; writes[7].dstBinding = 2; writes[7].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; writes[7].pBufferInfo = &interactionInfo;
	vkUpdateDescriptorSets(m_Device, 8, writes, 0, nullptr);

	// ---------------------------------------------------------------- pipelines
	VkPushConstantRange computeRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FoliagePushConstants) };
	VkPipelineLayoutCreateInfo computeLayoutCI = vkinit::pipeline_layout_create_info();
	computeLayoutCI.setLayoutCount = 1;
	computeLayoutCI.pSetLayouts = &m_ComputeLayout;
	computeLayoutCI.pushConstantRangeCount = 1;
	computeLayoutCI.pPushConstantRanges = &computeRange;
	VkPipelineLayout scatterLayout;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &computeLayoutCI, nullptr, &scatterLayout));
	m_Scatter = engine->loadComputeShader("src/shaders/bin/groundcover.comp.spv", "ground cover scatter", scatterLayout);

	VkPushConstantRange renderRange{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FoliagePushConstants) };
	VkPipelineLayoutCreateInfo renderLayoutCI = vkinit::pipeline_layout_create_info();
	renderLayoutCI.setLayoutCount = 1;
	renderLayoutCI.pSetLayouts = &m_RenderLayout;
	renderLayoutCI.pushConstantRangeCount = 1;
	renderLayoutCI.pPushConstantRanges = &renderRange;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &renderLayoutCI, nullptr, &m_RenderPipelineLayout));

	VkVertexInputBindingDescription binding{ 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
	std::vector<VkVertexInputAttributeDescription> attributes = {
		{ 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, positionSpecies) },
		{ 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, normalHeight) },
		{ 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, uv) },
	};
	CompiledShader vertexShader = engine->loadShader("src/shaders/bin/groundcover.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
	CompiledShader fragmentShader = engine->loadShader("src/shaders/bin/groundcover.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
	CompiledShader shadowShader = engine->loadShader("src/shaders/bin/groundcover_shadow.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);

	PipelineBuilder builder;
	builder._pipelineLayout = m_RenderPipelineLayout;
	builder.set_shaders(vertexShader.shader, fragmentShader.shader);
	builder.set_vertex_input({ binding }, attributes);
	builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
	builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE); // two sided leaves
	builder.set_multisampling_none();
	builder.disable_blending();
	builder.enable_depthtest(true, VK_COMPARE_OP_GREATER); // reversed z
	setGBufferTargets(builder, true);
	builder.set_depth_format(engine->m_DepthBuffer.imageFormat);
	m_RenderPipeline = builder.build_pipeline(m_Device);

	PipelineBuilder shadowBuilder;
	shadowBuilder._pipelineLayout = m_RenderPipelineLayout;
	shadowBuilder.set_shaders(vertexShader.shader, shadowShader.shader);
	shadowBuilder.set_vertex_input({ binding }, attributes);
	shadowBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	shadowBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
	shadowBuilder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE);
	shadowBuilder._rasterizer.depthBiasEnable = VK_TRUE;
	shadowBuilder._rasterizer.depthBiasConstantFactor = 2.0f;
	shadowBuilder._rasterizer.depthBiasSlopeFactor = 2.5f;
	shadowBuilder.set_multisampling_none();
	shadowBuilder.disable_blending();
	shadowBuilder.enable_depthtest(true, VK_COMPARE_OP_LESS); // shadow maps are regular z
	shadowBuilder.set_depth_format(shadowFormat);
	m_ShadowPipeline = shadowBuilder.build_pipeline(m_Device);

	vkDestroyShaderModule(m_Device, vertexShader.shader, nullptr);
	vkDestroyShaderModule(m_Device, fragmentShader.shader, nullptr);
	vkDestroyShaderModule(m_Device, shadowShader.shader, nullptr);
}

void GroundCover::destroy() {
	vkDestroyPipeline(m_Device, m_RenderPipeline, nullptr);
	vkDestroyPipeline(m_Device, m_ShadowPipeline, nullptr);
	vkDestroyPipelineLayout(m_Device, m_RenderPipelineLayout, nullptr);
	vkDestroyPipelineLayout(m_Device, m_Scatter.layout, nullptr);
	vkDestroyDescriptorSetLayout(m_Device, m_ComputeLayout, nullptr);
	vkDestroyDescriptorSetLayout(m_Device, m_RenderLayout, nullptr);
	for(AllocatedBuffer* buffer : { &m_VertexBuffer, &m_IndexBuffer, &m_Instances, &m_Commands, &m_SpeciesBuffer })
		vmaDestroyBuffer(m_Allocator, buffer->buffer, buffer->allocation);
	for(const AllocatedImage& image : m_Textures) m_TextureLoader->destroyImage(image);
	m_Textures.clear();
}

// ---------------------------------------------------------------- per frame

void GroundCover::update(VkCommandBuffer cmd, const FoliagePushConstants& pc) {
	m_PushConstants = pc;
	m_PushConstants.extra = m_Density; // groundcover.comp: pc.interaction
	if(!m_Enabled || m_Meshes.empty()) return;
	const VkPipelineStageFlags2 drawStages = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
	const VkAccessFlags2 drawReads = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;

	vkutil::memoryBarrier(cmd, drawStages, drawReads, VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	vkCmdUpdateBuffer(cmd, m_Commands.buffer, 0, m_CommandTemplate.size() * sizeof(VkDrawIndexedIndirectCommand), m_CommandTemplate.data());
	vkutil::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Scatter.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Scatter.layout, 0, 1, &m_ComputeSet, 0, nullptr);
	vkCmdPushConstants(cmd, m_Scatter.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FoliagePushConstants), &m_PushConstants);
	vkCmdDispatch(cmd, (THREADS + 63) / 64, 1, 1);

	vkutil::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, drawStages, drawReads);
}

void GroundCover::draw(VkCommandBuffer cmd) {
	if(!m_Enabled || m_Meshes.empty()) return;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_RenderPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_RenderPipelineLayout, 0, 1, &m_RenderSet, 0, nullptr);
	vkCmdPushConstants(cmd, m_RenderPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FoliagePushConstants), &m_PushConstants);
	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &m_VertexBuffer.buffer, &offset);
	vkCmdBindIndexBuffer(cmd, m_IndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
	vkCmdDrawIndexedIndirect(cmd, m_Commands.buffer, 0, static_cast<uint32_t>(m_Meshes.size()), sizeof(VkDrawIndexedIndirectCommand));
}

void GroundCover::drawShadow(VkCommandBuffer cmd, const glm::mat4& lightViewProj) {
	if(!m_Enabled || m_Meshes.empty()) return;
	FoliagePushConstants pc = m_PushConstants;
	pc.viewproj = lightViewProj;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ShadowPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_RenderPipelineLayout, 0, 1, &m_RenderSet, 0, nullptr);
	vkCmdPushConstants(cmd, m_RenderPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FoliagePushConstants), &pc);
	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &m_VertexBuffer.buffer, &offset);
	vkCmdBindIndexBuffer(cmd, m_IndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
	vkCmdDrawIndexedIndirect(cmd, m_Commands.buffer, MAX_MESHES * sizeof(VkDrawIndexedIndirectCommand), static_cast<uint32_t>(m_Meshes.size()), sizeof(VkDrawIndexedIndirectCommand));
}
