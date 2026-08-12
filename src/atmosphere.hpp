#ifndef ATMOSPHERE_HPP
#define ATMOSPHERE_HPP

#include <vk_types.h>

class Application;

struct AtmosphereUBO {
	glm::vec3 sunDir;	
};

class Atmosphere {
public:	
	void init(VkDevice device, VmaAllocator allocator, Application* engine);
	
	void updateUBO(const AtmosphereUBO& newUBO);
	
	AtmosphereUBO m_AtmosphereData;
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
	
	ComputeEffect m_TransmittanceEffect;
	ComputeEffect m_SkyviewEffect;
	ComputeEffect m_MultiscatteringEffect;
	ComputeEffect m_AerialEffect;

	AllocatedImage skyview;
	AllocatedImage transmittance;
	AllocatedImage multiscattering;
	AllocatedImage aerial;
};

#endif