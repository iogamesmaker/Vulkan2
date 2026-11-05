#include "shadows.hpp"
#include "application.hpp"
#include <glm/gtc/matrix_transform.hpp>

static_assert(SHADOW_FRAMES == FRAME_OVERLAP, "one shadow UBO per frame in flight");

void ShadowMaps::init(VkDevice device, VmaAllocator allocator, Application* engine) {
	m_Device = device;
	m_Allocator = allocator;

	m_Engine = engine;
	createImage();

	// comparison sampler, outside the map counts as lit
	VkSamplerCreateInfo samplerCI = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	samplerCI.magFilter = VK_FILTER_LINEAR;
	samplerCI.minFilter = VK_FILTER_LINEAR;
	samplerCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	samplerCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	samplerCI.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
	samplerCI.compareEnable = VK_TRUE;
	samplerCI.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL; // lit when the fragment is at or in front of the stored depth
	VK_CHECK(vkCreateSampler(m_Device, &samplerCI, nullptr, &m_Sampler));

	VkSamplerCreateInfo depthSamplerCI = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	depthSamplerCI.magFilter = VK_FILTER_NEAREST;
	depthSamplerCI.minFilter = VK_FILTER_NEAREST;
	depthSamplerCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	depthSamplerCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	depthSamplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
	depthSamplerCI.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE; // outside = far away = no blocker
	VK_CHECK(vkCreateSampler(m_Device, &depthSamplerCI, nullptr, &m_DepthSampler));

	// UBOs + descriptor sets, one per frame in flight since the blend changes every frame
	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // shadow map
	builder.add_binding(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);		   // cascades
	builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // same maps, raw depth
	m_DescriptorLayout = builder.build(m_Device, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT); // compute: lighting.comp

	for(uint32_t i = 0; i < SHADOW_FRAMES; i++) {
		VkBufferCreateInfo bufferCI{};
		bufferCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
		bufferCI.size = sizeof(ShadowUBO);
		bufferCI.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

		VmaAllocationCreateInfo vmaCI{};
		vmaCI.usage = VMA_MEMORY_USAGE_AUTO;
		vmaCI.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
		VK_CHECK(vmaCreateBuffer(m_Allocator, &bufferCI, &vmaCI, &m_UBOs[i].buffer, &m_UBOs[i].allocation, &m_UBOs[i].info));

		m_Descriptors[i] = engine->g_DescriptorAllocator.allocate(m_Device, m_DescriptorLayout);

		VkDescriptorBufferInfo bufferInfo{};
		bufferInfo.buffer = m_UBOs[i].buffer;
		bufferInfo.offset = 0;
		bufferInfo.range = sizeof(ShadowUBO);

		VkWriteDescriptorSet write{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		write.dstSet = m_Descriptors[i];
		write.dstBinding = 1;
		write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
		write.pBufferInfo = &bufferInfo;
		vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);
	}
	writeImageDescriptors();

	for(auto& set : m_Sets) {
		for(auto& matrix : set.lightViewProj) matrix = glm::mat4(1.0f);
		for(auto& matrix : set.renderedViewProj) matrix = glm::mat4(1.0f);
		for(auto& center : set.centers) center = glm::vec3(0.0f);
	}
}

