#include "atmosphere.hpp"
#include "application.hpp"

Atmosphere::~Atmosphere() {
	vkDestroyPipelineLayout(m_Device, m_TransmittanceEffect.layout, nullptr);
	vkDestroyPipeline(m_Device, m_TransmittanceEffect.pipeline, nullptr);

	vkDestroyPipelineLayout(m_Device, m_MultiscatteringEffect.layout, nullptr);
	vkDestroyPipeline(m_Device, m_MultiscatteringEffect.pipeline, nullptr);

	vkDestroyPipelineLayout(m_Device, m_SkyviewEffect.layout, nullptr);
	vkDestroyPipeline(m_Device, m_SkyviewEffect.pipeline, nullptr);

	vkDestroyPipelineLayout(m_Device, m_AerialEffect.layout, nullptr);
	vkDestroyPipeline(m_Device, m_AerialEffect.pipeline, nullptr);

	vkDestroyPipelineLayout(m_Device, m_SkyLayout, nullptr);
	vkDestroyPipeline(m_Device, m_SkyPipeline, nullptr);
	
	vmaDestroyBuffer(m_Allocator, m_AtmosphereUBO.buffer, m_AtmosphereUBO.allocation);
	
	vkDestroyImageView(m_Device, transmittance.imageView, nullptr);
	vmaDestroyImage(m_Allocator, transmittance.image, transmittance.allocation);
	
	vkDestroyImageView(m_Device, multiscattering.imageView, nullptr);
	vmaDestroyImage(m_Allocator, multiscattering.image, multiscattering.allocation);
	
	vkDestroyImageView(m_Device, skyview.imageView, nullptr);
	vmaDestroyImage(m_Allocator, skyview.image, skyview.allocation);
	
	vkDestroyImageView(m_Device, aerial.imageView, nullptr);
	vmaDestroyImage(m_Allocator, aerial.image, aerial.allocation);
	
	vkDestroyDescriptorSetLayout(m_Device, m_TransmittanceDescriptorLayout, nullptr);
	vkDestroyDescriptorSetLayout(m_Device, m_MultiscatteringDescriptorLayout, nullptr);
	vkDestroyDescriptorSetLayout(m_Device, m_SkyviewDescriptorLayout, nullptr);
	vkDestroyDescriptorSetLayout(m_Device, m_AerialDescriptorLayout, nullptr);
	vkDestroyDescriptorSetLayout(m_Device, m_UBODescriptorLayout, nullptr);
}

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
	
	iviewCI = vkinit::imageview_create_info(aerial.imageFormat, aerial.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_VIEW_TYPE_3D);
	
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
	m_AerialDescriptorLayout = aerialbuilder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
	
	m_AerialDescriptor = engine->g_DescriptorAllocator.allocate(m_Device, m_AerialDescriptorLayout);
	
	// sky
	DescriptorLayoutBuilder skybuilder;
	skybuilder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // read transmittance
	skybuilder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // read multiscattering
	skybuilder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // read skyview
	m_SkyDescriptorLayout = skybuilder.build(m_Device, VK_SHADER_STAGE_FRAGMENT_BIT);
	
	m_SkyDescriptor = engine->g_DescriptorAllocator.allocate(m_Device, m_SkyDescriptorLayout);
	
	// create descriptor sets for UBO
	DescriptorLayoutBuilder ubobuilder;
	ubobuilder.add_binding(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
	m_UBODescriptorLayout = ubobuilder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_FRAGMENT_BIT); // the frag shader will read from the same UBO as the LUT compute shaders
	
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
	sampleInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	sampleInfo.sampler = engine->m_Sampler;
	
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
	
	// create the compute shader pipelines

	VkPipelineLayoutCreateInfo computeLayoutCI{};
		
	computeLayoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	computeLayoutCI.pNext = nullptr;
	computeLayoutCI.setLayoutCount = 1;
	
	computeLayoutCI.pSetLayouts = &m_TransmittanceDescriptorLayout;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &computeLayoutCI, nullptr, &m_TransmittanceEffect.layout));
	
	computeLayoutCI.pSetLayouts = &m_MultiscatteringDescriptorLayout;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &computeLayoutCI, nullptr, &m_MultiscatteringEffect.layout));
	
	computeLayoutCI.pSetLayouts = &m_SkyviewDescriptorLayout;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &computeLayoutCI, nullptr, &m_SkyviewEffect.layout));
	
	computeLayoutCI.pSetLayouts = &m_AerialDescriptorLayout;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &computeLayoutCI, nullptr, &m_AerialEffect.layout));
	
	m_TransmittanceEffect = engine->loadComputeShader("src/shaders/bin/transmittancelut.comp.spv", "transmittance lut compute shader", m_TransmittanceEffect.layout);
}

void Atmosphere::updateUBO(const AtmosphereUBO& newUBO) {
	VmaAllocationInfo info;
	vmaGetAllocationInfo(m_Allocator, m_AtmosphereUBO.allocation, &info);
	
	memcpy(info.pMappedData, &newUBO, sizeof(AtmosphereUBO));
}

void Atmosphere::update(glm::vec2 sunpos) {
	m_AtmosphereData.sunDir = glm::normalize(glm::vec3(
		std::cos(sunpos.y) * std::sin(sunpos.x),
		std::sin(sunpos.y),
		std::cos(sunpos.y) * std::cos(sunpos.x)
	));
}

void Atmosphere::draw(VkCommandBuffer cmd) {
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_SkyPipeline);
		
	vkCmdDraw(cmd, 3, 1, 0, 0);
}

void Atmosphere::initSkyPipeline(Application* engine) {
	CompiledShader   vertexShader = engine->loadShader("src/shaders/bin/fullscreen.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
	CompiledShader fragmentShader = engine->loadShader("src/shaders/bin/sky.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
	
	VkPipelineLayoutCreateInfo pipelineLayoutInfo = vkinit::pipeline_layout_create_info();
	VkDescriptorSetLayout skyLayouts[] = { m_SkyDescriptorLayout, m_UBODescriptorLayout };
	pipelineLayoutInfo.pSetLayouts = skyLayouts;
	pipelineLayoutInfo.setLayoutCount = 2;
	
	VK_CHECK(vkCreatePipelineLayout(m_Device, &pipelineLayoutInfo, nullptr, &m_SkyLayout));
	
	PipelineBuilder pipelineBuilder;
		
	pipelineBuilder._pipelineLayout = m_SkyLayout;
	pipelineBuilder.set_shaders(vertexShader.shader, fragmentShader.shader);
	pipelineBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	pipelineBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
	pipelineBuilder.set_cull_mode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_CLOCKWISE);
	
	pipelineBuilder.set_multisampling_none();
	pipelineBuilder.disable_blending();
	pipelineBuilder.enable_depthtest(false, VK_COMPARE_OP_EQUAL);
	
	pipelineBuilder.set_color_attachment_format(engine->m_DrawImage.imageFormat);
	pipelineBuilder.set_depth_format(engine->m_DepthBuffer.imageFormat);
	
	m_SkyPipeline = pipelineBuilder.build_pipeline(m_Device);
}
