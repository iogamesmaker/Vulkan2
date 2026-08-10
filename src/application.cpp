// Application.cpp
#define VMA_IMPLEMENTATION
#include <glm/gtx/transform.hpp>
#include "vk_mem_alloc.h"

#include "application.hpp"

constexpr bool useValidationLayers = true; // ?

Application::Application(uint32_t width, uint32_t height) : m_Width(width), m_Height(height) {
	initWindow();

	initVulkan();
	initSwapchain();
	initCommands();
	initSyncStructures();
	
	initDescriptors();
	initPipelines();
	
	initTerrainPatches();
	
	initImGui();
	
	initDefaultData();
		
	m_InitDone = true;
}

Application::~Application() {
	if(m_InitDone) {
		vkDeviceWaitIdle(m_Device);
		
		for(int i = 0; i < FRAME_OVERLAP; i++) {
			vkDestroyCommandPool(m_Device, m_Frames[i].commandPool, nullptr);
			
			vkDestroyFence(m_Device, m_Frames[i].renderFence, nullptr);
			vkDestroySemaphore(m_Device, m_Frames[i].renderSemaphore, nullptr);
			vkDestroySemaphore(m_Device, m_Frames[i].swapchainSemaphore, nullptr);
			
			m_Frames[i].deletionQueue.flush();
		}
		
		destroySwapchain();
		
		for(auto& mesh : testMeshes) {
			destroyBuffer(mesh->meshBuffers.indexBuffer);
			destroyBuffer(mesh->meshBuffers.vertexBuffer);
		}
		m_DeletionQueue.flush();
	}
	
	SDL_Vulkan_UnloadLibrary();
	if (m_pWindow) {
        SDL_DestroyWindow(m_pWindow);
    }
    SDL_Quit();
	std::cout << "successfully shut down" << std::endl;
}

