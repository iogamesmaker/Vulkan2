#include "props.hpp"
#include "application.hpp"
#include "gbuffer.hpp"
#include <map>

// ---------------------------------------------------------------- meshes

namespace {
	float hash3(int x, int y, int z) {
		uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u + static_cast<uint32_t>(z) * 2147483647u;
		h = (h ^ (h >> 13)) * 1274126177u;
		return static_cast<float>((h ^ (h >> 16)) & 0xffffff) / static_cast<float>(0xffffff);
	}

	float valueNoise3(glm::vec3 p) {
		glm::vec3 i = glm::floor(p), f = p - i;
		glm::vec3 u = f * f * (3.0f - 2.0f * f);
		int x = static_cast<int>(i.x), y = static_cast<int>(i.y), z = static_cast<int>(i.z);
		auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
		float a = lerp(hash3(x, y, z), hash3(x + 1, y, z), u.x), b = lerp(hash3(x, y + 1, z), hash3(x + 1, y + 1, z), u.x);
		float c = lerp(hash3(x, y, z + 1), hash3(x + 1, y, z + 1), u.x), d = lerp(hash3(x, y + 1, z + 1), hash3(x + 1, y + 1, z + 1), u.x);
		return lerp(lerp(a, b, u.y), lerp(c, d, u.y), u.z);
	}
}

// a lumpy, faceted stone: an icosphere pushed in and out by noise, squashed, its bottom flattened (it sits sunk
// into the ground). the same noise for both LODs so they're the same shape
void Props::buildRock(uint32_t subdivisions, float seed, std::vector<Vertex>& vertices, std::vector<uint32_t>& indices) {
	const float t = (1.0f + std::sqrt(5.0f)) * 0.5f;
	std::vector<glm::vec3> positions = {
		{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t}, {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1},
	};
	std::vector<glm::uvec3> faces = {
		{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4}, {11, 10, 2}, {10, 7, 6}, {7, 1, 8},
		{3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8}, {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1},
	};
	for(auto& p : positions) p = glm::normalize(p);
	for(uint32_t s = 0; s < subdivisions; s++) {
		std::map<std::pair<uint32_t, uint32_t>, uint32_t> midpoints;
		auto midpoint = [&](uint32_t a, uint32_t b) {
			auto key = std::make_pair(std::min(a, b), std::max(a, b));
			auto found = midpoints.find(key);
			if(found != midpoints.end()) return found->second;
			positions.push_back(glm::normalize(positions[a] + positions[b]));
			return midpoints[key] = static_cast<uint32_t>(positions.size() - 1);
		};
		std::vector<glm::uvec3> next;
		for(auto f : faces) {
			uint32_t ab = midpoint(f.x, f.y), bc = midpoint(f.y, f.z), ca = midpoint(f.z, f.x);
			next.push_back({ f.x, ab, ca }); next.push_back({ f.y, bc, ab }); next.push_back({ f.z, ca, bc }); next.push_back({ ab, bc, ca });
		}
		faces = next;
	}

	glm::vec3 offset(seed * 17.3f, seed * 5.1f, seed * 11.7f);
	glm::vec3 squash = glm::vec3(1.0f, 0.62f, 0.85f) * 0.5f;
	for(auto& p : positions) {
		float r = 1.0f + (valueNoise3(p * 1.6f + offset) - 0.5f) * 0.7f + (valueNoise3(p * 4.2f + offset * 2.0f) - 0.5f) * 0.18f;
		p *= r;
		if(p.y < -0.3f) p.y = -0.3f + (p.y + 0.3f) * 0.15f; // flat bottom
		p *= squash;
	}

	std::vector<glm::vec3> normals(positions.size(), glm::vec3(0.0f));
	for(auto f : faces) {
		glm::vec3 n = glm::cross(positions[f.y] - positions[f.x], positions[f.z] - positions[f.x]);
		normals[f.x] += n; normals[f.y] += n; normals[f.z] += n;
	}
	for(size_t i = 0; i < positions.size(); i++) {
		glm::vec3 n = glm::normalize(normals[i]);
		if(glm::dot(n, positions[i]) < 0.0f) n = -n; // outward, whatever the winding
		float ao = 0.55f + 0.45f * glm::smoothstep(-0.25f, 0.2f, positions[i].y / squash.y * 0.5f); // darker towards the ground
		vertices.push_back({ glm::vec4(positions[i], 0.0f), glm::vec4(n, ao), glm::vec4(0.0f) });
	}
	for(auto f : faces) indices.insert(indices.end(), { f.x, f.y, f.z }); // relative to the mesh (its vertexOffset)
}

