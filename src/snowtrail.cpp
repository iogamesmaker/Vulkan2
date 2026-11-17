#include "snowtrail.hpp"
#include "application.hpp"
#include <vk_images.h>
#include <vk_initializers.h>
#include <vk_descriptors.h>

// the stride and the feet's shapes (metres)
static constexpr double STRIDE = 0.72;
static constexpr double FOOT_SPACING = 0.11;  // each foot this far to the side of the path
static constexpr double FOOT_HALF_LENGTH = 0.15, FOOT_HALF_WIDTH = 0.065;
static constexpr double FURROW_HALF_WIDTH = 0.2, FURROW_PRESS = 0.3; // the legs wading through: a shallow, rounded trough
static constexpr double TELEPORT = 3.0; // further than this in a frame: no furrow across the gap

void SnowTrail::init(Application* engine, VkDescriptorSet terrainSet) {
	m_Engine = engine;
	m_Device = engine->m_Device;
	m_Image = vkutil::create_image(m_Device, engine->getAllocator(), VK_FORMAT_R8G8B8A8_UNORM,
								   VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, { SIZE, SIZE, 1 });
	m_View = m_Image.imageView;

	VkSamplerCreateInfo samplerCI = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	samplerCI.magFilter = VK_FILTER_LINEAR;
	samplerCI.minFilter = VK_FILTER_LINEAR;
	samplerCI.addressModeU = samplerCI.addressModeV = samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT; // toroidal
	VK_CHECK(vkCreateSampler(m_Device, &samplerCI, nullptr, &m_Sampler));

	m_InfoBuffer = engine->createBuffer(sizeof(Info), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

	// the stamp pass: the image as storage
	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	m_StampLayout = builder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
	m_StampSet = engine->g_DescriptorAllocator.allocate(m_Device, m_StampLayout);
	VkDescriptorImageInfo storageInfo{ VK_NULL_HANDLE, m_View, VK_IMAGE_LAYOUT_GENERAL };
	VkWriteDescriptorSet write{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	write.dstSet = m_StampSet;
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	write.pImageInfo = &storageInfo;
	vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);

	VkPushConstantRange range{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(StampPushConstants) };
	VkPipelineLayoutCreateInfo layoutCI = vkinit::pipeline_layout_create_info();
	layoutCI.setLayoutCount = 1;
	layoutCI.pSetLayouts = &m_StampLayout;
	layoutCI.pushConstantRangeCount = 1;
	layoutCI.pPushConstantRanges = &range;
	VkPipelineLayout layout;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &layoutCI, nullptr, &layout));
	m_Effect = engine->loadComputeShader("src/shaders/bin/snowstamp.comp.spv", "snow stamps", layout); // pipeline destroyed by the application

	// the terrain's side: the info and the texture (in GENERAL, it's written and read in turn every frame)
	VkDescriptorBufferInfo infoBuffer{ m_InfoBuffer.buffer, 0, sizeof(Info) };
	VkDescriptorImageInfo sampledInfo{ m_Sampler, m_View, VK_IMAGE_LAYOUT_GENERAL };
	VkWriteDescriptorSet writes[2] = {};
	for(int i = 0; i < 2; i++) {
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = terrainSet;
		writes[i].dstBinding = 9 + i;
		writes[i].descriptorCount = 1;
	}
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	writes[0].pBufferInfo = &infoBuffer;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[1].pImageInfo = &sampledInfo;
	vkUpdateDescriptorSets(m_Device, 2, writes, 0, nullptr);

	// empty, and the info off until the first frame
	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		vkutil::transition_image(cmd, m_Image.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
		VkClearColorValue zero{};
		VkImageSubresourceRange all{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdClearColorImage(cmd, m_Image.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
		Info info{};
		vkCmdUpdateBuffer(cmd, m_InfoBuffer.buffer, 0, sizeof(Info), &info);
	});
}

void SnowTrail::destroy() {
	if(!m_Engine) return;
	vkDestroyImageView(m_Device, m_Image.imageView, nullptr);
	vmaDestroyImage(m_Engine->getAllocator(), m_Image.image, m_Image.allocation);
	vkDestroySampler(m_Device, m_Sampler, nullptr);
	m_Engine->destroyBuffer(m_InfoBuffer);
	vkDestroyDescriptorSetLayout(m_Device, m_StampLayout, nullptr);
	vkDestroyPipelineLayout(m_Device, m_Effect.layout, nullptr);
}

void SnowTrail::update(glm::dvec3 feetWorld, bool on, double metresPerUnit) {
	glm::dvec2 feet = glm::dvec2(feetWorld.x, feetWorld.z) * metresPerUnit; // metres
	if(!on) { m_HaveLast = false; return; }
	if(!m_HaveLast) { m_LastFeet = feet; m_HaveLast = true; m_Walked = 0.0; return; }
	glm::dvec2 delta = feet - m_LastFeet;
	double moved = glm::length(delta);
	if(moved > TELEPORT) { m_LastFeet = feet; m_Walked = 0.0; return; }
	if(moved < 1e-4) return;
	glm::dvec2 dir = delta / moved;
	auto toTexels = [](glm::dvec2 metres) { return metres / TEXEL_METRES; };
	// the legs' furrow along the way since the last frame
	m_Pending.push_back({ toTexels(m_LastFeet), toTexels(feet), FURROW_HALF_WIDTH / TEXEL_METRES, 0.0, FURROW_PRESS });
	// a print every stride, alternating feet either side of the path, where the stride ended
	m_Walked += moved;
	while(m_Walked >= STRIDE) {
		m_Walked -= STRIDE;
		glm::dvec2 at = feet - dir * m_Walked;
		glm::dvec2 side = glm::dvec2(-dir.y, dir.x) * (m_LeftFoot ? FOOT_SPACING : -FOOT_SPACING);
		m_Pending.push_back({ toTexels(at + side), dir, FOOT_HALF_WIDTH / TEXEL_METRES, FOOT_HALF_LENGTH / TEXEL_METRES, 1.0 });
		m_LeftFoot = !m_LeftFoot;
	}
	m_LastFeet = feet;
	if(m_Pending.size() > 64) m_Pending.erase(m_Pending.begin(), m_Pending.end() - 64); // (a stall: the newest)
}