void Application::initWindow() {
	#if defined(PLATFORM_LINUX)
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11"); // wayland does NOT play nice, x11 works on most wayland systems
	#endif
	
	if(!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(SDL_GetError());
	if (!SDL_Vulkan_LoadLibrary(NULL)) throw std::runtime_error(SDL_GetError());
	m_pWindow = SDL_CreateWindow("Vulkan Attempt 2", m_Width, m_Height, SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
	if(!m_pWindow) throw std::runtime_error(SDL_GetError());
}

void Application::initVulkan() {
	vkb::InstanceBuilder builder; // Vulkan Bootstrapper my goat
	
	auto instance_return = builder.set_app_name("Vulkan Attempt 2")
								  .request_validation_layers(useValidationLayers)
								  .use_default_debug_messenger()
								  .require_api_version(1, 3, 0)
								  .build();
								  
    vkb::Instance vkb_instance = instance_return.value();
	
	m_Instance = vkb_instance.instance;
	m_Debug = vkb_instance.debug_messenger;
	
	SDL_Vulkan_CreateSurface(m_pWindow, m_Instance, NULL, &m_Surface);

	// 1.3 features
	VkPhysicalDeviceVulkan13Features features13 { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
	features13.dynamicRendering = true;
	features13.synchronization2 = true;
	
	// 1.2 features
	VkPhysicalDeviceVulkan12Features features12 { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
	features12.bufferDeviceAddress = true;
	features12.descriptorIndexing = true;
	
	// Base features
	VkPhysicalDeviceFeatures features10 {};
	features10.tessellationShader = VK_TRUE;
	
	
	// GPU selection
	vkb::PhysicalDeviceSelector selector { vkb_instance };
	vkb::PhysicalDevice physicalDevice = selector
										 .set_minimum_version(1, 3)
										 .set_required_features_13(features13)
										 .set_required_features_12(features12)
										 .set_required_features(features10)
										 .set_surface(m_Surface)
										 .select()
										 .value();
										 
	vkb::DeviceBuilder deviceBuilder{ physicalDevice };
	vkb::Device vkbDevice = deviceBuilder.build().value();
	
	m_Device = vkbDevice.device;
	m_GPU = physicalDevice.physical_device;
	
	// Get a graphics queue going
	m_GraphicsQueue = vkbDevice.get_queue(vkb::QueueType::graphics).value();
	m_GraphicsQueueFamily = vkbDevice.get_queue_index(vkb::QueueType::graphics).value();
	
	// Initialise VMA
	VmaAllocatorCreateInfo allocatorInfo = {};
	allocatorInfo.physicalDevice = m_GPU;
	allocatorInfo.device = m_Device;
	allocatorInfo.instance = m_Instance;
	allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	
	vmaCreateAllocator(&allocatorInfo, &m_Allocator);
	
	m_DeletionQueue.push([&]() {
		vmaDestroyAllocator(m_Allocator);
	});
}

void Application::initSwapchain() {
	createSwapchain(m_Width, m_Height);
	
	VkExtent3D drawImageExtent = { m_Width, m_Height, 1 };
	
	m_DrawImage.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
	m_DrawImage.imageExtent = drawImageExtent;
	
	VkImageUsageFlags drawImageUsages{};
	drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	//drawImageUsages |= VK_IMAGE_USAGE_STORAGE_BIT;
	drawImageUsages |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	
	VkImageCreateInfo imageCI = vkinit::image_create_info(m_DrawImage.imageFormat, drawImageUsages, drawImageExtent);

	VmaAllocationCreateInfo imageAllocinfo = {};
	imageAllocinfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	imageAllocinfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	
	vmaCreateImage(m_Allocator, &imageCI, &imageAllocinfo, &m_DrawImage.image, &m_DrawImage.allocation, nullptr);

	VkImageViewCreateInfo rview_info = vkinit::imageview_create_info(m_DrawImage.imageFormat, m_DrawImage.image, VK_IMAGE_ASPECT_COLOR_BIT);
	
	VK_CHECK(vkCreateImageView(m_Device, &rview_info, nullptr, &m_DrawImage.imageView));
	
	// depth buffer creation
	
	m_DepthBuffer.imageFormat = VK_FORMAT_D32_SFLOAT;
	m_DepthBuffer.imageExtent = drawImageExtent;
	
	VkImageUsageFlags depthBufferUsages{};
	depthBufferUsages |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	
	VkImageCreateInfo depthBufferCI = vkinit::image_create_info(m_DepthBuffer.imageFormat, depthBufferUsages, drawImageExtent);
	
	vmaCreateImage(m_Allocator, &depthBufferCI, &imageAllocinfo, &m_DepthBuffer.image, &m_DepthBuffer.allocation, nullptr);
	
	VkImageViewCreateInfo depthViewCI = vkinit::imageview_create_info(m_DepthBuffer.imageFormat, m_DepthBuffer.image, VK_IMAGE_ASPECT_DEPTH_BIT);
	
	VK_CHECK(vkCreateImageView(m_Device, &depthViewCI, nullptr, &m_DepthBuffer.imageView));
}

void Application::initCommands() {
	VkCommandPoolCreateInfo commandPoolInfo = {};
	commandPoolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	commandPoolInfo.pNext = nullptr;
	commandPoolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	commandPoolInfo.queueFamilyIndex = m_GraphicsQueueFamily;
	
	for(int i = 0; i < FRAME_OVERLAP; i++) {
		// VK_CHECK makes sure the command inside succeeds, otherwise it'll abort immediately.
		VK_CHECK(vkCreateCommandPool(m_Device, &commandPoolInfo, nullptr, &m_Frames[i].commandPool));
		
		VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info(m_Frames[i].commandPool, 1); // nice little shortcut by vkguide.dev's abstraction
		
		VK_CHECK(vkAllocateCommandBuffers(m_Device, &cmdAllocInfo, &m_Frames[i].commandBuffer));
	}
	
	VK_CHECK(vkCreateCommandPool(m_Device, &commandPoolInfo, nullptr, &m_ImmCommandPool));
	
	VkCommandBufferAllocateInfo cmdAllocInfo = vkinit::command_buffer_allocate_info(m_ImmCommandPool, 1);
	
	VK_CHECK(vkAllocateCommandBuffers(m_Device, &cmdAllocInfo, &m_ImmCommandBuffer));
	
	m_DeletionQueue.push([&]() {
		vkDestroyCommandPool(m_Device, m_ImmCommandPool, nullptr);
	});
}

void Application::initSyncStructures() {
	VkFenceCreateInfo fenceCreateInfo = vkinit::fence_create_info(VK_FENCE_CREATE_SIGNALED_BIT);
	VkSemaphoreCreateInfo semaphoreCreateInfo = vkinit::semaphore_create_info();
	
	for(int i = 0; i < FRAME_OVERLAP; i++) {
		VK_CHECK(vkCreateFence(m_Device, &fenceCreateInfo, nullptr, &m_Frames[i].renderFence));
		
		VK_CHECK(vkCreateSemaphore(m_Device, &semaphoreCreateInfo, nullptr, &m_Frames[i].renderSemaphore));
		VK_CHECK(vkCreateSemaphore(m_Device, &semaphoreCreateInfo, nullptr, &m_Frames[i].swapchainSemaphore));
	}
	
	VK_CHECK(vkCreateFence(m_Device, &fenceCreateInfo, nullptr, &m_ImmFence));
	m_DeletionQueue.push([&]() {
		vkDestroyFence(m_Device, m_ImmFence, nullptr);
	});
}

void Application::initDescriptors() {
	std::vector<DescriptorAllocator::PoolSizeRatio> sizes = {
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 }
	};
	
	g_DescriptorAllocator.init_pool(m_Device, 10, sizes);
		
	VkSamplerCreateInfo samplerCI = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	
	samplerCI.magFilter = VK_FILTER_LINEAR;
	samplerCI.minFilter = VK_FILTER_LINEAR;
	
	vkCreateSampler(m_Device, &samplerCI, nullptr, &m_Sampler);
	
	samplerCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	
	vkCreateSampler(m_Device, &samplerCI, nullptr, &m_HeightmapSampler);
		
	m_DeletionQueue.push([&]() {
		vkDestroySampler(m_Device, m_Sampler, nullptr);
		vkDestroySampler(m_Device, m_HeightmapSampler, nullptr);
		
		g_DescriptorAllocator.destroy_pool(m_Device);
	});
}

ComputeEffect Application::loadComputeShader(std::string path, std::string name) {
	CompiledShader computeShader = loadShader(path, VK_SHADER_STAGE_COMPUTE_BIT);
	
	VkComputePipelineCreateInfo computePipelineCreateInfo{};
	computePipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	computePipelineCreateInfo.pNext = nullptr;
	computePipelineCreateInfo.layout = m_ComputeLayout;
	computePipelineCreateInfo.stage = computeShader.stageInfo;
	
	ComputeEffect computeEffect;
	computeEffect.layout = m_ComputeLayout;
	computeEffect.name = name;
	
	VK_CHECK(vkCreateComputePipelines(m_Device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &computeEffect.pipeline));

	vkDestroyShaderModule(m_Device, computeShader.shader, nullptr);
	
	VkPipeline pipeline = computeEffect.pipeline;
	m_DeletionQueue.push([this, pipeline]() {
		vkDestroyPipeline(m_Device, pipeline, nullptr);
	});

	return computeEffect;
}

void Application::initComputePipelines() {
	// create descriptor set layout for heightmap
	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	m_HeightmapDescriptorLayout = builder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
	
	m_HeightmapDescriptors = g_DescriptorAllocator.allocate(m_Device, m_HeightmapDescriptorLayout);
	// create the compute shader pipeline
	VkPipelineLayoutCreateInfo computeLayoutCI{};
		
	computeLayoutCI.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	computeLayoutCI.pNext = nullptr;
	computeLayoutCI.pSetLayouts = &m_HeightmapDescriptorLayout;
	computeLayoutCI.setLayoutCount = 1;
	
	VkPushConstantRange pushConstantRange{};
	
	pushConstantRange.offset = 0;
	pushConstantRange.size = sizeof(HeightmapPushConstants);
	pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	
	computeLayoutCI.pPushConstantRanges = &pushConstantRange;
	computeLayoutCI.pushConstantRangeCount = 1;
	
	VK_CHECK(vkCreatePipelineLayout(m_Device, &computeLayoutCI, nullptr, &m_ComputeLayout));
	
	m_DeletionQueue.push([&]() {
		vkDestroyPipelineLayout(m_Device, m_ComputeLayout, nullptr);
		vkDestroyDescriptorSetLayout(m_Device, m_HeightmapDescriptorLayout, nullptr);
	});
	
	// create heightmap image

	VkExtent3D heightmapExtent = {static_cast<uint32_t>(m_HeightmapSize), static_cast<uint32_t>(m_HeightmapSize), 1}; // Resolution of world data thing
	
	m_Heightmap.imageFormat = VK_FORMAT_R32G32B32A32_SFLOAT;
	m_Heightmap.imageExtent = heightmapExtent ;
	
	VkImageUsageFlags drawImageUsages{};
	drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	drawImageUsages |= VK_IMAGE_USAGE_STORAGE_BIT;
	drawImageUsages |= VK_IMAGE_USAGE_SAMPLED_BIT;
	
	VkImageCreateInfo imageCI = vkinit::image_create_info(m_Heightmap.imageFormat, drawImageUsages, heightmapExtent);

	VmaAllocationCreateInfo imageAllocinfo = {};
	imageAllocinfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	imageAllocinfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	
	vmaCreateImage(m_Allocator, &imageCI, &imageAllocinfo, &m_Heightmap.image, &m_Heightmap.allocation, nullptr);

	VkImageViewCreateInfo rview_info = vkinit::imageview_create_info(m_Heightmap.imageFormat, m_Heightmap.image, VK_IMAGE_ASPECT_COLOR_BIT);
	
	VK_CHECK(vkCreateImageView(m_Device, &rview_info, nullptr, &m_Heightmap.imageView));
	
	// create the compute shader to generate heightmap
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
	
	
	m_HeightmapPC.offset = {0,0};
	m_HeightmapPC.dirtyMin = {0,0};
	m_HeightmapEffect = loadComputeShader("src/shaders/bin/heightmap.comp.spv", "heightmap shader");
	
	m_DeletionQueue.push([&]() {
		vkDestroyImageView(m_Device, m_Heightmap.imageView, nullptr);
		vmaDestroyImage(m_Allocator, m_Heightmap.image, m_Heightmap.allocation);
	});
}

CompiledShader Application::loadShader(std::string path, VkShaderStageFlagBits stageBits) {
	VkShaderModule shader{};
	
	std::string shaderPath = util::getpath(path);
	if(!vkutil::load_shader_module(shaderPath.c_str(), m_Device, &shader)) { fmt::print("Failed to create shader {}\n", shaderPath); }
	
	VkPipelineShaderStageCreateInfo stageInfo{};
    stageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stageInfo.pNext = nullptr;
    stageInfo.stage = stageBits;
    stageInfo.module = shader;
    stageInfo.pName = "main";
	
	return { shader, stageInfo };
}

void Application::initMainPipeline() {
	CompiledShader   vertexShader = loadShader("src/shaders/bin/vertex.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
	CompiledShader fragmentShader = loadShader("src/shaders/bin/fragment.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
	
	VkPushConstantRange bufferRange{};
	bufferRange.offset = 0;
	bufferRange.size = sizeof(GPUDrawPushConstants);
	bufferRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
	
	VkPipelineLayoutCreateInfo pipelineLayoutInfo = vkinit::pipeline_layout_create_info();
	pipelineLayoutInfo.pPushConstantRanges = &bufferRange;
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	
	VK_CHECK(vkCreatePipelineLayout(m_Device, &pipelineLayoutInfo, nullptr, &m_MainLayout));
	
	PipelineBuilder pipelineBuilder;
	
	pipelineBuilder._pipelineLayout = m_MainLayout;
	pipelineBuilder.set_shaders(vertexShader.shader, fragmentShader.shader);
	pipelineBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	pipelineBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
	pipelineBuilder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	
	pipelineBuilder.set_multisampling_none();
	pipelineBuilder.disable_blending();
	pipelineBuilder.enable_depthtest(true, VK_COMPARE_OP_GREATER_OR_EQUAL);
	
	pipelineBuilder.set_color_attachment_format(m_DrawImage.imageFormat);
	pipelineBuilder.set_depth_format(m_DepthBuffer.imageFormat);
	
	m_MainPipeline = pipelineBuilder.build_pipeline(m_Device);
	
	vkDestroyShaderModule(m_Device, fragmentShader.shader, nullptr);
	vkDestroyShaderModule(m_Device, vertexShader.shader, nullptr);
	
	m_DeletionQueue.push([&]() {
		vkDestroyPipelineLayout(m_Device, m_MainLayout, nullptr);
		vkDestroyPipeline(m_Device, m_MainPipeline, nullptr);
	});
}
void Application::initTessellationPipeline() {
	CompiledShader   vertexShader = loadShader("src/shaders/bin/terrain.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
	CompiledShader fragmentShader = loadShader("src/shaders/bin/terrain.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
	CompiledShader     tescShader = loadShader("src/shaders/bin/terrain.tesc.spv", VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
	CompiledShader     teseShader = loadShader("src/shaders/bin/terrain.tese.spv", VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);
	
	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	m_TerrainDescriptorLayout = builder.build(m_Device, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	m_TerrainDescriptors = g_DescriptorAllocator.allocate(m_Device, m_TerrainDescriptorLayout);


	VkPushConstantRange bufferRange{};
	bufferRange.offset = 0;
	bufferRange.size = sizeof(TessellationPushConstants);
	bufferRange.stageFlags = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	
	VkPipelineLayoutCreateInfo pipelineLayoutInfo = vkinit::pipeline_layout_create_info();
	pipelineLayoutInfo.pPushConstantRanges = &bufferRange;
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	pipelineLayoutInfo.pSetLayouts = &m_TerrainDescriptorLayout;
	pipelineLayoutInfo.setLayoutCount = 1;
	
	VK_CHECK(vkCreatePipelineLayout(m_Device, &pipelineLayoutInfo, nullptr, &m_TerrainLayout));
	
	VkVertexInputBindingDescription vertexBinding{};
	vertexBinding.binding = 0;
	vertexBinding.stride = sizeof(Vertex);
	vertexBinding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
	
	std::vector<VkVertexInputAttributeDescription> attributes(2);
	
	attributes[0].binding = 0; // layout 0 -- vertex positions
	attributes[0].location = 0;
	attributes[0].format = VK_FORMAT_R32G32B32A32_SFLOAT;
	attributes[0].offset = offsetof(Vertex, position);
	
	attributes[1].binding = 0; // layout 1 -- uv coordinates
	attributes[1].location = 1;
	attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
	attributes[1].offset = offsetof(Vertex, uv);
	
	PipelineBuilder pipelineBuilder;
		
	pipelineBuilder._pipelineLayout = m_TerrainLayout;
	pipelineBuilder.set_shaders(vertexShader.shader, fragmentShader.shader);
	pipelineBuilder.set_tessellation_shaders(tescShader.shader, teseShader.shader);
	pipelineBuilder.set_tessellation_patch(4);
	pipelineBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_PATCH_LIST);
	pipelineBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
	pipelineBuilder.set_cull_mode(VK_CULL_MODE_BACK_BIT, VK_FRONT_FACE_CLOCKWISE);
	
	pipelineBuilder.set_vertex_input({vertexBinding}, attributes);
	
	pipelineBuilder.set_multisampling_none();
	pipelineBuilder.disable_blending();
	pipelineBuilder.enable_depthtest(true, VK_COMPARE_OP_GREATER_OR_EQUAL);
	
	pipelineBuilder.set_color_attachment_format(m_DrawImage.imageFormat);
	pipelineBuilder.set_depth_format(m_DepthBuffer.imageFormat);
	
	m_TerrainPipeline = pipelineBuilder.build_pipeline(m_Device);
	
	VkDescriptorImageInfo terrainImgInfo{};
	
	terrainImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	terrainImgInfo.imageView = m_Heightmap.imageView;
	terrainImgInfo.sampler = m_HeightmapSampler;
	
	VkWriteDescriptorSet descriptorWrite = {};
	descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	descriptorWrite.pNext = nullptr;
	descriptorWrite.dstBinding = 0;
	descriptorWrite.dstSet = m_TerrainDescriptors;
	descriptorWrite.descriptorCount = 1;
	descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	descriptorWrite.pImageInfo = &terrainImgInfo;
	
	vkUpdateDescriptorSets(m_Device, 1, &descriptorWrite, 0, nullptr);
	
	vkDestroyShaderModule(m_Device, fragmentShader.shader, nullptr);
	vkDestroyShaderModule(m_Device, vertexShader.shader, nullptr);
	vkDestroyShaderModule(m_Device, tescShader.shader, nullptr);
	vkDestroyShaderModule(m_Device, teseShader.shader, nullptr);
	
	
	m_DeletionQueue.push([&]() {
		vkDestroyPipelineLayout(m_Device, m_TerrainLayout, nullptr);
		vkDestroyPipeline(m_Device, m_TerrainPipeline, nullptr);
		vkDestroyDescriptorSetLayout(m_Device, m_TerrainDescriptorLayout, nullptr);
	});
}


void Application::initPipelines() {
	initComputePipelines();
	initTessellationPipeline();
	initMainPipeline();
}

void Application::initImGui() {
	VkDescriptorPoolSize pool_sizes[] = { { VK_DESCRIPTOR_TYPE_SAMPLER, 1000 },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
		{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1000 },
		{ VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1000 } };
		
	VkDescriptorPoolCreateInfo pool_info = {};
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	pool_info.maxSets = 1000;
	pool_info.poolSizeCount = (uint32_t)std::size(pool_sizes);
	pool_info.pPoolSizes = pool_sizes;
	
	VkDescriptorPool imguiPool;
	VK_CHECK(vkCreateDescriptorPool(m_Device, &pool_info, nullptr, &imguiPool));
	
	// -----
	
	ImGui::CreateContext();
	
	ImGui_ImplSDL3_InitForVulkan(m_pWindow);
	ImGui_ImplVulkan_InitInfo init_info = {};
	init_info.Instance = m_Instance;
	init_info.Device = m_Device;
	init_info.PhysicalDevice = m_GPU;
	init_info.Queue = m_GraphicsQueue;
	
	init_info.DescriptorPool = imguiPool;
	init_info.MinImageCount = 3;
	init_info.ImageCount = 3;
	init_info.UseDynamicRendering = true;
	
	init_info.PipelineInfoMain.PipelineRenderingCreateInfo = {};
	init_info.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
	init_info.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
	init_info.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &m_SwapchainFormat;
	
	ImGui_ImplVulkan_Init(&init_info);
	
	m_HeightmapImGuiDescriptors = ImGui_ImplVulkan_AddTexture(m_HeightmapSampler, m_Heightmap.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

	m_DeletionQueue.push([&]() {
		ImGui_ImplVulkan_Shutdown();
		vkDestroyDescriptorPool(m_Device, imguiPool, nullptr);
	});
}

AllocatedBuffer Application::createBuffer(size_t size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage) {
	VkBufferCreateInfo bufferInfo = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
	bufferInfo.pNext = nullptr;
	
	bufferInfo.size = size;
	bufferInfo.usage = usage;
	
	VmaAllocationCreateInfo vmaAllocInfo = {};
	vmaAllocInfo.usage = memoryUsage;
	vmaAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
	
	AllocatedBuffer newBuffer;
	
	VK_CHECK(vmaCreateBuffer(m_Allocator, &bufferInfo, &vmaAllocInfo, &newBuffer.buffer, &newBuffer.allocation, &newBuffer.info));
	
	return newBuffer;
}

GPUMeshBuffers Application::uploadMesh(std::span<uint32_t> indices, std::span<Vertex> vertices) {
	const size_t vertexBufferSize = vertices.size() * sizeof(Vertex);
	const size_t indexBufferSize = indices.size() * sizeof(uint32_t);
	
	GPUMeshBuffers newSurface;
		
	newSurface.vertexBuffer = createBuffer(vertexBufferSize, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VMA_MEMORY_USAGE_GPU_ONLY );
	
	VkBufferDeviceAddressInfo deviceAddressInfo {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = newSurface.vertexBuffer.buffer };
	newSurface.vertexBufferAddress = vkGetBufferDeviceAddress(m_Device, &deviceAddressInfo);
	
	newSurface.indexBuffer = createBuffer(indexBufferSize, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	
	AllocatedBuffer staging = createBuffer(vertexBufferSize + indexBufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	
	void* data = staging.allocation->GetMappedData();
	
	memcpy(data, vertices.data(), vertexBufferSize);
	
	memcpy((char*)data + vertexBufferSize, indices.data(), indexBufferSize);
	
	immediateSubmit([&](VkCommandBuffer cmd) {
		VkBufferCopy vertexCopy { 0 };
		vertexCopy.dstOffset = 0;
		vertexCopy.srcOffset = 0;
		vertexCopy.size = vertexBufferSize;
		
		vkCmdCopyBuffer(cmd, staging.buffer, newSurface.vertexBuffer.buffer, 1, &vertexCopy);
		
		VkBufferCopy indexCopy { 0 };
		indexCopy.dstOffset = 0;
		indexCopy.srcOffset = vertexBufferSize;
		indexCopy.size = indexBufferSize;
		
		vkCmdCopyBuffer(cmd, staging.buffer, newSurface.indexBuffer.buffer, 1, &indexCopy);
	});
	
	destroyBuffer(staging);
	
	return newSurface;
}

void Application::initDefaultData() {
	testMeshes = loadGltfMeshes(this, "assets\\basicmesh.glb").value();
	
	generateHeightmap();
	
	
	m_TerrainPC.factor = m_WorldSize * 0.5f;
	m_Camera.position.y = m_WorldSize * 0.25f;
}

void Application::initTerrainPatches() {
	m_PatchVertices.clear();
	Vertex vertex{};
	vertex.position.y = 0.0f;
	unsigned rez = 64;
	float width = m_WorldSize;
	float height = m_WorldSize;
	for(unsigned i = 0; i <= rez-1; i++) // https://learnopengl.com/Guest-Articles/2021/Tessellation/Tessellation
	{
		for(unsigned j = 0; j <= rez-1; j++)
		{
			vertex.position.x = -width /2.0f + width *i/(float)rez; // v.x
			vertex.position.z = -height/2.0f + height*j/(float)rez; // v.z
			vertex.uv.x = i / (float)rez; // u
			vertex.uv.y = j / (float)rez; // v
			m_PatchVertices.push_back(vertex);

			vertex.position.x = -width /2.0f + width *(i+1)/(float)rez; // v.x
			vertex.position.z = -height/2.0f + height*j/(float)rez;		// v.z
			vertex.uv.x = (i+1) / (float)rez; // u
			vertex.uv.y =  j    / (float)rez; // v
			m_PatchVertices.push_back(vertex);

			vertex.position.x = -width /2.0f + width *i/(float)rez;		// v.x
			vertex.position.z = -height/2.0f + height*(j+1)/(float)rez; // v.z
			vertex.uv.x = i / (float)rez; // u
			vertex.uv.y = (j+1) / (float)rez; // v
			m_PatchVertices.push_back(vertex);

			vertex.position.x = -width /2.0f + width *(i+1)/(float)rez; // v.x
			vertex.position.z = -height/2.0f + height*(j+1)/(float)rez; // v.z
			vertex.uv.x = (i+1) / (float)rez; // u
			vertex.uv.y = (j+1) / (float)rez; // v
			m_PatchVertices.push_back(vertex);
		}
	}
	// upload the shit to the GPU
	// similar to uploadMesh but without indexBuffer
	const size_t bufferSize = m_PatchVertices.size() * sizeof(Vertex);
	
	m_TerrainVertexBuffer = createBuffer(bufferSize,
		VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
		VMA_MEMORY_USAGE_GPU_ONLY);
		
	VkBufferDeviceAddressInfo deviceAddressInfo {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = m_TerrainVertexBuffer.buffer };
	m_TerrainVertexAddress = vkGetBufferDeviceAddress(m_Device, &deviceAddressInfo);
	
	AllocatedBuffer staging = createBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	
	void* data = staging.allocation->GetMappedData();
	memcpy(data, m_PatchVertices.data(), bufferSize);
	
	immediateSubmit([&](VkCommandBuffer cmd) {
		VkBufferCopy vertexCopy {0};
		vertexCopy.dstOffset = 0;
		vertexCopy.srcOffset = 0;
		vertexCopy.size = bufferSize;

		vkCmdCopyBuffer(cmd, staging.buffer, m_TerrainVertexBuffer.buffer, 1, &vertexCopy);
	});
	
	destroyBuffer(staging);
	m_DeletionQueue.push([&]() {
		destroyBuffer(m_TerrainVertexBuffer);
	});
}

void Application::drawImGui(VkCommandBuffer cmd, VkImageView targetImageView) {
	VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(targetImageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingInfo renderInfo = vkinit::rendering_info(m_SwapchainExtent, &colorAttachment, nullptr);
	
	vkCmdBeginRendering(cmd, &renderInfo);
	
	ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd);
	
	vkCmdEndRendering(cmd);
}

void Application::createSwapchain(uint32_t width, uint32_t height) {
	vkb::SwapchainBuilder swapchainBuilder{ m_GPU, m_Device, m_Surface };
	
	m_SwapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
	
	vkb::Swapchain vkbSwapchain = swapchainBuilder
								  //.use_default_format_selection()
								  .set_desired_format( VkSurfaceFormatKHR { .format = m_SwapchainFormat, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
								  .set_desired_present_mode(VK_PRESENT_MODE_FIFO_KHR) //vsync
								  .set_desired_extent(width, height)
								  .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
								  .build()
								  .value();
	  
	m_SwapchainExtent = vkbSwapchain.extent;
	m_Swapchain = vkbSwapchain.swapchain;
	m_SwapchainImages = vkbSwapchain.get_images().value();
	m_SwapchainImageViews = vkbSwapchain.get_image_views().value();
}

void Application::resizeSwapchain() {
	vkDeviceWaitIdle(m_Device);
	destroySwapchain();
	
	int w,h;
	SDL_GetWindowSizeInPixels(m_pWindow, &w, &h);
	m_Width = w * m_RenderScale;
	m_Height = h * m_RenderScale;
	m_TerrainPC.screen = {m_Width, m_Height};
	
	initSwapchain();
	
	m_Resized = false;
}

void Application::destroySwapchain() {
	vkDestroySwapchainKHR(m_Device, m_Swapchain, nullptr);
	
	for(int i = 0; i < m_SwapchainImageViews.size(); i++) {
		vkDestroyImageView(m_Device, m_SwapchainImageViews[i], nullptr);
	}
	
	vkDestroyImageView(m_Device, m_DrawImage.imageView, nullptr);
	vmaDestroyImage(m_Allocator, m_DrawImage.image, m_DrawImage.allocation);
		
	vkDestroyImageView(m_Device, m_DepthBuffer.imageView, nullptr);
	vmaDestroyImage(m_Allocator, m_DepthBuffer.image, m_DepthBuffer.allocation);	
}

void Application::clearImage(VkCommandBuffer cmd) {
	VkClearColorValue clearValue = {{0.0f, 0.0f, 0.1f, 1.0f}};
	
	VkImageSubresourceRange clearRange = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);
	
	vkCmdClearColorImage(cmd, m_DrawImage.image, VK_IMAGE_LAYOUT_GENERAL, &clearValue, 1, &clearRange);
}

void Application::generateHeightmap() {
	immediateSubmit([&](VkCommandBuffer cmd) {
		vkutil::transition_image(cmd, m_Heightmap.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
		
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_HeightmapEffect.pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ComputeLayout, 0, 1, &m_HeightmapDescriptors, 0, nullptr);
		
		HeightmapPushConstants pc{};
		pc.updateSize = glm::ivec2(m_HeightmapSize);
		
		vkCmdPushConstants(cmd, m_ComputeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(HeightmapPushConstants), &pc);
		
		uint32_t groupNX = std::ceil(static_cast<float>(m_HeightmapSize) / 16.f);
		uint32_t groupNY = std::ceil(static_cast<float>(m_HeightmapSize) / 16.f);
		vkCmdDispatch(cmd, groupNX, groupNY, 1);
		
		vkutil::transition_image(cmd, m_Heightmap.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	});
}

void Application::updateHeightmap(bool regenerate) {
	glm::ivec2 pos;
	pos.x = std::round(m_Camera.position.x);
	pos.y = std::round(m_Camera.position.z);
	
	glm::ivec2 delta = (m_HeightmapPC.offset / glm::ivec2(m_CoordinateMultiplier)) - pos;
	if((std::abs(delta.x) == 0 && std::abs(delta.y) == 0) && !regenerate) return;
		
	m_MapOffset = pos * glm::ivec2(m_CoordinateMultiplier);
	m_HeightmapPC.offset = m_MapOffset;
	if(regenerate) delta = {m_HeightmapSize, m_HeightmapSize};
	immediateSubmit([&](VkCommandBuffer cmd) {
		vkutil::transition_image(cmd, m_Heightmap.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
		
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_HeightmapEffect.pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ComputeLayout, 0, 1, &m_HeightmapDescriptors, 0, nullptr);
		
		HeightmapPushConstants pc = m_HeightmapPC;
		
		glm::ivec2 dirtyMin;
		dirtyMin.x = (pos.x * m_CoordinateMultiplier) % m_HeightmapSize;
		dirtyMin.y = (pos.y * m_CoordinateMultiplier) % m_HeightmapSize;
		if(dirtyMin.x < 0) dirtyMin.x += m_HeightmapSize;
		if(dirtyMin.y < 0) dirtyMin.y += m_HeightmapSize;
		
		pc.dirtyMin = dirtyMin;
		if(delta.x != 0) {
			uint32_t dispatchWidth = std::min((uint32_t)m_HeightmapSize,(uint32_t)std::abs(delta.x) * m_CoordinateMultiplier);
			uint32_t dispatchHeight  = m_HeightmapSize;
			
			HeightmapPushConstants pcX = pc;
			
			if(delta.x < 0) {
				uint32_t offsetX = m_HeightmapSize - dispatchWidth;
				pcX.offset.x += offsetX;
				pcX.dirtyMin.x = (pcX.dirtyMin.x + offsetX) % m_HeightmapSize;
			}
			pcX.updateSize.x = dispatchWidth;
			pcX.updateSize.y = dispatchHeight;
			
			vkCmdPushConstants(cmd, m_ComputeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(HeightmapPushConstants), &pcX);
			uint32_t groupNX = (dispatchWidth + 15) / 16;
			uint32_t groupNY = (dispatchHeight + 15) / 16;
			vkCmdDispatch(cmd, groupNX, groupNY, 1);
		}
		if(delta.y != 0) {
			HeightmapPushConstants pcY = pc;
			
			uint32_t dispatchWidth  = m_HeightmapSize;
			uint32_t dispatchHeight = std::min((uint32_t)m_HeightmapSize,(uint32_t)std::abs(delta.y) * m_CoordinateMultiplier);
			
			if(delta.y < 0) {
				uint32_t offsetY = m_HeightmapSize - dispatchHeight;
				pcY.offset.y += offsetY;
				pcY.dirtyMin.y = (pcY.dirtyMin.y + offsetY) % m_HeightmapSize;
			}
			pcY.updateSize.x = dispatchWidth;
			pcY.updateSize.y = dispatchHeight;
			
			vkCmdPushConstants(cmd, m_ComputeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(HeightmapPushConstants), &pcY);
			uint32_t groupNX = (dispatchWidth + 15) / 16;
			uint32_t groupNY = (dispatchHeight + 15) / 16;
			vkCmdDispatch(cmd, groupNX, groupNY, 1);
		}
		vkutil::transition_image(cmd, m_Heightmap.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	});	
}

void Application::drawGeometry(VkCommandBuffer cmd) {
	VkClearValue depthClear{};
    depthClear.depthStencil.depth = 0.0f;
	
	VkRenderingAttachmentInfo colorAttachment = vkinit::attachment_info(m_DrawImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingAttachmentInfo depthAttachment = vkinit::attachment_info(m_DepthBuffer.imageView, &depthClear, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
	
	VkRenderingInfo renderInfo = vkinit::rendering_info(m_DrawExtent, &colorAttachment, &depthAttachment);
	vkCmdBeginRendering(cmd, &renderInfo);
	
	VkViewport viewport = {};
	viewport.x = 0;
	viewport.y = 0;	
	viewport.width = m_DrawExtent.width;
	viewport.height = m_DrawExtent.height;
	viewport.minDepth = 0.f;
	viewport.maxDepth = 1.f;
	
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	
	VkRect2D scissor = {};
	scissor.offset.x = 0;
	scissor.offset.y = 0;
	scissor.extent.width = m_DrawExtent.width;
	scissor.extent.height = m_DrawExtent.height;
	
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	// calculate matrices
	glm::mat4 view = m_Camera.getViewMatrix({m_MapOffset.x / static_cast<float>(m_CoordinateMultiplier), 0.0, m_MapOffset.y / static_cast<float>(m_CoordinateMultiplier)});
	glm::mat4 projection = glm::perspective(glm::radians(70.f), (float)m_DrawExtent.width / (float)m_DrawExtent.height, 10000.f, 0.1f); // reversing the depth buffer apparently increases precision
	projection[1][1] *= -1; // vulkan had to go out of its way to break the standard and make the Y axis reversed ):
	/* // ---
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_MainPipeline);
	
	GPUDrawPushConstants pushConstants;
	pushConstants.worldMatrix = projection * view;
	
	pushConstants.vertexBuffer = testMeshes[meshIndex]->meshBuffers.vertexBufferAddress;
	
	vkCmdPushConstants(cmd, m_MainLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(GPUDrawPushConstants), &pushConstants);
	vkCmdBindIndexBuffer(cmd, testMeshes[meshIndex]->meshBuffers.indexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
	
	vkCmdDrawIndexed(cmd, testMeshes[meshIndex]->surfaces[0].count, 1, testMeshes[meshIndex]->surfaces[0].startIndex, 0, 0);
	*/ // ---
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_TerrainPipeline);
	
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_TerrainLayout, 0, 1, &m_TerrainDescriptors, 0, nullptr);
	
	m_TerrainPC.view = view;
	m_TerrainPC.projection = projection;
	m_TerrainPC.worldoffset = m_MapOffset;
	
	vkCmdPushConstants(cmd, m_TerrainLayout, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(TessellationPushConstants), &m_TerrainPC);
	
	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &m_TerrainVertexBuffer.buffer, &offset);
	
	vkCmdDraw(cmd, static_cast<uint32_t>(m_PatchVertices.size()), 1, 0, 0);
	vkCmdEndRendering(cmd);
}

void Application::run() {
	bool quit = false;
	
	SDL_Event event;
	
	Uint64 lastTime = SDL_GetTicks();
	
	while(!quit) {
		Uint64 now = SDL_GetTicks();
		float deltatime = (now - lastTime) / 1000.0f;
		lastTime = now;
		
		while(SDL_PollEvent(&event)) {
			if(event.type == SDL_EVENT_QUIT) quit = true;
			if(event.type == SDL_EVENT_WINDOW_RESIZED) m_Resized = true;
			if(event.type == SDL_EVENT_KEY_UP) { if(event.key.key == SDLK_ESCAPE) m_Mouselock = !m_Mouselock; }
			if(m_Mouselock) m_Camera.processSDLEvent(event);
			if(!m_Mouselock) ImGui_ImplSDL3_ProcessEvent(&event);
			SDL_SetWindowRelativeMouseMode(m_pWindow, m_Mouselock);
		}
		m_Camera.update(deltatime);
		if(!m_Mouselock) {
			// imgui new frame
			ImGui_ImplVulkan_NewFrame();
			ImGui_ImplSDL3_NewFrame();

			//some imgui UI
			ImGui::NewFrame();
			
			if (ImGui::Begin("settings")) {
				float var;
						
				ImGui::SliderInt("Model index", &meshIndex, 0, 2);
				if(ImGui::SliderFloat("Render scale", &m_RenderScale, 0.1f, 4.f)) {m_Resized = true;}
				ImGui::SliderFloat("Depth factor", &m_TerrainPC.factor, 0.0f, static_cast<float>(m_WorldSize) * 0.5);
				ImGui::SliderFloat("Tessellation Factor", &m_TerrainPC.tessellationFactor, 0.0f, 1.f);
			}
			ImGui::End();

			if (ImGui::Begin("heightmap")) {
				if(ImGui::Button("Regenerate terrain")) updateHeightmap(true);
				
				if(ImGui::SliderFloat("Erosion scale", &m_HeightmapPC.settings[0], 0.01f, 0.3f)) {updateHeightmap(true);}
				if(ImGui::SliderFloat("Erosion strength", &m_HeightmapPC.settings[1], 0.0f, 0.22f)) {updateHeightmap(true);}
				if(ImGui::SliderFloat("Erosion gully weight", &m_HeightmapPC.settings[2], 0.0f, 1.0f)) {updateHeightmap(true);}
				if(ImGui::SliderFloat("Erosion detail", &m_HeightmapPC.settings[3], 0.0f, 2.5f)) {updateHeightmap(true);}
				
				if(ImGui::SliderFloat("Ridge rounding", &m_HeightmapPC.settings[4], 0.0f, 0.3f)) {updateHeightmap(true);}
				if(ImGui::SliderFloat("Crease rounding", &m_HeightmapPC.settings[5], 0.0f, 0.3f)) {updateHeightmap(true);}
				if(ImGui::SliderFloat("Erosion cell scale", &m_HeightmapPC.settings[6], 0.0f, 1.5f)) {updateHeightmap(true);}
				if(ImGui::SliderFloat("Erosion normalization", &m_HeightmapPC.settings[7], 0.0f, 1.f)) {updateHeightmap(true);}
				
				int erosionoctaves = static_cast<int>(m_HeightmapPC.settings[8]);
				if(ImGui::SliderInt("Erosion octaves", &erosionoctaves, 0, 10)) {
					m_HeightmapPC.settings[8] = static_cast<float>(erosionoctaves);
					updateHeightmap(true);
				}
				if(ImGui::SliderFloat("Erosion lacunarity", &m_HeightmapPC.settings[9], 1.0f, 4.f)) {updateHeightmap(true);}
				if(ImGui::SliderFloat("Erosion gain", &m_HeightmapPC.settings[10], 0.0f, 1.f)) {updateHeightmap(true);}
				int heightOctaves = static_cast<int>(m_HeightmapPC.settings[11]);
				if(ImGui::SliderInt("Height octaves", &heightOctaves, 0, 10)) {
					m_HeightmapPC.settings[11] = static_cast<float>(heightOctaves);
					updateHeightmap(true);
				}
				
				if(ImGui::SliderFloat("Height frequency", &m_HeightmapPC.settings[12], 0.0f, 8.0f)) {updateHeightmap(true);}
				if(ImGui::SliderFloat("Height amplitude", &m_HeightmapPC.settings[13], 0.0f, 2.0f)) {updateHeightmap(true);}
				if(ImGui::SliderFloat("Height lacunarity", &m_HeightmapPC.settings[14], 1.0f, 4.0f)) {updateHeightmap(true);}
				if(ImGui::SliderFloat("Height gain", &m_HeightmapPC.settings[15], 0.0f, 1.0f)) {updateHeightmap(true);}
				
				ImVec2 panelSize = ImGui::GetContentRegionAvail();
				panelSize.x = std::min(std::min(panelSize.x, static_cast<float>(m_HeightmapSize)), panelSize.y);
				panelSize.y = panelSize.x;
				ImGui::Image((ImTextureID)m_HeightmapImGuiDescriptors, panelSize);
				
			}
			
			ImGui::End();

			ImGui::Render();
		}
		updateHeightmap();
		draw();
	}
}

void Application::draw() {
	if(m_Resized) resizeSwapchain();
	
	// Wait for the GPU to finish, reset the fence
	VK_CHECK(vkWaitForFences(m_Device, 1, &getCurrentFrame().renderFence, true, 1000000000));
	
	getCurrentFrame().deletionQueue.flush();
	
	VK_CHECK(vkResetFences(m_Device, 1, &getCurrentFrame().renderFence));
	
	// Get a swapchain index for the next frame
	uint32_t swapchainImageIndex;
	// If the swapchain is full, it'll wait up to a second
	VkResult e = vkAcquireNextImageKHR(m_Device, m_Swapchain, 1000000000, getCurrentFrame().swapchainSemaphore, nullptr, &swapchainImageIndex);
	if(e == VK_ERROR_OUT_OF_DATE_KHR) {
		m_Resized = true;
		return;
	}
	
	VkCommandBuffer cmd = getCurrentFrame().commandBuffer;
	VK_CHECK(vkResetCommandBuffer(cmd, 0));
	
	VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
	// the flag one time submit basically tells the driver that we're only submitting and executing the command once
	m_DrawExtent.width = m_DrawImage.imageExtent.width;
	m_DrawExtent.height = m_DrawImage.imageExtent.height;
	
	VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));	// command buffer is now recording!
	
	// make the swapchain image writeable
	vkutil::transition_image(cmd, m_DrawImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
	// START DRAW COMMANDS
	
	clearImage(cmd);
	
	vkutil::transition_image(cmd, m_DrawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	vkutil::transition_image(cmd, m_DepthBuffer.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
	
	drawGeometry(cmd);
	// END DRAW COMMANDS

	// set the draw image to read mode
	vkutil::transition_image(cmd, m_DrawImage.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	// set the swapchain to write mode
	vkutil::transition_image(cmd, m_SwapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	vkutil::copy_image_to_image(cmd, m_DrawImage.image, m_SwapchainImages[swapchainImageIndex], m_DrawExtent, m_SwapchainExtent);

	vkutil::transition_image(cmd, m_SwapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

	if(!m_Mouselock) { drawImGui(cmd, m_SwapchainImageViews[swapchainImageIndex]); }
		
	vkutil::transition_image(cmd, m_SwapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
	VK_CHECK(vkEndCommandBuffer(cmd));
	
	VkCommandBufferSubmitInfo cmdinfo = vkinit::command_buffer_submit_info(cmd); // beautiful abstraction 🙏
	
	VkSemaphoreSubmitInfo waitInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR, getCurrentFrame().swapchainSemaphore);
	VkSemaphoreSubmitInfo signalInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, getCurrentFrame().renderSemaphore);
	
	VkSubmitInfo2 submit = vkinit::submit_info(&cmdinfo, &signalInfo, &waitInfo);
	
	VK_CHECK(vkQueueSubmit2(m_GraphicsQueue, 1, &submit, getCurrentFrame().renderFence));
	
	// present prep
	VkPresentInfoKHR presentInfo = {};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.pNext = nullptr;
	presentInfo.pSwapchains = &m_Swapchain;
	presentInfo.swapchainCount = 1;
	
	presentInfo.pWaitSemaphores = &getCurrentFrame().renderSemaphore;
	presentInfo.waitSemaphoreCount = 1;
	
	presentInfo.pImageIndices = &swapchainImageIndex;
	
	VkResult presentResult = vkQueuePresentKHR(m_GraphicsQueue, &presentInfo);
	if(presentResult == VK_ERROR_OUT_OF_DATE_KHR) {
		m_Resized = true;
	}
	
	m_FrameNumber++;
}

void Application::immediateSubmit(std::function<void(VkCommandBuffer cmd)>&& function) {
	VK_CHECK(vkResetFences(m_Device, 1, &m_ImmFence));
	VK_CHECK(vkResetCommandBuffer(m_ImmCommandBuffer, 0));
	
	VkCommandBuffer cmd = m_ImmCommandBuffer;
	
	VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
	
	VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));
	
	function(cmd);
	
	VK_CHECK(vkEndCommandBuffer(cmd));
	
	VkCommandBufferSubmitInfo cmdinfo = vkinit::command_buffer_submit_info(cmd);
	VkSubmitInfo2 submit = vkinit::submit_info(&cmdinfo, nullptr, nullptr);
	
	VK_CHECK(vkQueueSubmit2(m_GraphicsQueue, 1, &submit, m_ImmFence));
	VK_CHECK(vkWaitForFences(m_Device, 1, &m_ImmFence, true, 9999999999));	
}