// the depth array, one layer per set and cascade, at m_Resolution. left in SHADER_READ_ONLY_OPTIMAL (how
// everything samples it outside of render())
void ShadowMaps::createImage() {
	constexpr uint32_t layers = SHADOW_SETS * SHADOW_CASCADES;
	m_Image.imageFormat = m_Format;
	m_Image.imageExtent = { m_Resolution, m_Resolution, 1 };

	VkImageCreateInfo imageCI = vkinit::image_create_info(m_Format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, m_Image.imageExtent);
	imageCI.arrayLayers = layers;

	VmaAllocationCreateInfo imageAllocinfo = {};
	imageAllocinfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	imageAllocinfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	VK_CHECK(vmaCreateImage(m_Allocator, &imageCI, &imageAllocinfo, &m_Image.image, &m_Image.allocation, nullptr));

	VkImageViewCreateInfo viewCI = vkinit::imageview_create_info(m_Format, m_Image.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_VIEW_TYPE_2D_ARRAY);
	viewCI.subresourceRange.layerCount = layers;
	VK_CHECK(vkCreateImageView(m_Device, &viewCI, nullptr, &m_Image.imageView));

	for(uint32_t i = 0; i < layers; i++) {
		VkImageViewCreateInfo layerCI = vkinit::imageview_create_info(m_Format, m_Image.image, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_VIEW_TYPE_2D);
		layerCI.subresourceRange.baseArrayLayer = i;
		layerCI.subresourceRange.layerCount = 1;
		VK_CHECK(vkCreateImageView(m_Device, &layerCI, nullptr, &m_LayerViews[i]));
	}

	m_Engine->immediateSubmit([&](VkCommandBuffer cmd) {
		for(uint32_t i = 0; i < layers; i++) transitionLayer(cmd, i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	});
}

void ShadowMaps::destroyImage() {
	for(VkImageView view : m_LayerViews) vkDestroyImageView(m_Device, view, nullptr);
	vkDestroyImageView(m_Device, m_Image.imageView, nullptr);
	vmaDestroyImage(m_Allocator, m_Image.image, m_Image.allocation);
}

// the maps (comparison + raw depth) in every frame's set
void ShadowMaps::writeImageDescriptors() {
	for(uint32_t i = 0; i < SHADOW_FRAMES; i++) {
		VkDescriptorImageInfo compareInfo{ m_Sampler, m_Image.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkDescriptorImageInfo depthInfo{ m_DepthSampler, m_Image.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
		VkWriteDescriptorSet writes[2] = {};
		for(auto& w : writes) {
			w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			w.dstSet = m_Descriptors[i];
			w.descriptorCount = 1;
			w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		}
		writes[0].dstBinding = 0; writes[0].pImageInfo = &compareInfo;
		writes[1].dstBinding = 2; writes[1].pImageInfo = &depthInfo;
		vkUpdateDescriptorSets(m_Device, 2, writes, 0, nullptr);
	}
}

// a new resolution per cascade: the maps are recreated and everything re-rendered. the caller makes sure the GPU
// is idle (no frame in flight still samples the old maps)
bool ShadowMaps::setResolution(uint32_t resolution) {
	if(resolution == m_Resolution || m_Device == VK_NULL_HANDLE) { m_Resolution = resolution; return false; }
	m_Resolution = resolution;
	destroyImage();
	createImage();
	writeImageDescriptors();
	for(auto& set : m_Sets) {
		set.key = LLONG_MIN;
		for(bool& due : set.due) due = true;
	}
	m_ForceAll = true;
	return true;
}

// the split distances of the near cascades, world units (~6m each)
void ShadowMaps::setNearSplits(float c0, float c1, float c2) {
	if(c0 != m_NearSplits[0] || c1 != m_NearSplits[1] || c2 != m_NearSplits[2]) m_ForceAll = true;
	m_NearSplits[0] = c0; m_NearSplits[1] = c1; m_NearSplits[2] = c2;
}

void ShadowMaps::destroy() {
	for(uint32_t i = 0; i < SHADOW_FRAMES; i++) vmaDestroyBuffer(m_Allocator, m_UBOs[i].buffer, m_UBOs[i].allocation);
	destroyImage();
	vkDestroySampler(m_Device, m_Sampler, nullptr);
	vkDestroySampler(m_Device, m_DepthSampler, nullptr);
	vkDestroyDescriptorSetLayout(m_Device, m_DescriptorLayout, nullptr);
}

void ShadowMaps::transitionLayer(VkCommandBuffer cmd, uint32_t layer, VkImageLayout from, VkImageLayout to) {
	VkImageSubresourceRange range = vkinit::image_subresource_range(VK_IMAGE_ASPECT_DEPTH_BIT);
	range.baseArrayLayer = layer;
	range.layerCount = 1;
	vkutil::transition_image(cmd, m_Image.image, from, to, range);
}

// cascades are spheres around the camera (with padding) instead of fitted to the view frustum, so turning the
// camera never needs a re-render and moving only does once the padding runs out
// the half width of a cascade's square, with room to move in (snapped so it doesn't change with float noise)
float ShadowMaps::cascadeRadius(uint32_t cascade) const {
	return std::ceil(m_Splits[cascade] * (1.0f + cascadePadding(cascade)) * 16.0f) / 16.0f;
}

void ShadowMaps::computeCascade(ShadowSet& set, uint32_t cascade, glm::vec3 center, float terrainHeightRange) {
	glm::vec3 lightDir = set.lightDir;
	float radius = cascadeRadius(cascade);

	glm::vec3 up = std::abs(lightDir.y) > 0.99f ? glm::vec3(0.0f, 0.0f, 1.0f) : glm::vec3(0.0f, 1.0f, 0.0f);

	// casters can be well outside the sphere when the light is low, extend towards the light
	float casterMargin = std::min(terrainHeightRange / std::max(lightDir.y, 0.03f), 8000.0f);
	float depthRange = 2.0f * radius + casterMargin;

	glm::mat4 lightView = glm::lookAt(center + lightDir * (radius + casterMargin), center, up);
	glm::mat4 lightProj = glm::ortho(-radius, radius, -radius, radius, 0.0f, depthRange);

	// snap to whole texels so a re-render after moving doesn't shift the shadow edges
	glm::mat4 lightViewProj = lightProj * lightView;
	glm::vec4 origin = lightViewProj * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
	origin *= static_cast<float>(m_Resolution) * 0.5f;
	glm::vec4 rounded = glm::round(origin);
	glm::vec4 offset = (rounded - origin) * (2.0f / static_cast<float>(m_Resolution));
	lightProj[3][0] += offset.x;
	lightProj[3][1] += offset.y;

	set.lightViewProj[cascade] = lightProj * lightView;
	set.depthRange[cascade] = depthRange;
	set.centers[cascade] = center;
	set.radius[cascade] = radius;
	set.due[cascade] = true;
}

void ShadowMaps::update(const glm::mat4& view, const ShadowLightKeys& keys, float terrainHeightRange, uint32_t frameNumber) {
	// the near cascades at fixed distances, focused on the player (settings.hpp picks them): cascade 0 the
	// immediate surroundings, 1 everything that casts (trees, rocks, grass), 2 the nearby terrain. a log / linear
	// mix spread them out to 26 / 125 / 1200 units, ~9m texels in cascade 2. not much tighter than ~12 for the
	// first one though: the PCSS blocker search (2.5 units) and its filter ran off the edge of a ~10 unit cascade.
	// they never reach past the world
	constexpr uint32_t nearCascades = SHADOW_CASCADES - 1;
	float limit = m_FarDistance * 0.75f;
	for(uint32_t i = 0; i < nearCascades; i++) m_Splits[i] = std::min(m_NearSplits[i], limit * (i + 1) / nearCascades);
	// the terrain only cascade covers exactly the visible world, a circle around the camera
	m_Splits[SHADOW_CASCADES - 1] = m_FarDistance;

	glm::vec3 camPos = glm::vec3(glm::inverse(view)[3]);

	// find (or recycle) a set for keyframe A and one for keyframe B
	auto findSet = [&](long long key) -> int {
		for(uint32_t i = 0; i < SHADOW_SETS; i++) if(m_Sets[i].key == key) return static_cast<int>(i);
		return -1;
	};
	int setA = findSet(keys.keyA);
	int setB = findSet(keys.keyB);
	if(setA < 0) setA = (setB == 0) ? 1 : 0;
	if(setB < 0) setB = (setA == 0) ? 1 : 0;

	const long long setKeys[2] = { keys.keyA, keys.keyB };
	const glm::vec3 setDirs[2] = { keys.dirA, keys.dirB };
	const int setIndices[2] = { setA, setB };

	for(int k = 0; k < 2; k++) {
		ShadowSet& set = m_Sets[setIndices[k]];
		bool newKey = set.key != setKeys[k];
		if(newKey) {
			set.key = setKeys[k];
			set.lightDir = setDirs[k];
		}

		for(uint32_t c = 0; c < SHADOW_CASCADES; c++) {
			bool movedOut = glm::distance(camPos, set.centers[c]) > m_Splits[c] * cascadePadding(c) * 0.8f;
			if(newKey || movedOut || m_ForceAll) computeCascade(set, c, camPos, terrainHeightRange);
		}
	}
	if(m_ForceAll) m_Urgent = true;
	m_ForceAll = false;
	m_SetA = setA;
	m_SetB = setB;
	m_FrameSlot = frameNumber % SHADOW_FRAMES;

	ShadowUBO& data = m_UBOData;
	for(uint32_t s = 0; s < SHADOW_SETS; s++)
		for(uint32_t c = 0; c < SHADOW_CASCADES; c++) data.lightViewProj[s * SHADOW_CASCADES + c] = m_Sets[s].lightViewProj[c];

	float widths[SHADOW_CASCADES];
	for(uint32_t c = 0; c < SHADOW_CASCADES; c++) widths[c] = 2.0f * cascadeRadius(c);

	data.splits = glm::vec4(m_Splits[0], m_Splits[1], m_Splits[2], m_Splits[3]);
	data.widths = glm::vec4(widths[0], widths[1], widths[2], widths[3]);
	data.params = glm::vec4(m_FilterRadius, m_NormalOffset, m_Enabled ? 1.0f : 0.0f, m_Splits[SHADOW_CASCADES - 1]);
	data.blend	= glm::vec4(keys.t, keys.strength, static_cast<float>(m_SetA), static_cast<float>(m_SetB));
	data.cascadeBlend = glm::vec4(m_CascadeBlend, m_Soft ? 1.0f : 0.0f, 0.0f, 0.0f);
	// matrices and depth ranges are filled in by render(), from what's actually in the maps
}

uint32_t ShadowMaps::render(VkCommandBuffer cmd, const std::function<void(VkCommandBuffer, const ShadowCascadeDraw&)>& drawScene) {
	uint32_t rendered = 0;
	auto writeUBO = [&]() {
		for(uint32_t s = 0; s < SHADOW_SETS; s++) {
			for(uint32_t c = 0; c < SHADOW_CASCADES; c++) m_UBOData.lightViewProj[s * SHADOW_CASCADES + c] = m_Sets[s].renderedViewProj[c];
			const float* range = m_Sets[s].renderedDepthRange;
			m_UBOData.depthRange[s] = glm::vec4(range[0], range[1], range[2], range[3]);
		}
		// this frame's fence was waited on, nothing is reading this buffer right now
		memcpy(m_UBOs[m_FrameSlot].info.pMappedData, &m_UBOData, sizeof(ShadowUBO));
	};
	if(!m_Enabled) {
		m_ForceAll = true; // everything is stale once they're turned back on
		writeUBO();
		return 0;
	}

	VkExtent2D extent = { m_Resolution, m_Resolution };

	// the set that's fading in goes last, it has the least weight
	const uint32_t order[SHADOW_SETS] = { m_SetA, m_SetB };
	uint32_t fadingInBudget = m_Urgent ? SHADOW_CASCADES : 1;
	m_Urgent = false;

	for(uint32_t s : order) {
		for(uint32_t c = 0; c < SHADOW_CASCADES; c++) {
			if(!m_Sets[s].due[c]) continue;
			if(s == m_SetB && s != m_SetA) {
				if(fadingInBudget == 0) continue;
				fadingInBudget--;
			}
			uint32_t layer = s * SHADOW_CASCADES + c;

			transitionLayer(cmd, layer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

			VkClearValue depthClear{};
			depthClear.depthStencil.depth = 1.0f;
			VkRenderingAttachmentInfo depthAttachment = vkinit::attachment_info(m_LayerViews[layer], &depthClear, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
			VkRenderingInfo renderInfo = vkinit::rendering_info(extent, nullptr, &depthAttachment);
			vkCmdBeginRendering(cmd, &renderInfo);

			VkViewport viewport = { 0.0f, 0.0f, static_cast<float>(m_Resolution), static_cast<float>(m_Resolution), 0.0f, 1.0f };
			vkCmdSetViewport(cmd, 0, 1, &viewport);
			VkRect2D scissor = { {0, 0}, extent };
			vkCmdSetScissor(cmd, 0, 1, &scissor);

			drawScene(cmd, { m_Sets[s].lightViewProj[c], m_Sets[s].centers[c], m_Sets[s].radius[c], m_CascadeTessLevels[c], c });

			vkCmdEndRendering(cmd);

			transitionLayer(cmd, layer, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			m_Sets[s].due[c] = false;
			m_Sets[s].renderedViewProj[c] = m_Sets[s].lightViewProj[c];
			m_Sets[s].renderedDepthRange[c] = m_Sets[s].depthRange[c];
			rendered++;
		}
	}
	writeUBO();
	return rendered;
}

// ---------------------------------------------------------------- screen space filter

struct ShadowFilterPushConstants {
	glm::ivec2 size;
	int pass;
	int radius;
};

void ShadowFilter::init(Application* engine) {
	m_Device = engine->m_Device;
	m_Allocator = engine->getAllocator();

	VkSamplerCreateInfo samplerCI = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	samplerCI.magFilter = VK_FILTER_NEAREST;
	samplerCI.minFilter = VK_FILTER_NEAREST;
	samplerCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VK_CHECK(vkCreateSampler(m_Device, &samplerCI, nullptr, &m_DepthSampler));

	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);		   // draw image
	builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);		   // direct light + shadow
	builder.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);		   // horizontally blurred shadow
	builder.add_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // depth
	m_Layout = builder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
	m_Set = engine->g_DescriptorAllocator.allocate(m_Device, m_Layout);

	VkPushConstantRange range{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ShadowFilterPushConstants) };
	VkPipelineLayoutCreateInfo layoutCI{ .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
	layoutCI.setLayoutCount = 1;
	layoutCI.pSetLayouts = &m_Layout;
	layoutCI.pushConstantRangeCount = 1;
	layoutCI.pPushConstantRanges = &range;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &layoutCI, nullptr, &m_PipelineLayout));
	m_Pipeline = engine->loadComputeShader("src/shaders/bin/shadowresolve.comp.spv", "shadow filter", m_PipelineLayout).pipeline; // destroyed by the engine
}

void ShadowFilter::destroyTargets() {
	for(AllocatedImage* image : { &m_Direct, &m_Temp }) {
		if(!image->image) continue;
		vkDestroyImageView(m_Device, image->imageView, nullptr);
		vmaDestroyImage(m_Allocator, image->image, image->allocation);
		*image = {};
	}
}

void ShadowFilter::destroy() {
	destroyTargets();
	vkDestroySampler(m_Device, m_DepthSampler, nullptr);
	vkDestroyPipelineLayout(m_Device, m_PipelineLayout, nullptr);
	vkDestroyDescriptorSetLayout(m_Device, m_Layout, nullptr);
}

void ShadowFilter::setTargets(const AllocatedImage& drawImage, const AllocatedImage& depthBuffer) {
	destroyTargets();

	VmaAllocationCreateInfo allocCI = {};
	allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	allocCI.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	auto create = [&](AllocatedImage& image, VkFormat format, VkImageUsageFlags usage) {
		image.imageFormat = format;
		image.imageExtent = drawImage.imageExtent;
		VkImageCreateInfo imageCI = vkinit::image_create_info(format, usage, image.imageExtent);
		VK_CHECK(vmaCreateImage(m_Allocator, &imageCI, &allocCI, &image.image, &image.allocation, nullptr));
		VkImageViewCreateInfo viewCI = vkinit::imageview_create_info(format, image.image, VK_IMAGE_ASPECT_COLOR_BIT);
		VK_CHECK(vkCreateImageView(m_Device, &viewCI, nullptr, &image.imageView));
	};
	create(m_Direct, DIRECT_LIGHT_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT); // sampled: the debug view
	create(m_Temp, VK_FORMAT_R16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT); // sampled: the debug view

	VkDescriptorImageInfo images[4] = {
		{ VK_NULL_HANDLE, drawImage.imageView, VK_IMAGE_LAYOUT_GENERAL },
		{ VK_NULL_HANDLE, m_Direct.imageView, VK_IMAGE_LAYOUT_GENERAL },
		{ VK_NULL_HANDLE, m_Temp.imageView, VK_IMAGE_LAYOUT_GENERAL },
		{ m_DepthSampler, depthBuffer.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
	};
	VkWriteDescriptorSet writes[4] = {};
	for(uint32_t i = 0; i < 4; i++) {
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = m_Set;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = i == 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
		writes[i].pImageInfo = &images[i];
	}
	vkUpdateDescriptorSets(m_Device, 4, writes, 0, nullptr);
}

void ShadowFilter::resolve(VkCommandBuffer cmd, VkExtent2D extent, VkImage drawImage, VkImage depthBuffer) {
	VkImageSubresourceRange depthRange = vkinit::image_subresource_range(VK_IMAGE_ASPECT_DEPTH_BIT);
	vkutil::transition_image(cmd, drawImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
	vkutil::transition_image(cmd, m_Direct.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
	vkutil::transition_image(cmd, m_Temp.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
	vkutil::transition_image(cmd, depthBuffer, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, depthRange);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PipelineLayout, 0, 1, &m_Set, 0, nullptr);
	ShadowFilterPushConstants pc{ glm::ivec2(extent.width, extent.height), 0, std::max(m_Radius, 0) };
	uint32_t groupsX = (extent.width + 7) / 8, groupsY = (extent.height + 7) / 8;
	vkCmdPushConstants(cmd, m_PipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	vkCmdDispatch(cmd, groupsX, groupsY, 1);

	VkMemoryBarrier2 barrier{ .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
	barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
	barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
	VkDependencyInfo dependency{ .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	dependency.memoryBarrierCount = 1;
	dependency.pMemoryBarriers = &barrier;
	vkCmdPipelineBarrier2(cmd, &dependency);

	pc.pass = 1;
	vkCmdPushConstants(cmd, m_PipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	vkCmdDispatch(cmd, groupsX, groupsY, 1);

	vkutil::transition_image(cmd, drawImage, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	vkutil::transition_image(cmd, depthBuffer, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, depthRange);
}