// a fallen trunk along x (-0.5 .. 0.5), lying on the ground (bottom at y = 0), slightly crooked, cut ends
void Props::buildLog(uint32_t segments, uint32_t rings, float seed, std::vector<Vertex>& vertices, std::vector<uint32_t>& indices) {
	const float radius = 0.055f; // LOG_RADIUS in propplacement.glsl
	auto centre = [&](float x) { return glm::vec3(x, radius + std::sin(x * 3.1f + seed * 7.0f) * 0.006f, std::sin(x * 4.3f + seed * 3.0f) * 0.012f); };
	uint32_t start = static_cast<uint32_t>(vertices.size());
	for(uint32_t r = 0; r <= rings; r++) {
		float v = static_cast<float>(r) / static_cast<float>(rings);
		float x = v - 0.5f;
		float taper = 1.0f - v * 0.25f; // thinner towards the top end
		for(uint32_t s = 0; s <= segments; s++) {
			float a = static_cast<float>(s) / static_cast<float>(segments) * 6.2831853f;
			glm::vec3 n(0.0f, std::cos(a), std::sin(a));
			glm::vec3 p = centre(x) + n * radius * taper;
			float ao = 0.6f + 0.4f * glm::smoothstep(-1.0f, 0.4f, n.y); // the underside, against the ground
			vertices.push_back({ glm::vec4(p, 1.0f), glm::vec4(n, ao), glm::vec4(static_cast<float>(s) / segments * 2.0f, v * 8.0f, 0.0f, 0.0f) });
		}
	}
	uint32_t stride = segments + 1;
	for(uint32_t r = 0; r < rings; r++) {
		for(uint32_t s = 0; s < segments; s++) {
			uint32_t a = r * stride + s, b = a + 1, c = a + stride, d = c + 1;
			indices.insert(indices.end(), { a, c, b, b, c, d });
		}
	}
	// end caps (growth rings)
	for(int end = 0; end < 2; end++) {
		float x = end == 0 ? -0.5f : 0.5f;
		float taper = end == 0 ? 1.0f : 0.75f;
		glm::vec3 n(end == 0 ? -1.0f : 1.0f, 0.0f, 0.0f);
		uint32_t centreIndex = static_cast<uint32_t>(vertices.size()) - start;
		vertices.push_back({ glm::vec4(centre(x), 2.0f), glm::vec4(n, 0.8f), glm::vec4(0.5f, 0.5f, 0.0f, 0.0f) });
		for(uint32_t s = 0; s <= segments; s++) {
			float a = static_cast<float>(s) / static_cast<float>(segments) * 6.2831853f;
			glm::vec3 p = centre(x) + glm::vec3(0.0f, std::cos(a), std::sin(a)) * radius * taper;
			vertices.push_back({ glm::vec4(p, 2.0f), glm::vec4(n, 0.8f), glm::vec4(0.5f + 0.5f * std::cos(a), 0.5f + 0.5f * std::sin(a), 0.0f, 0.0f) });
		}
		for(uint32_t s = 0; s < segments; s++) indices.insert(indices.end(), { centreIndex, centreIndex + 1 + s, centreIndex + 2 + s });
	}
}

// a tree stump: a bark cylinder flaring out into roots at the ground, growth rings on top (cut flat, or broken off
// in jagged splinters). mesh space: base at y = 0, STUMP_RADIUS / STUMP_HEIGHT in propplacement.glsl
void Props::buildStump(uint32_t segments, bool broken, float seed, std::vector<Vertex>& vertices, std::vector<uint32_t>& indices) {
	const float radius = 0.06f, height = 0.1f;
	const uint32_t rings = 5;
	auto topAt = [&](float a) { // the height of the top edge around the stump
		if(!broken) return height + std::sin(a + seed) * 0.004f; // a saw cut, not quite level
		float splinters = std::abs(std::sin(a * 3.0f + seed * 5.0f)) * 0.05f + std::abs(std::sin(a * 7.0f + seed)) * 0.025f;
		return height * 0.8f + splinters;
	};
	uint32_t start = static_cast<uint32_t>(vertices.size());
	for(uint32_t r = 0; r <= rings; r++) {
		float v = static_cast<float>(r) / rings;
		for(uint32_t s = 0; s <= segments; s++) {
			float a = static_cast<float>(s) / segments * 6.2831853f;
			// roots: lobes that swell out towards the ground
			float roots = std::pow(std::max(std::cos(a * 4.0f + seed * 3.0f), 0.0f), 3.0f);
			float flare = 1.0f + (1.0f - v) * (1.0f - v) * (0.5f + roots * 0.9f);
			glm::vec3 n(std::cos(a), (1.0f - v) * 0.6f * flare - 0.3f, std::sin(a));
			glm::vec3 p(std::cos(a) * radius * flare, glm::mix(-0.01f, topAt(a), v), std::sin(a) * radius * flare);
			float ao = glm::mix(0.55f, 1.0f, v);
			vertices.push_back({ glm::vec4(p, 1.0f), glm::vec4(glm::normalize(n), ao), glm::vec4(static_cast<float>(s) / segments * 2.0f, p.y * 8.0f / 0.4f, 0.0f, 0.0f) });
		}
	}
	uint32_t stride = segments + 1;
	for(uint32_t r = 0; r < rings; r++)
		for(uint32_t s = 0; s < segments; s++) {
			uint32_t a = r * stride + s, b = a + 1, c2 = a + stride, d = c2 + 1; // mesh relative (the mesh gets a vertex offset)
			indices.insert(indices.end(), { a, b, c2, b, d, c2 });
		}
	// the top: growth rings, a fan from the middle (raised a little where it's broken)
	uint32_t centreIndex = static_cast<uint32_t>(vertices.size()) - start;
	vertices.push_back({ glm::vec4(0.0f, broken ? height * 0.95f : height, 0.0f, 2.0f), glm::vec4(0.0f, 1.0f, 0.0f, 0.85f), glm::vec4(0.5f, 0.5f, 0.0f, 0.0f) });
	for(uint32_t s = 0; s <= segments; s++) {
		float a = static_cast<float>(s) / segments * 6.2831853f;
		glm::vec3 p(std::cos(a) * radius, topAt(a), std::sin(a) * radius);
		vertices.push_back({ glm::vec4(p, 2.0f), glm::vec4(0.0f, 1.0f, 0.0f, 0.8f), glm::vec4(0.5f + 0.5f * std::cos(a), 0.5f + 0.5f * std::sin(a), 0.0f, 0.0f) });
	}
	for(uint32_t s = 0; s < segments; s++) indices.insert(indices.end(), { centreIndex, centreIndex + 2 + s, centreIndex + 1 + s });
}

