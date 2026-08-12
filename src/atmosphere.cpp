#include "atmosphere.hpp"
#include "application.hpp"

void Atmosphere::init(VkDevice device, VmaAllocator allocator, Application* engine) {
	m_Device = device;
	m_Allocator = allocator;
	// init basic data
	
	transmittance.imageExtent = {256, 64, 1};
	transmittance.imageFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
	
	multiscattering.imageExtent = {32, 32, 1};
	multiscattering.imageFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
	
	skyview.imageExtent = {192, 108, 1};
	skyview.imageFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
	
	aerial.imageExtent = {32, 32, 32};
	aerial.imageFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
	
	// create LUT images
	
	VkImageUsageFlags drawImageUsages{};
	drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	drawImageUsages |= VK_IMAGE_USAGE_STORAGE_BIT;
	drawImageUsages |= VK_IMAGE_USAGE_SAMPLED_BIT;

	VmaAllocationCreateInfo imageAllocinfo = {};
	imageAllocinfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	imageAllocinfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	
	VkImageCreateInfo imageCI;
	VkImageViewCreateInfo iviewCI;
	// transmittance
	imageCI	= vkinit::image_create_info(transmittance.imageFormat, drawImageUsages, transmittance.imageExtent);
	vmaCreateImage(m_Allocator, &imageCI, &imageAllocinfo, &transmittance.image, &transmittance.allocation, nullptr);
	iviewCI = vkinit::imageview_create_info(transmittance.imageFormat, transmittance.image, VK_IMAGE_ASPECT_COLOR_BIT);
	
	VK_CHECK(vkCreateImageView(m_Device, &iviewCI, nullptr, &transmittance.imageView));
	// multiscattering
	imageCI	= vkinit::image_create_info(multiscattering.imageFormat, drawImageUsages, multiscattering.imageExtent);
	vmaCreateImage(m_Allocator, &imageCI, &imageAllocinfo, &multiscattering.image, &multiscattering.allocation, nullptr);

	iviewCI = vkinit::imageview_create_info(multiscattering.imageFormat, multiscattering.image, VK_IMAGE_ASPECT_COLOR_BIT);
	
	VK_CHECK(vkCreateImageView(m_Device, &iviewCI, nullptr, &multiscattering.imageView));
	// skyview
	imageCI	= vkinit::image_create_info(skyview.imageFormat, drawImageUsages, skyview.imageExtent);
	vmaCreateImage(m_Allocator, &imageCI, &imageAllocinfo, &skyview.image, &skyview.allocation, nullptr);
	
	iviewCI = vkinit::imageview_create_info(skyview.imageFormat, skyview.image, VK_IMAGE_ASPECT_COLOR_BIT);
	
	VK_CHECK(vkCreateImageView(m_Device, &iviewCI, nullptr, &skyview.imageView));
	// aerial
	imageCI	= vkinit::image_create_info(aerial.imageFormat, drawImageUsages, aerial.imageExtent);
	vmaCreateImage(m_Allocator, &imageCI, &imageAllocinfo, &aerial.image, &aerial.allocation, nullptr);
	
	iviewCI = vkinit::imageview_create_info(aerial.imageFormat, aerial.image, VK_IMAGE_ASPECT_COLOR_BIT);
	
	VK_CHECK(vkCreateImageView(m_Device, &iviewCI, nullptr, &aerial.imageView));
	
	// allocate the UBO
	
	VkBufferCreateInfo bufferCI{};
	bufferCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
	bufferCI.size = sizeof(AtmosphereUBO);
	bufferCI.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
	
	VmaAllocationCreateInfo vmaCI{};
	vmaCI.usage = VMA_MEMORY_USAGE_AUTO;
	vmaCI.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
	
	VmaAllocationInfo allocInfo;
	
	vmaCreateBuffer(m_Allocator, &bufferCI, &vmaCI, &m_AtmosphereUBO.buffer, &m_AtmosphereUBO.allocation, &allocInfo);
	
	// create descriptor sets for the LUTs
	// transmittance
	DescriptorLayoutBuilder transmittancebuilder;
	transmittancebuilder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE); // write transmittance
	m_TransmittanceDescriptorLayout = transmittancebuilder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
	
	m_TransmittanceDescriptor = engine->g_DescriptorAllocator.allocate(m_Device, m_TransmittanceDescriptorLayout);
	// multiscattering
	DescriptorLayoutBuilder multiscatteringbuilder;
	multiscatteringbuilder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE); // write multiscattering
	multiscatteringbuilder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // read transmittance
	m_MultiscatteringDescriptorLayout = multiscatteringbuilder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
	
	m_MultiscatteringDescriptor = engine->g_DescriptorAllocator.allocate(m_Device, m_MultiscatteringDescriptorLayout);
	// skyview
	DescriptorLayoutBuilder skyviewbuilder;
	skyviewbuilder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE); // write skyview
	skyviewbuilder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // read transmittance
	skyviewbuilder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // read multiscattering
	m_SkyviewDescriptorLayout = skyviewbuilder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
	
	m_SkyviewDescriptor = engine->g_DescriptorAllocator.allocate(m_Device, m_SkyviewDescriptorLayout);
	// aerial
	DescriptorLayoutBuilder aerialbuilder;
	aerialbuilder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE); // write aerial
	m_AerialDescriptorLayout = aerialbuilder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
	
	m_AerialDescriptor = engine->g_DescriptorAllocator.allocate(m_Device, m_AerialDescriptorLayout);
	
	// create descriptor sets for UBO
	DescriptorLayoutBuilder ubobuilder;
	ubobuilder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	m_UBODescriptorLayout = ubobuilder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT); // the frag shader will read from the same UBO as the LUT compute shaders
	
	m_UBODescriptor = engine->g_DescriptorAllocator.allocate(m_Device, m_UBODescriptorLayout);
	
	// create the compute shader pipeline
	/*
	VkPipelineLayoutCreateInfo computeLayoutCI{};
		
	computeLayoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	computeLayoutCI.pNext = nullptr;
	computeLayoutCI.pSetLayouts = &m_LUTDescriptorLayout;
	computeLayoutCI.setLayoutCount = 1;
	
	VK_CHECK(vkCreatePipelineLayout(m_Device, &computeLayoutCI, nullptr, &m_HeightmapLayout));
	
	// create the shaders to generate LUTs
	VkDescriptorImageInfo heightmapImgInfo{};
	
	heightmapImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	heightmapImgInfo.imageView = m_Heightmap.imageView;
	
	VkWriteDescriptorSet heightmapWrite = {};
	heightmapWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	heightmapWrite.pNext = nullptr;
	heightmapWrite.dstBinding = 0;
	heightmapWrite.dstSet = m_HeightmapDescriptors;
	heightmapWrite.descriptorCount = 1;
	heightmapWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	heightmapWrite.pImageInfo = &heightmapImgInfo;
	
	vkUpdateDescriptorSets(m_Device, 1, &heightmapWrite, 0, nullptr);
	
	
	ComputeEffect  = loadComputeShader("src/shaders/bin/heightmap.comp.spv", "heightmap shader");*/
}

void Atmosphere::updateUBO(const AtmosphereUBO& newUBO) {
	VmaAllocationInfo info;
	vmaGetAllocationInfo(m_Allocator, m_AtmosphereUBO.allocation, &info);
	
	memcpy(info.pMappedData, &newUBO, sizeof(AtmosphereUBO));
}