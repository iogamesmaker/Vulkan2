#include "props.hpp"
#include "application.hpp"
#include "gbuffer.hpp"
#include <filesystem>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <glm/gtc/quaternion.hpp>
#include <meshoptimizer.h>

// ---------------------------------------------------------------- meshes

namespace {
	// photoscans from Poly Haven (CC0, assets/props/README.md), in mesh order (propplacement.glsl: PROP_ROCKS rocks,
	// then the logs, then the stumps). node: the one mesh of a set file to use (nullptr: the file's only one).
	// lod0 / lod1: the triangle budgets the simplifier gets (the scans are 12k-100k triangles, thousands of rocks are
	// scattered around the camera)
	enum class Kind { Rock, Log, Stump };
	struct PropFile { const char* folder; const char* node; Kind kind; size_t lod0, lod1; };
	constexpr PropFile PROP_FILES[] = {
		{ "boulder_01", nullptr, Kind::Rock, 3000, 400 },
		{ "rock_07", nullptr, Kind::Rock, 2000, 300 },
		{ "rock_09", nullptr, Kind::Rock, 2000, 300 },
		{ "rock_moss_set_02", "rock_moss_set_02_rock10", Kind::Rock, 2000, 300 },
		{ "rock_moss_set_02", "rock_moss_set_02_rock12", Kind::Rock, 2000, 300 },
		{ "rock_moss_set_02", "rock_moss_set_02_rock09", Kind::Rock, 2000, 300 },
		{ "dead_tree_trunk", nullptr, Kind::Log, 5000, 700 },
		{ "dead_tree_trunk_02", nullptr, Kind::Log, 5000, 700 },
		{ "tree_stump_01", nullptr, Kind::Stump, 4000, 600 },
		{ "tree_stump_02", nullptr, Kind::Stump, 4000, 600 },
	};

	glm::mat3 nodeRotationScale(const fastgltf::Node& node) {
		// the rotation / scale only: the set's rocks are laid out side by side, each one goes to its own origin
		glm::mat4 m(1.0f);
		if(const auto* trs = std::get_if<fastgltf::TRS>(&node.transform)) {
			glm::quat q(trs->rotation[3], trs->rotation[0], trs->rotation[1], trs->rotation[2]); // w, x, y, z
			m = glm::mat4_cast(q) * glm::scale(glm::mat4(1.0f), glm::vec3(trs->scale[0], trs->scale[1], trs->scale[2]));
		} else if(const auto* mat = std::get_if<fastgltf::math::fmat4x4>(&node.transform)) {
			for(int c = 0; c < 3; c++)
				for(int r = 0; r < 3; r++) m[c][r] = (*mat)[c][r];
		}
		return glm::mat3(m);
	}
}