// ---------------------------------------------------------------- setup

void Props::init(Application* engine, const AllocatedImage& heightmap, const AllocatedImage& climateMap, VkSampler heightmapSampler,
				 const VkImageView* textures, bool endGrainTexture, VkSampler textureSampler, VkFormat shadowFormat) {
	m_Device = engine->m_Device;
	m_Allocator = engine->getAllocator();

	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
	for(uint32_t mesh = 0; mesh < MESHES; mesh++) {
		for(uint32_t lod = 0; lod < 2; lod++) {
			Mesh& m = m_Meshes[mesh][lod];
			m.firstIndex = static_cast<uint32_t>(indices.size());
			m.vertexOffset = static_cast<int32_t>(vertices.size());
			std::vector<Vertex> meshVertices;
			if(mesh < 4) buildRock(lod == 0 ? 3 : 1, static_cast<float>(mesh) + 0.5f, meshVertices, indices);
			else if(mesh < 6) buildLog(lod == 0 ? 12 : 6, lod == 0 ? 8 : 2, static_cast<float>(mesh), meshVertices, indices);
			else buildStump(lod == 0 ? 14 : 7, mesh == 7, static_cast<float>(mesh), meshVertices, indices);
			vertices.insert(vertices.end(), meshVertices.begin(), meshVertices.end());
			m.indexCount = static_cast<uint32_t>(indices.size()) - m.firstIndex;
		}
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
	renderBuilder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, PROP_TEXTURES); // rock, bark, moss, end grain
	m_RenderLayout = renderBuilder.build(m_Device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	m_RenderSet = engine->g_DescriptorAllocator.allocate(m_Device, m_RenderLayout);

	VkDescriptorBufferInfo instanceInfo{ m_Instances.buffer, 0, VK_WHOLE_SIZE };
	VkDescriptorBufferInfo commandInfo{ m_Commands.buffer, 0, VK_WHOLE_SIZE };
	VkDescriptorImageInfo textureInfos[PROP_TEXTURES];
	for(uint32_t i = 0; i < PROP_TEXTURES; i++) textureInfos[i] = { textureSampler, textures[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
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
	// props.frag's END_GRAIN_TEXTURE: the photographed cross section, or the procedural rings without it
	VkBool32 endGrain = endGrainTexture ? VK_TRUE : VK_FALSE;
	VkSpecializationMapEntry endGrainEntry{ 0, 0, sizeof(VkBool32) };
	VkSpecializationInfo endGrainInfo{ 1, &endGrainEntry, sizeof(VkBool32), &endGrain };
	builder._shaderStages[1].pSpecializationInfo = &endGrainInfo; // [0] vertex, [1] fragment
	builder.set_vertex_input({ binding }, attributes);
	builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
	builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE); // the procedural winding isn't consistent, cheap meshes anyway
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
	for(uint32_t bin = 0; bin < MESHES * 2; bin++)
		vkCmdDrawIndexedIndirect(cmd, m_Commands.buffer, bin * sizeof(VkDrawIndexedIndirectCommand), 1, sizeof(VkDrawIndexedIndirectCommand));
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
	for(uint32_t bin = MESHES * 2; bin < BINS; bin++)
		vkCmdDrawIndexedIndirect(cmd, m_Commands.buffer, bin * sizeof(VkDrawIndexedIndirectCommand), 1, sizeof(VkDrawIndexedIndirectCommand));
}
