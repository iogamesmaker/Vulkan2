#include "atmosphere.hpp"
#include "application.hpp"
#include <glm/trigonometric.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/common.hpp>

void Atmosphere::destroy() {
	vkDestroyPipelineLayout(m_Device, m_TransmittanceEffect.layout, nullptr);
	vkDestroyPipelineLayout(m_Device, m_MultiscatteringEffect.layout, nullptr);
	vkDestroyPipelineLayout(m_Device, m_SkyviewEffect.layout, nullptr);
	vkDestroyPipelineLayout(m_Device, m_AerialEffect.layout, nullptr);
	vkDestroyPipelineLayout(m_Device, m_PostEffect.layout, nullptr);

	vkDestroyPipelineLayout(m_Device, m_SkyLayout, nullptr);
	vkDestroyPipeline(m_Device, m_SkyPipeline, nullptr);

	vmaDestroyBuffer(m_Allocator, m_AtmosphereUBO.buffer, m_AtmosphereUBO.allocation);
	vmaDestroyBuffer(m_Allocator, m_ExposureBuffer.buffer, m_ExposureBuffer.allocation);

	vkDestroyImageView(m_Device, transmittance.imageView, nullptr);
	vmaDestroyImage(m_Allocator, transmittance.image, transmittance.allocation);

	vkDestroyImageView(m_Device, multiscattering.imageView, nullptr);
	vmaDestroyImage(m_Allocator, multiscattering.image, multiscattering.allocation);

	vkDestroyImageView(m_Device, skyview.imageView, nullptr);
	vmaDestroyImage(m_Allocator, skyview.image, skyview.allocation);

	vkDestroyImageView(m_Device, aerial.imageView, nullptr);
	vmaDestroyImage(m_Allocator, aerial.image, aerial.allocation);
	vkDestroyImageView(m_Device, aerialSun.imageView, nullptr);
	vmaDestroyImage(m_Allocator, aerialSun.image, aerialSun.allocation);

	vkDestroyDescriptorSetLayout(m_Device, m_TransmittanceDescriptorLayout, nullptr);
	vkDestroyDescriptorSetLayout(m_Device, m_MultiscatteringDescriptorLayout, nullptr);
	vkDestroyDescriptorSetLayout(m_Device, m_SkyviewDescriptorLayout, nullptr);
	vkDestroyDescriptorSetLayout(m_Device, m_AerialDescriptorLayout, nullptr);
	vkDestroyDescriptorSetLayout(m_Device, m_SkyDescriptorLayout, nullptr);
	vkDestroyDescriptorSetLayout(m_Device, m_PostDescriptorLayout, nullptr);
	vkDestroyDescriptorSetLayout(m_Device, m_UBODescriptorLayout, nullptr);
}