// one scan into mesh space, the space the placement and the collision shapes in propplacement.glsl are measured in:
//   rocks: centred, the larger horizontal half extent 0.55, standing on y = 0 (ROCK_SINK buries them a little)
//   logs: along x, -0.5 .. 0.5, lying on y = 0
//   stumps: the scan's own ground at y = 0, STUMP_HEIGHT (0.1) above it
// returns the vertices (w = the mesh, props.frag's texture) and indices relative to them
bool Props::loadMesh(uint32_t mesh, std::vector<Vertex>& vertices, std::vector<uint32_t>& indices) {
	const PropFile& file = PROP_FILES[mesh];
	std::filesystem::path path = util::getpath(std::string("assets\\props\\") + file.folder + "\\" + file.folder + "_1k.gltf");
	auto data = fastgltf::GltfDataBuffer::FromPath(path);
	if(data.error() != fastgltf::Error::None) {
		fmt::print("props: couldn't read {}\n", path.string());
		return false;
	}
	fastgltf::Parser parser{};
	auto asset = parser.loadGltf(data.get(), path.parent_path(), fastgltf::Options::LoadExternalBuffers);
	if(!asset) {
		fmt::print("props: couldn't load {}\n", path.string());
		return false;
	}
	fastgltf::Asset& gltf = asset.get();
	const fastgltf::Node* node = nullptr;
	for(const fastgltf::Node& n : gltf.nodes)
		if(n.meshIndex && (!file.node || n.name == file.node)) { node = &n; break; }
	if(!node) {
		fmt::print("props: no mesh {} in {}\n", file.node ? file.node : "", path.string());
		return false;
	}

	glm::mat3 transform = nodeRotationScale(*node);
	for(const fastgltf::Primitive& p : gltf.meshes[*node->meshIndex].primitives) {
		if(!p.indicesAccessor || p.findAttribute("POSITION") == p.attributes.end() || p.findAttribute("NORMAL") == p.attributes.end()
		   || p.findAttribute("TEXCOORD_0") == p.attributes.end()) continue;
		size_t base = vertices.size();
		const fastgltf::Accessor& positions = gltf.accessors[p.findAttribute("POSITION")->accessorIndex];
		vertices.resize(base + positions.count);
		fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, positions, [&](glm::vec3 v, size_t i) {
			vertices[base + i].positionMaterial = glm::vec4(transform * v, static_cast<float>(mesh));
		});
		fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, gltf.accessors[p.findAttribute("NORMAL")->accessorIndex], [&](glm::vec3 v, size_t i) {
			vertices[base + i].normalAO = glm::vec4(glm::normalize(transform * v), 1.0f);
		});
		fastgltf::iterateAccessorWithIndex<glm::vec2>(gltf, gltf.accessors[p.findAttribute("TEXCOORD_0")->accessorIndex], [&](glm::vec2 v, size_t i) {
			vertices[base + i].uv = glm::vec4(v, 0.0f, 0.0f);
		});
		fastgltf::iterateAccessor<uint32_t>(gltf, gltf.accessors[*p.indicesAccessor], [&](uint32_t index) { indices.push_back(index + static_cast<uint32_t>(base)); });
	}
	if(vertices.empty()) return false;

	glm::vec3 lo(1e9f), hi(-1e9f);
	for(const Vertex& v : vertices) { lo = glm::min(lo, glm::vec3(v.positionMaterial)); hi = glm::max(hi, glm::vec3(v.positionMaterial)); }
	if(file.kind == Kind::Log) {
		// the scans lie on a slope of their own (one end on its root plate, the other end raised): stood on their
		// lowest point, the far end hung in the air on flat ground. levelled about z so the lowest points near both
		// ends (the outer 15% of the length) are at the same height
		float length = hi.x - lo.x;
		float lowLeft = 1e9f, lowRight = 1e9f, xLeft = lo.x, xRight = hi.x;
		for(const Vertex& v : vertices) {
			glm::vec3 p(v.positionMaterial);
			if(p.x < lo.x + 0.15f * length && p.y < lowLeft) { lowLeft = p.y; xLeft = p.x; }
			if(p.x > hi.x - 0.15f * length && p.y < lowRight) { lowRight = p.y; xRight = p.x; }
		}
		float angle = -std::atan2(lowRight - lowLeft, xRight - xLeft);
		float c = std::cos(angle), s = std::sin(angle);
		auto rotateZ = [&](glm::vec3 v) { return glm::vec3(v.x * c - v.y * s, v.x * s + v.y * c, v.z); };
		lo = glm::vec3(1e9f); hi = glm::vec3(-1e9f);
		for(Vertex& v : vertices) {
			glm::vec3 p = rotateZ(glm::vec3(v.positionMaterial));
			v.positionMaterial = glm::vec4(p, v.positionMaterial.w);
			v.normalAO = glm::vec4(rotateZ(glm::vec3(v.normalAO)), v.normalAO.w);
			lo = glm::min(lo, p); hi = glm::max(hi, p);
		}
		fmt::print("props: {} levelled by {:.1f} degrees\n", file.folder, glm::degrees(angle));
	}
	glm::vec3 centre = (lo + hi) * 0.5f;
	glm::vec3 origin;
	float scale;
	if(file.kind == Kind::Rock) {
		origin = { centre.x, lo.y, centre.z };
		scale = 0.55f / std::max(hi.x - lo.x, hi.z - lo.z) * 2.0f;
	} else if(file.kind == Kind::Log) {
		origin = { centre.x, lo.y, centre.z };
		scale = 1.0f / (hi.x - lo.x);
	} else {
		origin = { centre.x, 0.0f, centre.z };
		scale = 0.1f / hi.y; // STUMP_HEIGHT
	}
	float height = (hi.y - origin.y) * scale;
	for(Vertex& v : vertices) {
		glm::vec3 p = (glm::vec3(v.positionMaterial) - origin) * scale;
		v.positionMaterial = glm::vec4(p, v.positionMaterial.w);
		// ambient occlusion: darker towards the ground they sit on, and (logs) on their underside
		glm::vec3 n(v.normalAO);
		float ao = 0.5f + 0.5f * glm::smoothstep(0.0f, 0.35f * height, p.y);
		if(file.kind == Kind::Log) ao = std::min(ao, 0.6f + 0.4f * glm::smoothstep(-1.0f, 0.4f, n.y));
		v.normalAO.w = ao;
	}
	return true;
}

