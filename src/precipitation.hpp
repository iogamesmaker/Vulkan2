#pragma once
// rain, snow, blowing sand and leaves blown off the trees in a forest (precipitation.vert / .frag): instanced quads in a box around the camera, no buffers,
// drawn onto the window resolution output after the TAA (so the streaks don't ghost), depth tested by hand against
// the render resolution depth buffer. how much of each comes from the weather (weather.hpp)
#include <vk_types.h>

class Application;

struct PrecipitationPushConstants {
	glm::mat4 viewProj;
	glm::vec3 campos;
	float time;
	glm::vec2 offsetWorld;
	glm::vec2 wind;
	glm::ivec4 counts; // x rain, y snow, z sand, w leaves
	glm::vec4 view;	   // xy output size, z projScale
	glm::vec3 sundir;
	float standingEye = 0.0f; // the ground under the camera + the eye height (the near-ground particles don't jump with you)
	glm::vec4 motion{ 0.0f }; // xyz = the camera's velocity, world units / s
	glm::ivec4 counts2{ 0 };  // the wind's: x motes (pollen, seed fluff, dust in the sun), y unused (0), z spindrift, w 0
};

class Precipitation {
public:
	static constexpr uint32_t MAX_RAIN = 60000, MAX_SNOW = 30000, MAX_SAND = 40000, MAX_LEAVES = 2500;
	static constexpr uint32_t MAX_MOTES = 1600, MAX_DRIFT = 4500; // what the wind carries (counts2)

	void init(Application* engine, VkFormat outputFormat, VkDescriptorSetLayout atmosphereLayout);
	// after every resize: the depth buffer it tests against (SHADER_READ_ONLY when it draws)
	void setDepth(VkImageView depth, VkSampler nearest);
	// outside of rendering, the output image in GENERAL. nothing when nothing falls
	void draw(VkCommandBuffer cmd, const PrecipitationPushConstants& pc, VkDescriptorSet atmosphereSet, VkImageView output, VkExtent2D extent);
	void destroy();

private:
	VkDevice m_Device = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_Layout = VK_NULL_HANDLE;
	VkDescriptorSet m_Set = VK_NULL_HANDLE;
	VkPipelineLayout m_PipelineLayout = VK_NULL_HANDLE;
	VkPipeline m_Pipeline = VK_NULL_HANDLE;
};
