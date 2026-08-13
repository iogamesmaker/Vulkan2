#ifndef ATMOSPHERE_HPP
#define ATMOSPHERE_HPP

#include <vk_types.h>

class Application;

struct AtmosphereUBO {
	glm::vec3 sunDir;	
};

class Atmosphere {
public:	
	~Atmosphere();
	void init(VkDevice device, VmaAllocator allocator, Application* engine);
	void initSkyPipeline(Application* engine);
	
	void draw(VkCommandBuffer cmd);
	
	void update(glm::vec2 sunpos);
	void updateUBO(const AtmosphereUBO& newUBO);
	
	AtmosphereUBO m_AtmosphereData;
	AllocatedImage transmittance;
	AllocatedImage multiscattering;
	AllocatedImage skyview;
	AllocatedImage aerial;
	
	glm::vec2 sunpos;
private:
	VkDevice m_Device;
	VmaAllocator m_Allocator;
	
	AllocatedBuffer m_AtmosphereUBO; // stores VMA data + VkBuffer
	
	VkDescriptorSetLayout m_UBODescriptorLayout;
	VkDescriptorSet m_UBODescriptor;

	VkDescriptorSetLayout m_SkyviewDescriptorLayout;
	VkDescriptorSet m_SkyviewDescriptor;

	VkDescriptorSetLayout m_TransmittanceDescriptorLayout;
	VkDescriptorSet m_TransmittanceDescriptor;

	VkDescriptorSetLayout m_MultiscatteringDescriptorLayout;
	VkDescriptorSet m_MultiscatteringDescriptor;

	VkDescriptorSetLayout m_AerialDescriptorLayout;
	VkDescriptorSet m_AerialDescriptor;
	
	VkPipeline m_SkyPipeline;
	VkPipelineLayout m_SkyLayout;
	
	VkDescriptorSetLayout m_SkyDescriptorLayout;
	VkDescriptorSet m_SkyDescriptor;
	
	ComputeEffect m_TransmittanceEffect;
	ComputeEffect m_SkyviewEffect;
	ComputeEffect m_MultiscatteringEffect;
	ComputeEffect m_AerialEffect;
	
	/*
	struct ComputeEffect {
		std::string name;
		
		VkPipeline pipeline;
		VkPipelineLayout layout;
	};
	*/

};

#endif