void Atmosphere::init(VkDevice device, VmaAllocator allocator, Application* engine) {
	m_Device = device;
	m_Allocator = allocator;
	m_Engine = engine;
	// init basic data

	transmittance.imageExtent = {256, 64, 1};
	transmittance.imageFormat = VK_FORMAT_R32G32B32A32_SFLOAT;

	multiscattering.imageExtent = {32, 32, 1};
	multiscattering.imageFormat = VK_FORMAT_R32G32B32A32_SFLOAT;

	// these two get rebuilt every frame so half floats save some bandwidth
	skyview.imageExtent = {192, 108, 1};
	skyview.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

	aerial.imageExtent = {32, 32, 32};
	aerial.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
	aerialSun.imageExtent = aerial.imageExtent;
	aerialSun.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

	// and in the UBO

	m_AtmosphereData.radiusbottom = 6360.0f; // radius of earth in km
	m_AtmosphereData.radiustop	  = 6420.0f; // + 60km atmosphere
	m_AtmosphereData.worldScale	  = 0.006f;	 // 6m per world unit
	m_AtmosphereData.baseAltitude = 0.2f;
	m_AtmosphereData.textureRepeatNear = 0.6f;
	m_AtmosphereData.textureRepeatFar = 3.0f;
	m_AtmosphereData.terrainCache = 1.0f;

	// set by the application in setTerrainSize()
	m_AtmosphereData.terrainSize = 2048.0f;
	m_AtmosphereData.fogEnd		 = 950.0f;
	m_AtmosphereData.fogStart	 = 0.0f;

	setDefaults();

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

	iviewCI = vkinit::imageview_create_info(aerial.imageFormat, aerial.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_3D);

	VK_CHECK(vkCreateImageView(m_Device, &iviewCI, nullptr, &aerial.imageView));
	imageCI	= vkinit::image_create_info(aerialSun.imageFormat, drawImageUsages, aerialSun.imageExtent);
	vmaCreateImage(m_Allocator, &imageCI, &imageAllocinfo, &aerialSun.image, &aerialSun.allocation, nullptr);
	iviewCI = vkinit::imageview_create_info(aerialSun.imageFormat, aerialSun.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_3D);
	VK_CHECK(vkCreateImageView(m_Device, &iviewCI, nullptr, &aerialSun.imageView));

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
	transmittancebuilder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);	 // write transmittance
	m_TransmittanceDescriptorLayout = transmittancebuilder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);

	m_TransmittanceDescriptor = engine->g_DescriptorAllocator.allocate(m_Device, m_TransmittanceDescriptorLayout);
	// multiscattering
	DescriptorLayoutBuilder multiscatteringbuilder;
	multiscatteringbuilder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);		  // write multiscattering
	multiscatteringbuilder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // read transmittance
	m_MultiscatteringDescriptorLayout = multiscatteringbuilder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);

	m_MultiscatteringDescriptor = engine->g_DescriptorAllocator.allocate(m_Device, m_MultiscatteringDescriptorLayout);
	// skyview
	DescriptorLayoutBuilder skyviewbuilder;
	skyviewbuilder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);		  // write skyview
	skyviewbuilder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // read transmittance
	skyviewbuilder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // read multiscattering
	m_SkyviewDescriptorLayout = skyviewbuilder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);

	m_SkyviewDescriptor = engine->g_DescriptorAllocator.allocate(m_Device, m_SkyviewDescriptorLayout);
	// aerial
	DescriptorLayoutBuilder aerialbuilder;
	aerialbuilder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE); 		  // write aerial
	aerialbuilder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // read transmittance
	aerialbuilder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // read multiscattering
	aerialbuilder.add_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);		  // write aerial sun (its single scattering part)
	m_AerialDescriptorLayout = aerialbuilder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);

	m_AerialDescriptor = engine->g_DescriptorAllocator.allocate(m_Device, m_AerialDescriptorLayout);

	// sky + terrain
	DescriptorLayoutBuilder skybuilder;
	skybuilder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // read transmittance
	skybuilder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // read multiscattering
	skybuilder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // read skyview
	skybuilder.add_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // read aerial
	skybuilder.add_binding(4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);		  // atmosphere UBO
	skybuilder.add_binding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // moon colour map (setMoonTexture)
	skybuilder.add_binding(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // cirrus noise lattice (setCirrusNoise)
	skybuilder.add_binding(7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // integrated volumetric fog (setVolumetricFog)
	skybuilder.add_binding(8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // read aerial sun (lighting.comp shadows it)
	m_SkyDescriptorLayout = skybuilder.build(m_Device, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT); // compute: lighting.comp

	m_SkyDescriptor = engine->g_DescriptorAllocator.allocate(m_Device, m_SkyDescriptorLayout);

	// post processing
	DescriptorLayoutBuilder postbuilder;
	postbuilder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);		   // draw image, read + write
	postbuilder.add_binding(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);		   // atmosphere UBO
	postbuilder.add_binding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);		   // auto exposure state (exposure.comp)
	postbuilder.add_binding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // bloom (setBloom)
	m_PostDescriptorLayout = postbuilder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);

	m_PostDescriptor = engine->g_DescriptorAllocator.allocate(m_Device, m_PostDescriptorLayout);

	// create descriptor sets for UBO
	DescriptorLayoutBuilder ubobuilder;
	ubobuilder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	m_UBODescriptorLayout = ubobuilder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);

	m_UBODescriptor = engine->g_DescriptorAllocator.allocate(m_Device, m_UBODescriptorLayout);

	// set ubo info
	VkDescriptorBufferInfo uboInfo{};
	uboInfo.buffer = m_AtmosphereUBO.buffer;
	uboInfo.offset = 0;
	uboInfo.range = sizeof(AtmosphereUBO);

	VkWriteDescriptorSet uboWrite = {};
	uboWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	uboWrite.dstSet = m_UBODescriptor;
	uboWrite.dstBinding = 0;
	uboWrite.descriptorCount = 1;
	uboWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
	uboWrite.pBufferInfo = &uboInfo;

	vkUpdateDescriptorSets(m_Device, 1, &uboWrite, 0, nullptr);

	uboWrite.dstSet = m_SkyDescriptor;
	uboWrite.dstBinding = 4;

	vkUpdateDescriptorSets(m_Device, 1, &uboWrite, 0, nullptr);

	uboWrite.dstSet = m_PostDescriptor;
	uboWrite.dstBinding = 3;

	vkUpdateDescriptorSets(m_Device, 1, &uboWrite, 0, nullptr);

	// set image read and write info
	VkDescriptorImageInfo imageInfo{};
	imageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

	VkWriteDescriptorSet imageWrite = {};
	imageWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	imageWrite.pNext = nullptr;
	imageWrite.dstBinding = 0;
	imageWrite.descriptorCount = 1;
	imageWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;

	VkDescriptorImageInfo sampleInfo{};
	sampleInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	sampleInfo.sampler = engine->m_AtmosphereSampler;

	// transmittance
	imageInfo.imageView = transmittance.imageView;
	imageWrite.pImageInfo = &imageInfo;
	imageWrite.dstSet = m_TransmittanceDescriptor;
	imageWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	imageWrite.dstBinding = 0;
	vkUpdateDescriptorSets(m_Device, 1, &imageWrite, 0, nullptr); // BINDING 0: WRITE TRANSMITTANCE
	// multiscattering											------------------
	imageInfo.imageView = multiscattering.imageView;
	imageWrite.pImageInfo = &imageInfo;
	imageWrite.dstSet = m_MultiscatteringDescriptor;
	imageWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	imageWrite.dstBinding = 0;
	vkUpdateDescriptorSets(m_Device, 1, &imageWrite, 0, nullptr); // BINDING 0: WRITE MULTISCATTERING

	sampleInfo.imageView = transmittance.imageView;
	imageWrite.pImageInfo = &sampleInfo;
	imageWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	imageWrite.dstSet = m_MultiscatteringDescriptor;
	imageWrite.dstBinding = 1;
	vkUpdateDescriptorSets(m_Device, 1, &imageWrite, 0, nullptr); // BINDING 1: READ TRANSMITTANCE
	// skyview													------------------
	imageInfo.imageView = skyview.imageView;
	imageWrite.pImageInfo = &imageInfo;
	imageWrite.dstSet = m_SkyviewDescriptor;
	imageWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	imageWrite.dstBinding = 0;
	vkUpdateDescriptorSets(m_Device, 1, &imageWrite, 0, nullptr); // BINDING 0: WRITE SKYVIEW

	sampleInfo.imageView = transmittance.imageView;
	imageWrite.pImageInfo = &sampleInfo;
	imageWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	imageWrite.dstSet = m_SkyviewDescriptor;
	imageWrite.dstBinding = 1;
	vkUpdateDescriptorSets(m_Device, 1, &imageWrite, 0, nullptr); // BINDING 1: READ TRANSMITTANCE

	sampleInfo.imageView = multiscattering.imageView;
	imageWrite.pImageInfo = &sampleInfo;
	imageWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	imageWrite.dstSet = m_SkyviewDescriptor;
	imageWrite.dstBinding = 2;
	vkUpdateDescriptorSets(m_Device, 1, &imageWrite, 0, nullptr); // BINDING 2: READ MULTISCATTERING
	// aerial
	imageInfo.imageView = aerial.imageView;
	imageWrite.pImageInfo = &imageInfo;
	imageWrite.dstSet = m_AerialDescriptor;
	imageWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	imageWrite.dstBinding = 0;
	vkUpdateDescriptorSets(m_Device, 1, &imageWrite, 0, nullptr); // BINDING 0: WRITE AERIAL

	sampleInfo.imageView = transmittance.imageView;
	imageWrite.pImageInfo = &sampleInfo;
	imageWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	imageWrite.dstSet = m_AerialDescriptor;
	imageWrite.dstBinding = 1;
	vkUpdateDescriptorSets(m_Device, 1, &imageWrite, 0, nullptr); // BINDING 1: READ TRANSMITTANCE

	sampleInfo.imageView = multiscattering.imageView;
	imageWrite.pImageInfo = &sampleInfo;
	imageWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	imageWrite.dstSet = m_AerialDescriptor;
	imageWrite.dstBinding = 2;
	vkUpdateDescriptorSets(m_Device, 1, &imageWrite, 0, nullptr); // BINDING 2: READ MULTISCATTERING

	imageInfo.imageView = aerialSun.imageView;
	imageWrite.pImageInfo = &imageInfo;
	imageWrite.dstSet = m_AerialDescriptor;
	imageWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	imageWrite.dstBinding = 3;
	vkUpdateDescriptorSets(m_Device, 1, &imageWrite, 0, nullptr); // BINDING 3: WRITE AERIAL SUN
	// sky
	sampleInfo.imageView = transmittance.imageView;
	imageWrite.pImageInfo = &sampleInfo;
	imageWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	imageWrite.dstSet = m_SkyDescriptor;
	imageWrite.dstBinding = 0;
	vkUpdateDescriptorSets(m_Device, 1, &imageWrite, 0, nullptr); // BINDING 0: READ TRANSMITTANCE

	sampleInfo.imageView = multiscattering.imageView;
	imageWrite.pImageInfo = &sampleInfo;
	imageWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	imageWrite.dstSet = m_SkyDescriptor;
	imageWrite.dstBinding = 1;
	vkUpdateDescriptorSets(m_Device, 1, &imageWrite, 0, nullptr); // BINDING 1: READ MULTISCATTERING

	sampleInfo.imageView = skyview.imageView;
	imageWrite.pImageInfo = &sampleInfo;
	imageWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	imageWrite.dstSet = m_SkyDescriptor;
	imageWrite.dstBinding = 2;
	vkUpdateDescriptorSets(m_Device, 1, &imageWrite, 0, nullptr); // BINDING 2: READ SKYVIEW

	sampleInfo.imageView = aerial.imageView;
	imageWrite.pImageInfo = &sampleInfo;
	imageWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	imageWrite.dstSet = m_SkyDescriptor;
	imageWrite.dstBinding = 3;
	vkUpdateDescriptorSets(m_Device, 1, &imageWrite, 0, nullptr); // BINDING 3: READ AERIAL

	sampleInfo.imageView = aerialSun.imageView;
	imageWrite.pImageInfo = &sampleInfo;
	imageWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	imageWrite.dstSet = m_SkyDescriptor;
	imageWrite.dstBinding = 8;
	vkUpdateDescriptorSets(m_Device, 1, &imageWrite, 0, nullptr); // BINDING 8: READ AERIAL SUN
	setRenderTargets(engine->m_OutputImage); // post processing runs on the output image (binding 0)

	// create the compute shader pipelines

	VkDescriptorSetLayout layouts[] = {
		m_TransmittanceDescriptorLayout,
		m_UBODescriptorLayout
	};

	VkPipelineLayoutCreateInfo computeLayoutCI{};

	computeLayoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	computeLayoutCI.pNext = nullptr;
	computeLayoutCI.setLayoutCount = 2;
	computeLayoutCI.pSetLayouts = layouts;

	VkPipelineLayout transmittanceLayout, multiscatteringLayout, skyviewLayout, aerialLayout;

	VK_CHECK(vkCreatePipelineLayout(m_Device, &computeLayoutCI, nullptr, &transmittanceLayout));

	layouts[0] = m_MultiscatteringDescriptorLayout;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &computeLayoutCI, nullptr, &multiscatteringLayout));

	// the per frame LUTs get the camera + sun through push constants
	VkPushConstantRange pushConstantRange{};
	pushConstantRange.offset = 0;
	pushConstantRange.size = sizeof(AtmospherePushConstants);
	pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

	computeLayoutCI.pPushConstantRanges = &pushConstantRange;
	computeLayoutCI.pushConstantRangeCount = 1;

	layouts[0] = m_SkyviewDescriptorLayout;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &computeLayoutCI, nullptr, &skyviewLayout));

	layouts[0] = m_AerialDescriptorLayout;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &computeLayoutCI, nullptr, &aerialLayout));

	VkPipelineLayout postLayout;
	pushConstantRange.size = sizeof(PostPushConstants);
	computeLayoutCI.setLayoutCount = 1;
	computeLayoutCI.pSetLayouts = &m_PostDescriptorLayout;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &computeLayoutCI, nullptr, &postLayout));

	m_TransmittanceEffect = engine->loadComputeShader("src/shaders/bin/transmittancelut.comp.spv", "transmittance lut compute shader", transmittanceLayout);
	m_MultiscatteringEffect = engine->loadComputeShader("src/shaders/bin/multiscatterlut.comp.spv", "multiscattering lut compute shader", multiscatteringLayout);
	m_SkyviewEffect = engine->loadComputeShader("src/shaders/bin/skyviewlut.comp.spv", "sky view lut compute shader", skyviewLayout);
	m_AerialEffect = engine->loadComputeShader("src/shaders/bin/aeriallut.comp.spv", "aerial perspective lut compute shader", aerialLayout);
	m_PostEffect = engine->loadComputeShader("src/shaders/bin/post.comp.spv", "tonemapping compute shader", postLayout);
	m_ExposureEffect = engine->loadComputeShader("src/shaders/bin/exposure.comp.spv", "auto exposure", postLayout); // pipeline layout is shared, destroyed with the post one

	m_ExposureBuffer = engine->createBuffer(sizeof(glm::vec4), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
	memset(m_ExposureBuffer.info.pMappedData, 0, sizeof(glm::vec4)); // 0 = take the first metering as is
	VkDescriptorBufferInfo exposureInfo{ m_ExposureBuffer.buffer, 0, sizeof(glm::vec4) };
	VkWriteDescriptorSet exposureWrite{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	exposureWrite.dstSet = m_PostDescriptor;
	exposureWrite.dstBinding = 4;
	exposureWrite.descriptorCount = 1;
	exposureWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	exposureWrite.pBufferInfo = &exposureInfo;
	vkUpdateDescriptorSets(m_Device, 1, &exposureWrite, 0, nullptr);

	// build the transmittance LUT (THANK YOU GOAT https://www.shadertoy.com/view/DsBGWG)

	updateUBO(m_AtmosphereData);
	buildStaticLUTs();

	computeSunDir();

	// the per frame LUTs are only ever read in SHADER_READ_ONLY_OPTIMAL outside of update()
	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		vkutil::transition_image(cmd, skyview.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		vkutil::transition_image(cmd, aerial.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		vkutil::transition_image(cmd, aerialSun.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	});
}

void Atmosphere::setDefaults() {
	AtmosphereUBO& d = m_AtmosphereData;

	d.rayleighscale	   = 8.0f;
	d.miescale		   = 1.4f;
	d.rayleigh		   = glm::vec3(5.802, 13.558, 33.100) * glm::vec3(0.001);
	d.mie			   = glm::vec3(0.0047); // scattering, extinction is 1.11x this. ~1.7x the textbook clear day: a humid
											   // summer haze, distance reads as layers of softer, warmer air
	d.mieg			   = 0.8f;
	d.absorbtion	   = glm::vec3(0.000650,0.001881,0.000085);
	d.groundAlbedo	   = glm::vec3(0.16f, 0.17f, 0.12f); // roughly the terrain, it's what you see past the edge of the world

	d.sunIlluminance	= 20.0f;
	d.moonIlluminance	= 0.06f;   // way brighter than the real thing (1/400000 of the sun) so nights are playable
	d.aerialDistance	= 32.0f;
	d.sunAngularRadius	= 0.02f; // ~4x the real sun, it looks tiny otherwise (the shadow penumbras use the real size)
	d.moonAngularRadius = 0.03f;
	d.starBrightness	= 0.05f;

	d.fogDensity = 2.0f;
	d.fogStart	 = d.fogEnd * 0.65f; // the last third, most of it in the last few hundred metres (fogDensity)

	m_ExposureEV		 = 0.0f;
}

void Atmosphere::setTerrainSize(float worldSize, float renderDistance, float textureRepeatNear, float textureRepeatFar) {
	float fogStartFraction = m_AtmosphereData.fogEnd > 0.0f ? m_AtmosphereData.fogStart / m_AtmosphereData.fogEnd : 0.5f;
	m_AtmosphereData.terrainSize = worldSize;
	m_AtmosphereData.textureRepeatNear = textureRepeatNear;
	m_AtmosphereData.textureRepeatFar = textureRepeatFar;
m_AtmosphereData.fogEnd = renderDistance;
	m_AtmosphereData.fogStart = renderDistance * fogStartFraction;
	updateUBO(m_AtmosphereData);
}

void Atmosphere::tick(float deltatime) {
	m_Deltatime = deltatime;
	m_CloudWind += glm::vec2(0.8f, 0.35f) * m_CloudWindSpeed * deltatime; // the same direction the grass sways
	if(m_AnimateTime && m_DayLength > 0.0f) m_TimeOfDay = std::fmod(m_TimeOfDay + deltatime * 24.0f / m_DayLength, 24.0f);
computeSunDir();
}

void Atmosphere::computeSunDir() {
	m_HourAngle = (m_TimeOfDay - 12.0f) / 24.0f * glm::two_pi<float>();
	m_SunDir = sunDirAt(m_TimeOfDay);
}

glm::vec3 Atmosphere::sunDirAt(float timeOfDay) const {
	// position of the sun in the local sky from the hour angle, latitude and declination
	// x = east, y = up, z = north
	float hourAngle = (timeOfDay - 12.0f) / 24.0f * glm::two_pi<float>();
	float lat = glm::radians(m_Latitude);
	float dec = glm::radians(m_Declination);

	glm::vec3 dir = {
		-std::cos(dec) * std::sin(hourAngle),
		 std::sin(lat) * std::sin(dec) + std::cos(lat) * std::cos(dec) * std::cos(hourAngle),
		 std::cos(lat) * std::sin(dec) - std::sin(lat) * std::cos(dec) * std::cos(hourAngle)
	};

	float offset = glm::radians(m_SunAzimuthOffset);
	return glm::normalize(glm::vec3(
		dir.x * std::cos(offset) - dir.z * std::sin(offset),
		dir.y,
		dir.x * std::sin(offset) + dir.z * std::cos(offset)
	));
}

ShadowLightKeys Atmosphere::getShadowLight(float stepDegrees) const {
	// the sun moves 15 degrees per hour along its path
	double stepHours = std::max(stepDegrees, 0.05f) / 15.0;
	double position = m_TimeOfDay / stepHours;

	ShadowLightKeys keys{};
	keys.keyA = static_cast<long long>(std::floor(position));
	keys.keyB = keys.keyA + 1;
	keys.t = static_cast<float>(position - std::floor(position));

	auto lightAt = [&](long long key, float& strength) {
		glm::vec3 sun = sunDirAt(static_cast<float>(key * stepHours));
		glm::vec3 light = sun.y >= 0.0f ? sun : -sun; // the moon is opposite the sun
		strength = glm::smoothstep(0.0f, 0.012f, light.y); // lighting.comp fades the direct light out wider than this
		return light;
	};

	float strengthA, strengthB;
	keys.dirA = lightAt(keys.keyA, strengthA);
	keys.dirB = lightAt(keys.keyB, strengthB);

	// near the horizon (where the light switches between sun and moon) both keyframes have ~0 strength
	float sunHeight = std::abs(m_SunDir.y);
	keys.strength = std::min(glm::mix(strengthA, strengthB, keys.t), glm::smoothstep(0.0f, 0.012f, sunHeight));
	return keys;
}

void Atmosphere::buildStaticLUTs() {
	m_Engine->immediateSubmit([&](VkCommandBuffer cmd) {
		// transmittance
		vkutil::transition_image(cmd, transmittance.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_TransmittanceEffect.pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_TransmittanceEffect.layout, 0, 1, &m_TransmittanceDescriptor, 0, nullptr);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_TransmittanceEffect.layout, 1, 1, &m_UBODescriptor, 0, nullptr);

		vkCmdDispatch(cmd, 32, 8, 1); // 32*8 = 256, 8*8 = 64 :)

		vkutil::transition_image(cmd, transmittance.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

		// multiscattering, doesn't depend on the sun position (it's one of the LUT axes) so this only runs when the parameters change
		vkutil::transition_image(cmd, multiscattering.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_MultiscatteringEffect.pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_MultiscatteringEffect.layout, 0, 1, &m_MultiscatteringDescriptor, 0, nullptr);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_MultiscatteringEffect.layout, 1, 1, &m_UBODescriptor, 0, nullptr);

		vkCmdDispatch(cmd, 4, 4, 1); // 32x32

		vkutil::transition_image(cmd, multiscattering.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	});
}

void Atmosphere::updateUBO(const AtmosphereUBO& newUBO) {
	VmaAllocationInfo info;
	vmaGetAllocationInfo(m_Allocator, m_AtmosphereUBO.allocation, &info);

	memcpy(info.pMappedData, &newUBO, sizeof(AtmosphereUBO));
}

void Atmosphere::setMoonTexture(VkImageView view, VkSampler sampler) {
	VkDescriptorImageInfo info{ sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkWriteDescriptorSet write{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	write.dstSet = m_SkyDescriptor;
	write.dstBinding = 5;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &info;
	vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);
}

// the weather into the UBO every frame (small values that ease slowly, so the frames in flight seeing one or the
// other doesn't matter): the cloud layer, the wind that moves it, and the weather vectors the shaders read
void Atmosphere::setWeather(const WeatherState& w) {
	AtmosphereUBO& d = m_AtmosphereData;
	d.cloudCoverage = w.coverage;
	d.cloudDensity = w.density;
	d.weather = glm::vec4(w.overcast, w.wetness, w.flash, w.snowCover);
	d.weather2 = glm::vec4(w.rain, w.snow, w.sand, w.wind);
	d.weather3 = glm::vec4(w.instability, w.rainDeck, w.rainCells, 0.0f);
	d.weather4 = glm::vec4(w.strikeKm, w.strikeFlash, w.strikeSeed);
	d.cloudAltitude = w.cloudBase; // low grey decks when it's damp (the mountains disappear into them), high in fair weather
	m_CloudWindSpeed = 0.004f + w.wind * 0.03f; // km/s: a breeze .. a gale up at the clouds
	updateUBO(d);
}

void Atmosphere::setVolumetricFog(VkImageView view, VkSampler sampler) {
	VkDescriptorImageInfo info{ sampler, view, VK_IMAGE_LAYOUT_GENERAL }; // written + sampled every frame, never transitioned
	VkWriteDescriptorSet write{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	write.dstSet = m_SkyDescriptor;
	write.dstBinding = 7;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &info;
	vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);
}

void Atmosphere::setCirrusNoise(VkImageView view, VkSampler sampler) {
	VkDescriptorImageInfo info{ sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkWriteDescriptorSet write{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	write.dstSet = m_SkyDescriptor;
	write.dstBinding = 6;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &info;
	vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);
}

void Atmosphere::setRenderTargets(const AllocatedImage& drawImage) {
	VkDescriptorImageInfo storageInfo{};
	storageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	storageInfo.imageView = drawImage.imageView;

	VkWriteDescriptorSet write{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	write.dstSet = m_PostDescriptor;
	write.dstBinding = 0;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	write.pImageInfo = &storageInfo;
	vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);
}

void Atmosphere::update(VkCommandBuffer cmd, const glm::mat4& view, const glm::mat4& projection, glm::vec3 campos, glm::vec2 cameraWorld) {
	glm::mat4 rotationView = view;
	rotationView[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f); // the atmosphere only cares about directions
	glm::mat4 rotationViewProj = projection * rotationView;

	float altitude = std::max(m_AtmosphereData.baseAltitude + campos.y * m_AtmosphereData.worldScale, 0.01f);

	m_PushConstants.invViewProj = glm::inverse(rotationViewProj);
	m_PushConstants.camPos = glm::vec3(0.0f, m_AtmosphereData.radiusbottom + altitude, 0.0f);
	m_PushConstants.sunDir = m_SunDir;
	m_PushConstants.starRotation = m_HourAngle;
	m_PushConstants.latitude = glm::radians(m_Latitude);
	m_PushConstants.cameraWorldKm = cameraWorld * m_AtmosphereData.worldScale;
	m_PushConstants.cloudWind = m_CloudWind;
	m_PushConstants.frame = std::fmod(m_PushConstants.frame + 1.0f, 1024.0f);

	// post: the exposure itself is automatic (exposure.comp), this is the compensation on top
	m_PostPushConstants.exposure = std::exp2(m_ExposureEV);
	m_PostPushConstants.deltaTime = m_Deltatime;
	m_PostPushConstants.bloomStrength = m_BloomStrength;
	m_PostPushConstants.time = std::fmod(m_PostPushConstants.time + m_Deltatime, 60.0f);

	// sky view
	vkutil::transition_image(cmd, skyview.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_SkyviewEffect.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_SkyviewEffect.layout, 0, 1, &m_SkyviewDescriptor, 0, nullptr);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_SkyviewEffect.layout, 1, 1, &m_UBODescriptor, 0, nullptr);
	vkCmdPushConstants(cmd, m_SkyviewEffect.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AtmospherePushConstants), &m_PushConstants);

	vkCmdDispatch(cmd, (skyview.imageExtent.width + 7) / 8, (skyview.imageExtent.height + 7) / 8, 1);

	vkutil::transition_image(cmd, skyview.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	// aerial perspective, one thread per froxel column
	vkutil::transition_image(cmd, aerial.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
	vkutil::transition_image(cmd, aerialSun.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);

	if(m_AerialPerspective) { // off: the shaders don't sample it (ubo.aerialPerspective)
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_AerialEffect.pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_AerialEffect.layout, 0, 1, &m_AerialDescriptor, 0, nullptr);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_AerialEffect.layout, 1, 1, &m_UBODescriptor, 0, nullptr);
		vkCmdPushConstants(cmd, m_AerialEffect.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AtmospherePushConstants), &m_PushConstants);
		vkCmdDispatch(cmd, (aerial.imageExtent.width + 7) / 8, (aerial.imageExtent.height + 7) / 8, 1);
	}

	vkutil::transition_image(cmd, aerial.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	vkutil::transition_image(cmd, aerialSun.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void Atmosphere::postProcess(VkCommandBuffer cmd, VkExtent2D extent, const std::function<void(const char*)>& mark) {
	// meter the HDR image first (one workgroup), then the post pass reads the adapted exposure
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ExposureEffect.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PostEffect.layout, 0, 1, &m_PostDescriptor, 0, nullptr);
	vkCmdPushConstants(cmd, m_PostEffect.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PostPushConstants), &m_PostPushConstants);
	vkCmdDispatch(cmd, 1, 1, 1);
	if(mark) mark("auto exposure");

	VkMemoryBarrier2 barrier{ .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
	barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	barrier.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
	barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
	VkDependencyInfo dependency{ .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	dependency.memoryBarrierCount = 1;
	dependency.pMemoryBarriers = &barrier;
	vkCmdPipelineBarrier2(cmd, &dependency);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_PostEffect.pipeline);

	vkCmdDispatch(cmd, (extent.width + 7) / 8, (extent.height + 7) / 8, 1);
	if(mark) mark("tonemap");
}

void Atmosphere::setBloom(VkImageView view, VkSampler sampler) {
	VkDescriptorImageInfo info{ sampler, view, VK_IMAGE_LAYOUT_GENERAL };
	VkWriteDescriptorSet write{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	write.dstSet = m_PostDescriptor;
	write.dstBinding = 5;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	write.pImageInfo = &info;
	vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);
}

glm::vec2 Atmosphere::getExposureState() const {
	const float* state = static_cast<const float*>(m_ExposureBuffer.info.pMappedData);
	return glm::vec2(state[1], state[0]);
}

void Atmosphere::draw(VkCommandBuffer cmd) {
	vkutil::bindGraphics(cmd, m_SkyPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_SkyLayout, 0, 1, &m_SkyDescriptor, 0, nullptr);
	vkCmdPushConstants(cmd, m_SkyLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(AtmospherePushConstants), &m_PushConstants);
	vkCmdDraw(cmd, 3, 1, 0, 0);
}

void Atmosphere::drawImGui() {
	bool rebuild = false; // physical parameters changed -> rebuild transmittance + multiscattering
	bool changed = false; // only the UBO needs updating

	AtmosphereUBO& d = m_AtmosphereData;

	ImGui::SeparatorText("Time");

	ImGui::SliderFloat("Time of day", &m_TimeOfDay, 0.0f, 24.0f, "%.2f h");
	ImGui::Checkbox("Animate", &m_AnimateTime);
	ImGui::SameLine();
	ImGui::SliderFloat("Day length (s)", &m_DayLength, 10.0f, 3600.0f, "%.0f", ImGuiSliderFlags_Logarithmic);
	ImGui::SliderFloat("Latitude", &m_Latitude, -89.0f, 89.0f, "%.1f deg");
	ImGui::SliderFloat("Season", &m_Declination, -23.44f, 23.44f, "%.1f deg");

	ImGui::SliderFloat("Exposure (EV)", &m_ExposureEV, -3.0f, 3.0f, "%+.1f"); // on top of the automatic exposure
	// (the weather below drives the clouds, the wind and the sky now: Application::drawWeather)

	if(rebuild || changed) {
		// the UBO is shared with the frames in flight, so let them finish first
		std::lock_guard<std::mutex> lock(m_Engine->m_QueueMutex); // (the loading screen may be presenting)
		vkDeviceWaitIdle(m_Device);
		updateUBO(d);
		if(rebuild) buildStaticLUTs();
	}
}

void Atmosphere::initSkyPipeline(Application* engine) {
	CompiledShader   vertexShader = engine->loadShader("src/shaders/bin/fullscreen.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
	CompiledShader fragmentShader = engine->loadShader("src/shaders/bin/sky.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);

	VkPushConstantRange pushConstantRange{};
	pushConstantRange.offset = 0;
	pushConstantRange.size = sizeof(AtmospherePushConstants);
	pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

	VkPipelineLayoutCreateInfo pipelineLayoutInfo = vkinit::pipeline_layout_create_info();
	pipelineLayoutInfo.pSetLayouts = &m_SkyDescriptorLayout;
	pipelineLayoutInfo.setLayoutCount = 1;
	pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;
	pipelineLayoutInfo.pushConstantRangeCount = 1;

	VK_CHECK(vkCreatePipelineLayout(m_Device, &pipelineLayoutInfo, nullptr, &m_SkyLayout));

	PipelineBuilder pipelineBuilder;

	pipelineBuilder._pipelineLayout = m_SkyLayout;
	pipelineBuilder.set_shaders(vertexShader.shader, fragmentShader.shader);
	pipelineBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	pipelineBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
	pipelineBuilder.set_cull_mode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_CLOCKWISE);

	pipelineBuilder.set_multisampling_none();
	pipelineBuilder.disable_blending();
	pipelineBuilder.enable_depthtest(false, VK_COMPARE_OP_EQUAL); // only where the terrain didn't draw (depth still 0, reversed z)
	pipelineBuilder._noWireframe = true; // a fullscreen pass, not geometry

	pipelineBuilder.set_color_attachment_format(engine->m_DrawImage.imageFormat);
	pipelineBuilder.add_color_attachment(DIRECT_LIGHT_FORMAT); // sun / moon light + shadow, see ShadowFilter
	pipelineBuilder.set_depth_format(engine->m_DepthBuffer.imageFormat);

	m_SkyPipeline = pipelineBuilder.build_pipeline(m_Device);

	vkDestroyShaderModule(m_Device, vertexShader.shader, nullptr);
	vkDestroyShaderModule(m_Device, fragmentShader.shader, nullptr);
}