void SnowTrail::dispatch(VkCommandBuffer cmd, const StampPushConstants& pc) {
	vkCmdPushConstants(cmd, m_Effect.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	vkCmdDispatch(cmd, (pc.boxSize.x + 7) / 8, (pc.boxSize.y + 7) / 8, 1);
	// the next stamp reads what this one wrote (they overlap along the furrow)
	vkutil::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
						  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
}

void SnowTrail::record(VkCommandBuffer cmd, glm::dvec2 cameraWorld, double metresPerUnit, bool enabled) {
	glm::dvec2 camTexel = cameraWorld * metresPerUnit / TEXEL_METRES;
	long long centreX = static_cast<long long>(std::llround(camTexel.x / SNAP)) * SNAP;
	long long centreY = static_cast<long long>(std::llround(camTexel.y / SNAP)) * SNAP;

	bool work = !m_Pending.empty() || !m_HaveCentre || centreX != m_CentreX || centreY != m_CentreY;
	if(work) {
		// the last frame's terrain is done reading it
		vkutil::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
							  VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Effect.pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Effect.layout, 0, 1, &m_StampSet, 0, nullptr);
		StampPushConstants clear{};
		clear.mode = 1;
		if(!m_HaveCentre) {
			clear.boxSize = glm::ivec2(SIZE);
			dispatch(cmd, clear);
		} else {
			// the columns / rows coming into the window held prints from the last time it was there (or never): cleared
			long long dx = centreX - m_CentreX, dy = centreY - m_CentreY;
			if(dx != 0) {
				long long start = dx > 0 ? m_CentreX + SIZE / 2 : centreX - SIZE / 2;
				clear.boxMin = glm::ivec2(wrap(start), 0);
				clear.boxSize = glm::ivec2(static_cast<int>(std::min<long long>(std::llabs(dx), SIZE)), SIZE);
				dispatch(cmd, clear);
			}
			if(dy != 0) {
				long long start = dy > 0 ? m_CentreY + SIZE / 2 : centreY - SIZE / 2;
				clear.boxMin = glm::ivec2(0, wrap(start));
				clear.boxSize = glm::ivec2(SIZE, static_cast<int>(std::min<long long>(std::llabs(dy), SIZE)));
				dispatch(cmd, clear);
			}
		}
		m_CentreX = centreX;
		m_CentreY = centreY;
		m_HaveCentre = true;

		for(const Stamp& s : m_Pending) {
			bool print = s.halfLength > 0.0;
			double reach = 2.0 * (print ? s.halfLength : s.halfWidth) + 2.0; // the rim reaches twice the shape's size
			glm::dvec2 lo = print ? s.a - reach : glm::min(s.a, s.b) - reach;
			glm::dvec2 hi = print ? s.a + reach : glm::max(s.a, s.b) + reach;
			// (only inside the window: anything else would land on texels holding another place's prints)
			if(hi.x < centreX - SIZE / 2 || lo.x > centreX + SIZE / 2 || hi.y < centreY - SIZE / 2 || lo.y > centreY + SIZE / 2) continue;
			glm::dvec2 origin = glm::floor(lo);
			StampPushConstants pc{};
			pc.boxMin = glm::ivec2(wrap(static_cast<long long>(origin.x)), wrap(static_cast<long long>(origin.y)));
			pc.boxSize = glm::ivec2(glm::min(glm::ceil(hi - origin), glm::dvec2(SIZE)));
			pc.a = glm::vec2(s.a - origin);
			pc.b = print ? glm::vec2(s.b) : glm::vec2(s.b - origin);
			pc.halfWidth = static_cast<float>(s.halfWidth);
			pc.halfLength = static_cast<float>(s.halfLength);
			pc.pressed = static_cast<float>(s.pressed);
			dispatch(cmd, pc);
		}
		m_Pending.clear();
		// this frame's terrain samples what was pressed
		vkutil::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
							  VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
	}

	// where the camera is in the texture (terrain.tese / terrain.frag work relative to it: small, precise numbers)
	Info info{};
	glm::dvec2 wrapped = glm::mod(camTexel, glm::dvec2(SIZE));
	info.camera = glm::vec4(glm::vec2(wrapped), static_cast<float>(metresPerUnit / TEXEL_METRES), enabled ? 1.0f : 0.0f);
	info.window = glm::vec4(static_cast<float>(camTexel.x - static_cast<double>(centreX)), static_cast<float>(camTexel.y - static_cast<double>(centreY)),
							static_cast<float>(SIZE), static_cast<float>(1.0 / metresPerUnit));
	vkutil::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_UNIFORM_READ_BIT,
						  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
	vkCmdUpdateBuffer(cmd, m_InfoBuffer.buffer, 0, sizeof(Info), &info);
	vkutil::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
						  VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_UNIFORM_READ_BIT);
}
