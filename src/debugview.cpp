#include "debugview.hpp"
#include "application.hpp"
#include <algorithm>

struct DebugViewPushConstants {
	glm::ivec4 mode;  // x = kind, y = channels, z = flags, w = unused
	glm::vec4 range;  // x = min, y = max, z = mip, w = array layer / 3D slice (0-1)
	glm::vec4 extra;  // x = near plane, y = far plane, zw = region written
};

enum : int { FLAG_SIGNED = 1, FLAG_GAMMA = 2, FLAG_INVERT = 4, FLAG_DEPTH_DISTANCE = 8 };

void DebugView::init(Application* engine, VkImageView fallback2D, VkImageView fallbackArray, VkImageView fallback3D) {
	m_Device = engine->m_Device;
	m_Allocator = engine->getAllocator();
	m_Fallback2D = fallback2D;
	m_FallbackArray = fallbackArray;
	m_Fallback3D = fallback3D;

	m_Output.imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
	m_Output.imageExtent = { SIZE, SIZE, 1 };
	VkImageCreateInfo imageCI = vkinit::image_create_info(m_Output.imageFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, m_Output.imageExtent);
	VmaAllocationCreateInfo allocCI{ .usage = VMA_MEMORY_USAGE_GPU_ONLY };
	VK_CHECK(vmaCreateImage(m_Allocator, &imageCI, &allocCI, &m_Output.image, &m_Output.allocation, nullptr));
	VkImageViewCreateInfo viewCI = vkinit::imageview_create_info(m_Output.imageFormat, m_Output.image, VK_IMAGE_ASPECT_COLOR_BIT);
	VK_CHECK(vkCreateImageView(m_Device, &viewCI, nullptr, &m_Output.imageView));

	VkSamplerCreateInfo samplerCI = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	samplerCI.addressModeU = samplerCI.addressModeV = samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerCI.maxLod = VK_LOD_CLAMP_NONE; // every lookup names its mip
	samplerCI.magFilter = samplerCI.minFilter = VK_FILTER_NEAREST;
	VK_CHECK(vkCreateSampler(m_Device, &samplerCI, nullptr, &m_Nearest));
	samplerCI.magFilter = samplerCI.minFilter = VK_FILTER_LINEAR;
	VK_CHECK(vkCreateSampler(m_Device, &samplerCI, nullptr, &m_Linear));

	// its own little pool, the application's is sized for what it allocates itself
	VkDescriptorPoolSize poolSizes[] = {
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3 * FRAMES },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, FRAMES },
	};
	VkDescriptorPoolCreateInfo poolCI{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
	poolCI.maxSets = FRAMES;
	poolCI.poolSizeCount = 2;
	poolCI.pPoolSizes = poolSizes;
	VK_CHECK(vkCreateDescriptorPool(m_Device, &poolCI, nullptr, &m_Pool));

	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // 2D source
	builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // array source
	builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // 3D source
	builder.add_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);		   // the copy
	m_Layout = builder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
	for(VkDescriptorSet& set : m_Sets) {
		VkDescriptorSetAllocateInfo allocInfo{ .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
		allocInfo.descriptorPool = m_Pool;
		allocInfo.descriptorSetCount = 1;
		allocInfo.pSetLayouts = &m_Layout;
		VK_CHECK(vkAllocateDescriptorSets(m_Device, &allocInfo, &set));
	}

	VkPushConstantRange range{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(DebugViewPushConstants) };
	VkPipelineLayoutCreateInfo layoutCI = vkinit::pipeline_layout_create_info();
	layoutCI.setLayoutCount = 1;
	layoutCI.pSetLayouts = &m_Layout;
	layoutCI.pushConstantRangeCount = 1;
	layoutCI.pPushConstantRanges = &range;
	VkPipelineLayout layout;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &layoutCI, nullptr, &layout));
	m_Effect = engine->loadComputeShader("src/shaders/bin/debugview.comp.spv", "debug view", layout); // pipeline destroyed by the application

	m_ImGuiTexture = ImGui_ImplVulkan_AddTexture(m_Linear, m_Output.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void DebugView::destroy() {
	ImGui_ImplVulkan_RemoveTexture(m_ImGuiTexture);
	vkDestroyPipelineLayout(m_Device, m_Effect.layout, nullptr);
	vkDestroyDescriptorSetLayout(m_Device, m_Layout, nullptr);
	vkDestroyDescriptorPool(m_Device, m_Pool, nullptr);
	vkDestroySampler(m_Device, m_Linear, nullptr);
	vkDestroySampler(m_Device, m_Nearest, nullptr);
	vkDestroyImageView(m_Device, m_Output.imageView, nullptr);
	vmaDestroyImage(m_Allocator, m_Output.image, m_Output.allocation);
}

void DebugView::applyDefaults(const DebugTexture& texture) {
	m_Mip = 0;
	m_Layer = 0;
	m_Slice = 0.5f;
	m_Min = 0.0f;
	m_Max = 1.0f;
	m_Channels = 0;
	m_Signed = m_Invert = m_DepthDistance = false;
	m_Gamma = true;
	m_Filter = !texture.depthFormat && texture.hint != DebugTexHint::ReverseDepth; // the depth pyramid's texels mean something on their own
	switch(texture.hint) {
		case DebugTexHint::Color: break;
		case DebugTexHint::Hdr: m_Max = 4.0f; break;
		case DebugTexHint::Normal: m_Signed = true; m_Gamma = false; break;
		case DebugTexHint::Depth: m_Channels = 1; m_Gamma = false; break;
		case DebugTexHint::ReverseDepth: m_Channels = 1; m_Gamma = false; m_DepthDistance = true; break;
		case DebugTexHint::Data: m_Gamma = false; break;
		case DebugTexHint::Velocity: m_Channels = 5; m_Signed = true; m_Gamma = false; m_Min = 0.45f; m_Max = 0.55f; break;
	}
	if(texture.channels >= 0) m_Channels = texture.channels;
}

void DebugView::drawImGui(const std::vector<DebugTexture>& textures) {
	if(!m_Open || textures.empty()) return;
	if(!ImGui::Begin("debug textures", &m_Open)) { ImGui::End(); return; }

	m_Selected = std::clamp(m_Selected, 0, static_cast<int>(textures.size()) - 1);
	if(ImGui::BeginCombo("texture", textures[m_Selected].name)) {
		for(int i = 0; i < static_cast<int>(textures.size()); i++) {
			if(ImGui::Selectable(textures[i].name, i == m_Selected)) m_Selected = i;
			if(i == m_Selected) ImGui::SetItemDefaultFocus();
		}
		ImGui::EndCombo();
	}
	const DebugTexture& texture = textures[m_Selected];
	if(m_Defaulted != m_Selected) { applyDefaults(texture); m_Defaulted = m_Selected; }

	ImGui::TextDisabled("%u x %u%s%u, %u mip%s", texture.extent.width, texture.extent.height,
						texture.kind == DebugTexKind::Tex3D ? " x " : (texture.layers > 1 ? ", layers " : ""),
						texture.kind == DebugTexKind::Tex3D ? texture.extent.depth : (texture.layers > 1 ? texture.layers : 0u),
						texture.mips, texture.mips == 1 ? "" : "s");
	if(texture.mips > 1) ImGui::SliderInt("mip", &m_Mip, 0, static_cast<int>(texture.mips) - 1);
	if(texture.layers > 1) {
		m_Layer = std::clamp(m_Layer, 0, static_cast<int>(texture.layers) - 1);
		ImGui::SliderInt("layer", &m_Layer, 0, static_cast<int>(texture.layers) - 1, texture.layerName ? texture.layerName(m_Layer) : "%d");
	}
	if(texture.kind == DebugTexKind::Tex3D) ImGui::SliderFloat("slice", &m_Slice, 0.0f, 1.0f);

	const char* channels[] = { "rgb", "r", "g", "b", "a", "rg" };
	ImGui::Combo("channels", &m_Channels, channels, IM_ARRAYSIZE(channels));
	ImGui::DragFloatRange2("range", &m_Min, &m_Max, std::max((m_Max - m_Min) * 0.005f, 1e-5f), 0.0f, 0.0f, "%.4g", "%.4g");
	ImGui::Checkbox("signed (-1..1)", &m_Signed); ImGui::SameLine();
	ImGui::Checkbox("gamma", &m_Gamma); ImGui::SameLine();
	ImGui::Checkbox("invert", &m_Invert);
	if(texture.hint == DebugTexHint::ReverseDepth) {
		ImGui::Checkbox("depth as distance (log)", &m_DepthDistance);
		ImGui::SetItemTooltip("reversed z camera depth: log2 of the view distance, near plane to far plane");
	}
	if(!texture.depthFormat) {
		if(texture.hint == DebugTexHint::ReverseDepth) ImGui::SameLine();
		ImGui::Checkbox("filtered", &m_Filter);
	}
	if(ImGui::Button("reset")) applyDefaults(texture);
	ImGui::SameLine();
	ImGui::TextDisabled("magenta = NaN / inf");

	// the copy of last frame's pick, its aspect kept
	ImVec2 avail = ImGui::GetContentRegionAvail();
	float aspect = static_cast<float>(m_Region.x) / static_cast<float>(m_Region.y);
	ImVec2 size(avail.x, avail.x / aspect);
	if(size.y > avail.y && avail.y > 64.0f) size = ImVec2(avail.y * aspect, avail.y);
	if(m_OutputReady) ImGui::Image((ImTextureID)m_ImGuiTexture, size, ImVec2(0.0f, 0.0f), ImVec2(m_Region.x / static_cast<float>(SIZE), m_Region.y / static_cast<float>(SIZE)));
	ImGui::End();
}

void DebugView::record(VkCommandBuffer cmd, const std::vector<DebugTexture>& textures, uint32_t frameNumber) {
	if(!m_Open || textures.empty()) return;
	const DebugTexture& texture = textures[std::clamp(m_Selected, 0, static_cast<int>(textures.size()) - 1)];

	if(texture.transitionFrom != VK_IMAGE_LAYOUT_UNDEFINED && texture.transitionFrom != texture.layout)
		vkutil::transition_image(cmd, texture.image, texture.transitionFrom, texture.layout);

	// the longer side fills the copy
	float longest = static_cast<float>(std::max(texture.extent.width, texture.extent.height));
	m_Region = glm::uvec2(std::max(1u, static_cast<uint32_t>(texture.extent.width * SIZE / longest)),
						  std::max(1u, static_cast<uint32_t>(texture.extent.height * SIZE / longest)));

	// this frame slot's set was last used FRAMES frames ago, its fence has been waited on
	VkSampler sampler = (m_Filter && !texture.depthFormat) ? m_Linear : m_Nearest;
	auto pick = [&](DebugTexKind kind, VkImageView fallback) {
		return texture.kind == kind ? VkDescriptorImageInfo{ sampler, texture.view, texture.layout }
									: VkDescriptorImageInfo{ m_Nearest, fallback, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }; // (the shadow array is depth)
	};
	VkDescriptorImageInfo infos[4] = {
		pick(DebugTexKind::Tex2D, m_Fallback2D),
		pick(DebugTexKind::Array, m_FallbackArray),
		pick(DebugTexKind::Tex3D, m_Fallback3D),
		{ VK_NULL_HANDLE, m_Output.imageView, VK_IMAGE_LAYOUT_GENERAL },
	};
	VkDescriptorSet set = m_Sets[frameNumber % FRAMES];
	VkWriteDescriptorSet writes[4];
	for(uint32_t i = 0; i < 4; i++) {
		writes[i] = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		writes[i].dstSet = set;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = i == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[i].pImageInfo = &infos[i];
	}
	vkUpdateDescriptorSets(m_Device, 4, writes, 0, nullptr);

	DebugViewPushConstants pc{};
	pc.mode = glm::ivec4(static_cast<int>(texture.kind), m_Channels,
						 (m_Signed ? FLAG_SIGNED : 0) | (m_Gamma ? FLAG_GAMMA : 0) | (m_Invert ? FLAG_INVERT : 0) | (m_DepthDistance && texture.hint == DebugTexHint::ReverseDepth ? FLAG_DEPTH_DISTANCE : 0), 0);
	pc.range = glm::vec4(m_Min, m_Max, static_cast<float>(std::min<uint32_t>(m_Mip, texture.mips - 1)),
						 texture.kind == DebugTexKind::Tex3D ? m_Slice : static_cast<float>(std::min<uint32_t>(m_Layer, texture.layers - 1)));
	pc.extra = glm::vec4(m_Near, m_Far, static_cast<float>(m_Region.x), static_cast<float>(m_Region.y));

	vkutil::transition_image(cmd, m_Output.image, m_OutputReady ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Effect.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Effect.layout, 0, 1, &set, 0, nullptr);
	vkCmdPushConstants(cmd, m_Effect.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	vkCmdDispatch(cmd, (m_Region.x + 7) / 8, (m_Region.y + 7) / 8, 1);
	vkutil::transition_image(cmd, m_Output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	m_OutputReady = true;
}