// ---------------------------------------------------------------- setup

void Props::init(Application* engine, TextureLoader& textures, const AllocatedImage& heightmap, const AllocatedImage& climateMap, VkSampler heightmapSampler,
				 VkImageView mossTexture, VkSampler textureSampler, VkFormat shadowFormat) {
	m_Device = engine->m_Device;
	m_Allocator = engine->getAllocator();
	m_TextureLoader = &textures;
	static_assert(std::size(PROP_FILES) == MESHES);

	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
	for(uint32_t mesh = 0; mesh < MESHES; mesh++) {
		const PropFile& file = PROP_FILES[mesh];
		std::string base = std::string("assets\\props\\") + file.folder + "\\textures\\" + file.folder;
		m_Textures[mesh * 2] = textures.loadCompressed(base + "_diff_1k.jpg", engine, TextureKind::Albedo);
		m_Textures[mesh * 2 + 1] = textures.loadCompressed(base + "_nor_gl_1k.jpg", engine, TextureKind::Normal);

		std::vector<Vertex> meshVertices;
		std::vector<uint32_t> meshIndices;
		if(!loadMesh(mesh, meshVertices, meshIndices)) {
			meshVertices.assign(3, Vertex{});
			meshIndices = { 0, 1, 2 };
		}
		// the LODs from the scan: the simplifier keeps the uv seams and the silhouette. LOD0 within a small error
		// (it stops early on simple shapes), LOD1 down to its budget whatever it costs (it's small on screen)
		for(uint32_t lod = 0; lod < 2; lod++) {
			size_t target = std::min(meshIndices.size(), (lod == 0 ? file.lod0 : file.lod1) * 3);
			std::vector<uint32_t> simplified(meshIndices.size());
			float error = 0.0f;
			simplified.resize(meshopt_simplify(simplified.data(), meshIndices.data(), meshIndices.size(), &meshVertices[0].positionMaterial.x,
											   meshVertices.size(), sizeof(Vertex), target, lod == 0 ? 0.01f : 1.0f, 0, &error));
			if(simplified.size() > target * 2) // couldn't get anywhere near the budget within the error: drop whatever it takes
				simplified.resize(meshopt_simplifySloppy(simplified.data(), meshIndices.data(), meshIndices.size(), &meshVertices[0].positionMaterial.x,
														 meshVertices.size(), sizeof(Vertex), target, 1.0f, &error));
			meshopt_optimizeVertexCache(simplified.data(), simplified.data(), simplified.size(), meshVertices.size());
			Mesh& m = m_Meshes[mesh][lod];
			m.firstIndex = static_cast<uint32_t>(indices.size());
			m.indexCount = static_cast<uint32_t>(simplified.size());
			m.vertexOffset = static_cast<int32_t>(vertices.size());
			indices.insert(indices.end(), simplified.begin(), simplified.end());
		}
		fmt::print("props: {} {}: {} -> {} / {} triangles\n", file.folder, file.node ? file.node : "", meshIndices.size() / 3, m_Meshes[mesh][0].indexCount / 3, m_Meshes[mesh][1].indexCount / 3);
		vertices.insert(vertices.end(), meshVertices.begin(), meshVertices.end());
	}

	size_t vertexBytes = vertices.size() * sizeof(Vertex), indexBytes = indices.size() * sizeof(uint32_t);
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
	fmt::print("props: {} vertices, {} triangles\n", vertices.size(), indices.size() / 3);

	// bins: mesh * 2 + lod for the camera, then the shadow casters (cheap LOD)
	m_Instances = engine->createBuffer(BINS * CAPACITY * sizeof(glm::vec4) * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	m_Commands = engine->createBuffer(BINS * sizeof(VkDrawIndexedIndirectCommand), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	m_CommandTemplate.resize(BINS);
	for(uint32_t bin = 0; bin < BINS; bin++) {
		const Mesh& m = bin < MESHES * 2 ? m_Meshes[bin / 2][bin % 2] : m_Meshes[bin - MESHES * 2][1];
		m_CommandTemplate[bin] = { m.indexCount, 0, m.firstIndex, m.vertexOffset, bin * CAPACITY };
	}

	// descriptors
	DescriptorLayoutBuilder computeBuilder;
	computeBuilder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // heightmap
	computeBuilder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // climate map

	computeBuilder.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);		  // instances
	computeBuilder.add_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);		  // draw commands
	m_ComputeLayout = computeBuilder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
	m_ComputeSet = engine->g_DescriptorAllocator.allocate(m_Device, m_ComputeLayout);

	DescriptorLayoutBuilder renderBuilder;
	renderBuilder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);			// instances
	renderBuilder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, PROP_TEXTURES); // per mesh albedo + normal, then the moss
	m_RenderLayout = renderBuilder.build(m_Device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	m_RenderSet = engine->g_DescriptorAllocator.allocate(m_Device, m_RenderLayout);

	VkDescriptorBufferInfo instanceInfo{ m_Instances.buffer, 0, VK_WHOLE_SIZE };
	VkDescriptorBufferInfo commandInfo{ m_Commands.buffer, 0, VK_WHOLE_SIZE };
	VkDescriptorImageInfo textureInfos[PROP_TEXTURES];
	for(uint32_t i = 0; i < MESHES * 2; i++) textureInfos[i] = { textureSampler, m_Textures[i].imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	textureInfos[MESHES * 2] = { textureSampler, mossTexture, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	// the stone the set's rocks get instead of their share of the set's one 1k texture (that was a smear on them)
	const char* stones[4] = { "rock030.jpg", "rock030_normal.jpg", "rock062.jpg", "rock062_normal.jpg" };
	for(uint32_t i = 0; i < 4; i++) {
		m_Stone[i] = textures.loadCompressed(std::string("assets\\props\\stone\\") + stones[i], engine, i % 2 ? TextureKind::Normal : TextureKind::Albedo);
		textureInfos[MESHES * 2 + 1 + i] = { textureSampler, m_Stone[i].imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	}
	VkWriteDescriptorSet writes[4] = {};
	for(auto& write : writes) { write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; write.descriptorCount = 1; }
	writes[0].dstSet = m_ComputeSet; writes[0].dstBinding = 2; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[0].pBufferInfo = &instanceInfo;
	writes[1].dstSet = m_ComputeSet; writes[1].dstBinding = 3; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[1].pBufferInfo = &commandInfo;
	writes[2].dstSet = m_RenderSet; writes[2].dstBinding = 0; writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; writes[2].pBufferInfo = &instanceInfo;
	writes[3].dstSet = m_RenderSet; writes[3].dstBinding = 1; writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[3].descriptorCount = PROP_TEXTURES; writes[3].pImageInfo = textureInfos;
	vkUpdateDescriptorSets(m_Device, 4, writes, 0, nullptr);
	setHeightmap(heightmap, climateMap, heightmapSampler); // compute set bindings 0 and 1

	// pipelines
	VkPushConstantRange computeRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FoliagePushConstants) };
	VkPipelineLayoutCreateInfo computeLayoutCI = vkinit::pipeline_layout_create_info();
	computeLayoutCI.setLayoutCount = 1;
	computeLayoutCI.pSetLayouts = &m_ComputeLayout;
	computeLayoutCI.pushConstantRangeCount = 1;
	computeLayoutCI.pPushConstantRanges = &computeRange;
	VkPipelineLayout scatterLayout;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &computeLayoutCI, nullptr, &scatterLayout));
	m_Scatter = engine->loadComputeShader("src/shaders/bin/props.comp.spv", "props scatter", scatterLayout);

	VkPushConstantRange renderRange{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FoliagePushConstants) };
	VkPipelineLayoutCreateInfo renderLayoutCI = vkinit::pipeline_layout_create_info();
	renderLayoutCI.setLayoutCount = 1;
	renderLayoutCI.pSetLayouts = &m_RenderLayout;
	renderLayoutCI.pushConstantRangeCount = 1;
	renderLayoutCI.pPushConstantRanges = &renderRange;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &renderLayoutCI, nullptr, &m_RenderPipelineLayout));

	VkVertexInputBindingDescription binding{ 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
	std::vector<VkVertexInputAttributeDescription> attributes = {
		{ 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, positionMaterial) },
		{ 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, normalAO) },
		{ 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, uv) },
	};
	CompiledShader vertexShader = engine->loadShader("src/shaders/bin/props.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
	CompiledShader fragmentShader = engine->loadShader("src/shaders/bin/props.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
	CompiledShader shadowShader = engine->loadShader("src/shaders/bin/props_shadow.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);

	PipelineBuilder builder;
	builder._pipelineLayout = m_RenderPipelineLayout;
	builder.set_shaders(vertexShader.shader, fragmentShader.shader);
	builder.set_vertex_input({ binding }, attributes);
	builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
	builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE); // (the stumps' and logs' scans are open at the bottom)
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
	shadowBuilder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
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

void Props::setHeightmap(const AllocatedImage& heightmap, const AllocatedImage& climateMap, VkSampler heightmapSampler) {
	VkDescriptorImageInfo infos[2] = {
		{ heightmapSampler, heightmap.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ heightmapSampler, climateMap.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
	};
	VkWriteDescriptorSet writes[2] = {};
	for(uint32_t i = 0; i < 2; i++) {
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = m_ComputeSet;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[i].pImageInfo = &infos[i];
	}
	vkUpdateDescriptorSets(m_Device, 2, writes, 0, nullptr);
}

void Props::destroy() {
	vkDestroyPipeline(m_Device, m_RenderPipeline, nullptr);
	vkDestroyPipeline(m_Device, m_ShadowPipeline, nullptr);
	vkDestroyPipelineLayout(m_Device, m_RenderPipelineLayout, nullptr);
	vkDestroyPipelineLayout(m_Device, m_Scatter.layout, nullptr);
	vkDestroyDescriptorSetLayout(m_Device, m_ComputeLayout, nullptr);
	vkDestroyDescriptorSetLayout(m_Device, m_RenderLayout, nullptr);
	for(AllocatedBuffer* buffer : { &m_VertexBuffer, &m_IndexBuffer, &m_Instances, &m_Commands })
		vmaDestroyBuffer(m_Allocator, buffer->buffer, buffer->allocation);
	for(const AllocatedImage& image : m_Textures) m_TextureLoader->destroyImage(image);
	for(const AllocatedImage& image : m_Stone) m_TextureLoader->destroyImage(image);
}

// ---------------------------------------------------------------- per frame

void Props::update(VkCommandBuffer cmd, const FoliagePushConstants& pc) {
	m_PushConstants = pc;
	if(!m_Enabled) return;
	const VkPipelineStageFlags2 drawStages = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
	const VkAccessFlags2 drawReads = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;

	// the previous frame may still be drawing from these
	vkutil::memoryBarrier(cmd, drawStages, drawReads, VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	vkCmdUpdateBuffer(cmd, m_Commands.buffer, 0, m_CommandTemplate.size() * sizeof(VkDrawIndexedIndirectCommand), m_CommandTemplate.data());
	vkutil::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Scatter.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Scatter.layout, 0, 1, &m_ComputeSet, 0, nullptr);
	vkCmdPushConstants(cmd, m_Scatter.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FoliagePushConstants), &m_PushConstants);
	vkCmdDispatch(cmd, (GRID_SIDE * GRID_SIDE + 63) / 64, 1, 1);

	vkutil::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, drawStages, drawReads);
}

void Props::draw(VkCommandBuffer cmd) {
	if(!m_Enabled) return;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_RenderPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_RenderPipelineLayout, 0, 1, &m_RenderSet, 0, nullptr);
	vkCmdPushConstants(cmd, m_RenderPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FoliagePushConstants), &m_PushConstants);
	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &m_VertexBuffer.buffer, &offset);
	vkCmdBindIndexBuffer(cmd, m_IndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
	vkCmdDrawIndexedIndirect(cmd, m_Commands.buffer, 0, MESHES * 2, sizeof(VkDrawIndexedIndirectCommand)); // every mesh and LOD (multiDrawIndirect)
}

void Props::drawShadow(VkCommandBuffer cmd, const glm::mat4& lightViewProj) {
	if(!m_Enabled) return;
	FoliagePushConstants pc = m_PushConstants;
	pc.viewproj = lightViewProj;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ShadowPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_RenderPipelineLayout, 0, 1, &m_RenderSet, 0, nullptr);
	vkCmdPushConstants(cmd, m_RenderPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FoliagePushConstants), &pc);
	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &m_VertexBuffer.buffer, &offset);
	vkCmdBindIndexBuffer(cmd, m_IndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
	vkCmdDrawIndexedIndirect(cmd, m_Commands.buffer, MESHES * 2 * sizeof(VkDrawIndexedIndirectCommand), MESHES, sizeof(VkDrawIndexedIndirectCommand));
}
