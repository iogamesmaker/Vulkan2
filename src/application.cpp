// Application.cpp
#define VMA_IMPLEMENTATION
#include <glm/gtx/transform.hpp>
#include "vk_mem_alloc.h"

#include "application.hpp"
#include <glm/gtc/packing.hpp>
#include <sstream>
#include <filesystem>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

constexpr bool useValidationLayers = true; // ?

Application::Application(uint32_t width, uint32_t height) : m_Width(width), m_Height(height) {
	initWindow();

	initVulkan();
	// a minimized window's surface has no size and so no swapchain: launched minimized, wait for it to be shown
	// (still answering the window's messages). the surface's own extent, SDL's minimized flag clears a moment before
	// it has one. closed before that: nothing worth cleaning up yet, just go
	for(;;) {
		VkSurfaceCapabilitiesKHR capabilities;
		VK_CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(m_GPU, m_Surface, &capabilities));
		if(capabilities.currentExtent.width > 0 && capabilities.currentExtent.height > 0) break;
		SDL_Event event;
		if(SDL_WaitEventTimeout(&event, 100) && event.type == SDL_EVENT_QUIT) {
			SDL_Quit();
			std::exit(0);
		}
	}
	initSwapchain();
	initCommands();
	initSyncStructures();

	// the rest loads on a worker thread while this one keeps the window responsive and draws a spinner (loading.cpp)
	initLoadingScreen();
	runWhileLoading([this]() {
		initDescriptors();
		m_AtmosphereManager.init(m_Device, m_Allocator, this);
		{
			// the terrain is m_WorldSize wide around the map offset, the outer 1.6% is discarded in terrain.frag
			// and the offset lags behind the camera by up to m_WorldSize / 64, so that's the closest an edge gets
			float renderDistance = (m_WorldSize * 0.5f * (1.0f - 2.0f * 0.016f) - m_WorldSize / 64.0f - 8.0f) * m_TerrainScale;
			m_AtmosphereManager.setTerrainSize(m_WorldSize * m_TerrainScale, renderDistance, m_TextureRepeatNear, m_TextureRepeatFar);
			m_TerrainPC.worldRadius = renderDistance; // round world, every direction sees equally far
		}
		m_Shadows.init(m_Device, m_Allocator, this);
		m_Shadows.setWorldRadius(m_TerrainPC.worldRadius);
		m_ShadowFilter.init(this);
		m_ShadowFilter.setTargets(m_DrawImage, m_DepthBuffer);

		initPipelines();

		initTerrainPatches();
	});

	initImGui(); // on this thread, ImGui's SDL backend talks to the window

	runWhileLoading([this]() { initDefaultData(); }); // textures, the heightmap, the foliage / trees / props
	destroyLoadingScreen();
	m_InitDone = true;
}

Application::~Application() {
	if(m_InitDone) {
		vkDeviceWaitIdle(m_Device);
		
		for(int i = 0; i < FRAME_OVERLAP; i++) {
			vkDestroyCommandPool(m_Device, m_Frames[i].commandPool, nullptr);
			
			vkDestroyFence(m_Device, m_Frames[i].renderFence, nullptr);
			vkDestroySemaphore(m_Device, m_Frames[i].swapchainSemaphore, nullptr);
			
			m_Frames[i].deletionQueue.flush();
		}
		
		destroySwapchain();
		m_AtmosphereManager.destroy();
		m_Shadows.destroy();
		m_ShadowFilter.destroy();
		m_Bloom.destroy();
		m_HiZ.destroy();
		m_TerrainCache.destroy();
		m_DebugView.destroy(); // before imgui shuts down (the deletion queue), it removes its imgui texture
		m_Props.destroy();
		m_GroundCover.destroy();
		m_Precipitation.destroy();
		m_Foliage.destroy();
		m_Trees.destroy();
		m_PlayerTrail.destroy();

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
	// VULKAN2_FULLSCREEN: borderless fullscreen at the desktop resolution (benchmarks at the real size)
	SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE;
	bool fullscreen = std::getenv("VULKAN2_FULLSCREEN") != nullptr;
	if(fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
	m_pWindow = SDL_CreateWindow("Vulkan Attempt 2", m_Width, m_Height, flags);
	if(!m_pWindow) throw std::runtime_error(SDL_GetError());
	if(fullscreen) m_Resized = true; // the swapchain was sized for the windowed size, match the screen
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
	
	features12.shaderSampledImageArrayNonUniformIndexing = true;
	features12.descriptorBindingPartiallyBound = true;
	features12.runtimeDescriptorArray = true;
	features12.descriptorBindingStorageImageUpdateAfterBind = true;
	features12.descriptorBindingSampledImageUpdateAfterBind = true;
	
	// Base features
	VkPhysicalDeviceFeatures features10 {};
	features10.tessellationShader = VK_TRUE;
	features10.multiDrawIndirect = VK_TRUE; // the ground cover draws all its meshes with one indirect call
	features10.textureCompressionBC = VK_TRUE; // block compressed textures, see TextureLoader::loadCompressed
	features10.shaderStorageImageExtendedFormats = VK_TRUE; // rg16f storage image for the sky visibility (horizon AO + canopy) map
	features10.samplerAnisotropy = VK_TRUE; // the material sampler (see initSamplers): grazing ground otherwise smears into streaks
	
	
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
	// the driver's real per heap usage / budget (all programs), for the settings window. optional
	m_MemoryBudgetExtension = physicalDevice.enable_extension_if_present(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);

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
	allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_3; // (the budget query uses the 1.1 properties2 path)
	allocatorInfo.flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT;
	if(m_MemoryBudgetExtension) allocatorInfo.flags |= VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT;
	
	vmaCreateAllocator(&allocatorInfo, &m_Allocator);
	
	m_DeletionQueue.push([&]() {
		vmaDestroyAllocator(m_Allocator);
	});
}

void Application::initSwapchain(bool vsync) {
	createSwapchain(m_Width, m_Height, vsync);
	
	VkExtent3D drawImageExtent = { m_Width, m_Height, 1 };
	
	m_DrawImage.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
	m_DrawImage.imageExtent = drawImageExtent;
	
	VkImageUsageFlags drawImageUsages{};
	drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	drawImageUsages |= VK_IMAGE_USAGE_STORAGE_BIT; // post processing writes to it
	drawImageUsages |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	drawImageUsages |= VK_IMAGE_USAGE_SAMPLED_BIT; // bloom reads it filtered
	
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
	depthBufferUsages |= VK_IMAGE_USAGE_SAMPLED_BIT; // godrays read it

	VkImageCreateInfo depthBufferCI = vkinit::image_create_info(m_DepthBuffer.imageFormat, depthBufferUsages, drawImageExtent);
	
	vmaCreateImage(m_Allocator, &depthBufferCI, &imageAllocinfo, &m_DepthBuffer.image, &m_DepthBuffer.allocation, nullptr);
	
	VkImageViewCreateInfo depthViewCI = vkinit::imageview_create_info(m_DepthBuffer.imageFormat, m_DepthBuffer.image, VK_IMAGE_ASPECT_DEPTH_BIT);
	
	VK_CHECK(vkCreateImageView(m_Device, &depthViewCI, nullptr, &m_DepthBuffer.imageView));

	// G-buffer, written by the opaque materials, read by the lighting pass
	auto createTarget = [&](AllocatedImage& target, VkFormat format) {
		target.imageFormat = format;
		target.imageExtent = drawImageExtent;
		VkImageCreateInfo targetCI = vkinit::image_create_info(format, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, drawImageExtent);
		VK_CHECK(vmaCreateImage(m_Allocator, &targetCI, &imageAllocinfo, &target.image, &target.allocation, nullptr));
		VkImageViewCreateInfo targetViewCI = vkinit::imageview_create_info(format, target.image, VK_IMAGE_ASPECT_COLOR_BIT);
		VK_CHECK(vkCreateImageView(m_Device, &targetViewCI, nullptr, &target.imageView));
	};
	createTarget(m_GAlbedo, GBUFFER_ALBEDO);
	createTarget(m_GNormal, GBUFFER_NORMAL);
	createTarget(m_GMaterial, GBUFFER_MATERIAL);
	createTarget(m_GVelocity, GBUFFER_VELOCITY);

	// TAA / TSR history (ping-pong) at the OUTPUT resolution, read bilinear + written as storage
	VkExtent3D outputExtent = { m_SwapchainExtent.width, m_SwapchainExtent.height, 1 };
	for(AllocatedImage& history : m_History) {
		history.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
		history.imageExtent = outputExtent;
		VkImageCreateInfo historyCI = vkinit::image_create_info(history.imageFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, outputExtent);
		VK_CHECK(vmaCreateImage(m_Allocator, &historyCI, &imageAllocinfo, &history.image, &history.allocation, nullptr));
		VkImageViewCreateInfo historyViewCI = vkinit::imageview_create_info(history.imageFormat, history.image, VK_IMAGE_ASPECT_COLOR_BIT);
		VK_CHECK(vkCreateImageView(m_Device, &historyViewCI, nullptr, &history.imageView));
	}
	m_HistoryValid = false;

	// the output image (window resolution): TSR result, post processing, copied to the swapchain
	{
		m_OutputImage.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
		m_OutputImage.imageExtent = outputExtent;
		VkImageCreateInfo outputCI = vkinit::image_create_info(m_OutputImage.imageFormat,
			VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, outputExtent); // (the rain draws onto it)
		VK_CHECK(vmaCreateImage(m_Allocator, &outputCI, &imageAllocinfo, &m_OutputImage.image, &m_OutputImage.allocation, nullptr));
		VkImageViewCreateInfo outputViewCI = vkinit::imageview_create_info(m_OutputImage.imageFormat, m_OutputImage.image, VK_IMAGE_ASPECT_COLOR_BIT);
		VK_CHECK(vkCreateImageView(m_Device, &outputViewCI, nullptr, &m_OutputImage.imageView));
	}

	// SSAO and the volumetric clouds: half resolution, or full on Ultra (each its own setting; the shaders work out
	// the ratio from the image sizes)
	{
		VkExtent3D half = { std::max(drawImageExtent.width / 2, 1u), std::max(drawImageExtent.height / 2, 1u), 1 };
		VkExtent3D ssaoExtent = m_FullResSSAO ? drawImageExtent : half;
		VkExtent3D quarter = { std::max(drawImageExtent.width / 4, 1u), std::max(drawImageExtent.height / 4, 1u), 1 };
		VkExtent3D cloudExtent = m_CloudResolution >= 2 ? drawImageExtent : (m_CloudResolution == 1 ? half : quarter);
		m_SSAOImage.imageFormat = VK_FORMAT_R8G8_UNORM; // AO + light shaft visibility
		m_SSAOImage.imageExtent = ssaoExtent;
		VkImageCreateInfo ssaoCI = vkinit::image_create_info(m_SSAOImage.imageFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, ssaoExtent);
		VK_CHECK(vmaCreateImage(m_Allocator, &ssaoCI, &imageAllocinfo, &m_SSAOImage.image, &m_SSAOImage.allocation, nullptr));
		VkImageViewCreateInfo ssaoViewCI = vkinit::imageview_create_info(m_SSAOImage.imageFormat, m_SSAOImage.image, VK_IMAGE_ASPECT_COLOR_BIT);
		VK_CHECK(vkCreateImageView(m_Device, &ssaoViewCI, nullptr, &m_SSAOImage.imageView));

		// the volumetric clouds
		m_CloudImage.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT; // in-scattered light, transmittance
		m_CloudImage.imageExtent = cloudExtent;
		// (the resolved result: the march goes into m_CloudRaw, cloudresolve.comp accumulates it over frames into
		// this one, which is then copied into m_CloudHistory for the next frame)
		VkImageCreateInfo cloudCI = vkinit::image_create_info(m_CloudImage.imageFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, cloudExtent);
		VK_CHECK(vmaCreateImage(m_Allocator, &cloudCI, &imageAllocinfo, &m_CloudImage.image, &m_CloudImage.allocation, nullptr));
		VkImageViewCreateInfo cloudViewCI = vkinit::imageview_create_info(m_CloudImage.imageFormat, m_CloudImage.image, VK_IMAGE_ASPECT_COLOR_BIT);
		VK_CHECK(vkCreateImageView(m_Device, &cloudViewCI, nullptr, &m_CloudImage.imageView));
		std::pair<AllocatedImage*, VkImageUsageFlags> cloudTargets[] = {
			{ &m_CloudRaw, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT },
			{ &m_CloudHistory, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT },
		};
		for(auto& [image, usage] : cloudTargets) {
			image->imageFormat = m_CloudImage.imageFormat;
			image->imageExtent = cloudExtent;
			VkImageCreateInfo imageCI = vkinit::image_create_info(image->imageFormat, usage, cloudExtent);
			VK_CHECK(vmaCreateImage(m_Allocator, &imageCI, &imageAllocinfo, &image->image, &image->allocation, nullptr));
			VkImageViewCreateInfo viewCI = vkinit::imageview_create_info(image->imageFormat, image->image, VK_IMAGE_ASPECT_COLOR_BIT);
			VK_CHECK(vkCreateImageView(m_Device, &viewCI, nullptr, &image->imageView));
		}
		std::pair<AllocatedImage*, VkImageUsageFlags> cloudDepthTargets[] = {
			{ &m_CloudDepth, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT },
			{ &m_CloudDepthHistory, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT },
		};
		for(auto& [image, usage] : cloudDepthTargets) {
			image->imageFormat = VK_FORMAT_R32_SFLOAT;
			image->imageExtent = cloudExtent;
			VkImageCreateInfo imageCI = vkinit::image_create_info(image->imageFormat, usage, cloudExtent);
			VK_CHECK(vmaCreateImage(m_Allocator, &imageCI, &imageAllocinfo, &image->image, &image->allocation, nullptr));
			VkImageViewCreateInfo viewCI = vkinit::imageview_create_info(image->imageFormat, image->image, VK_IMAGE_ASPECT_COLOR_BIT);
			VK_CHECK(vkCreateImageView(m_Device, &viewCI, nullptr, &image->imageView));
		}
		m_CloudHistoryValid = false;
	}

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
		
		VK_CHECK(vkCreateSemaphore(m_Device, &semaphoreCreateInfo, nullptr, &m_Frames[i].swapchainSemaphore));
	}
	
	VK_CHECK(vkCreateFence(m_Device, &fenceCreateInfo, nullptr, &m_ImmFence));

	VkQueryPoolCreateInfo queryCI{ .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
	queryCI.queryType = VK_QUERY_TYPE_TIMESTAMP;
	queryCI.queryCount = GPU_QUERIES * FRAME_OVERLAP;
	VK_CHECK(vkCreateQueryPool(m_Device, &queryCI, nullptr, &m_TimestampPool));
	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(m_GPU, &properties);
	m_TimestampPeriod = properties.limits.timestampPeriod;
	m_DeletionQueue.push([&]() {
		vkDestroyQueryPool(m_Device, m_TimestampPool, nullptr);
		vkDestroyFence(m_Device, m_ImmFence, nullptr);
	});
}

void Application::initDescriptors() {
	std::vector<DescriptorAllocator::PoolSizeRatio> sizes = {
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1000 },
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1000 },
		{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1000 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1000 }
	};
	
	g_DescriptorAllocator.init_pool(m_Device, 128, sizes); // atmosphere, shadows, lighting, TAA, bloom passes...
		
	VkSamplerCreateInfo samplerCI = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	samplerCI.magFilter = VK_FILTER_LINEAR;
	samplerCI.minFilter = VK_FILTER_LINEAR;
	vkCreateSampler(m_Device, &samplerCI, nullptr, &m_HeightmapSampler); // repeating: the heightmap is a ring buffer


	samplerCI.magFilter = VK_FILTER_LINEAR;
	samplerCI.minFilter = VK_FILTER_LINEAR;

	samplerCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	
	vkCreateSampler(m_Device, &samplerCI, nullptr, &m_AtmosphereSampler);
	
	samplerCI.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	samplerCI.minLod = 0.0f;
	samplerCI.maxLod = 10.0f;
	samplerCI.mipLodBias = -0.33f;
	samplerCI.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerCI.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	
	// anisotropic filtering: steep and distant ground seen at a grazing angle smears into streaks without it.
	// VULKAN2_ANISOTROPY=N picks the level (1 = off), 8x by default (16x was very heavy on the Iris Xe)
	float anisotropy = 8.0f;
	if(const char* level = std::getenv("VULKAN2_ANISOTROPY")) anisotropy = std::max(1.0f, static_cast<float>(std::atof(level)));
	samplerCI.anisotropyEnable = anisotropy > 1.0f ? VK_TRUE : VK_FALSE;
	samplerCI.maxAnisotropy = anisotropy;
	
	vkCreateSampler(m_Device, &samplerCI, nullptr, &m_Sampler);
		
	m_DeletionQueue.push([&]() {
		vkDestroySampler(m_Device, m_Sampler, nullptr);
		vkDestroySampler(m_Device, m_HeightmapSampler, nullptr);
		vkDestroySampler(m_Device, m_AtmosphereSampler, nullptr);
		
		g_DescriptorAllocator.destroy_pool(m_Device);
	});
}

ComputeEffect Application::loadComputeShader(std::string path, std::string name, VkPipelineLayout layout) {
	CompiledShader computeShader = loadShader(path, VK_SHADER_STAGE_COMPUTE_BIT);
	
	VkComputePipelineCreateInfo computePipelineCreateInfo{};
	computePipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	computePipelineCreateInfo.pNext = nullptr;
	computePipelineCreateInfo.layout = layout;
	computePipelineCreateInfo.stage = computeShader.stageInfo;
	
	ComputeEffect computeEffect;
	computeEffect.layout = layout;
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
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE); // height
	builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE); // climate (half resolution)
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
	
	VkPipelineLayout heightmapLayout;
	
	VK_CHECK(vkCreatePipelineLayout(m_Device, &computeLayoutCI, nullptr, &heightmapLayout));
	
	m_DeletionQueue.push([&]() {
		vkDestroyDescriptorSetLayout(m_Device, m_HeightmapDescriptorLayout, nullptr);
	});
	
	createHeightmapImages();

	// create the compute shader to generate heightmap
	VkDescriptorImageInfo heightmapImgInfo{};
	
	heightmapImgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	heightmapImgInfo.imageView = m_HeightmapStorageView;

	VkWriteDescriptorSet heightmapWrite = {};
	heightmapWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	heightmapWrite.pNext = nullptr;
	heightmapWrite.dstBinding = 0;
	heightmapWrite.dstSet = m_HeightmapDescriptors;
	heightmapWrite.descriptorCount = 1;
	heightmapWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	heightmapWrite.pImageInfo = &heightmapImgInfo;

	vkUpdateDescriptorSets(m_Device, 1, &heightmapWrite, 0, nullptr);

	VkDescriptorImageInfo climateImgInfo{ VK_NULL_HANDLE, m_ClimateMap.imageView, VK_IMAGE_LAYOUT_GENERAL };
	heightmapWrite.dstBinding = 1;
	heightmapWrite.pImageInfo = &climateImgInfo;
	vkUpdateDescriptorSets(m_Device, 1, &heightmapWrite, 0, nullptr);

	m_HeightmapPC.offset = {0,0};
	m_HeightmapPC.dirtyMin = {0,0};
	m_HeightmapEffect = loadComputeShader("src/shaders/bin/heightmap.comp.spv", "heightmap shader", heightmapLayout);
	initHorizonAO();
	
	m_DeletionQueue.push([&]() {
		vkDestroyPipelineLayout(m_Device, m_HeightmapEffect.layout, nullptr);
		destroyHeightmapImages();
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

void Application::initTessellationPipeline() {
	const uint32_t MAX_TEXTURES = TERRAIN_MATERIALS; // one per biome + the rock strata, see loadTextures / terrainmaterial.glsl
	
	CompiledShader   vertexShader = loadShader("src/shaders/bin/terrain.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
	CompiledShader fragmentShader = loadShader("src/shaders/bin/terrain.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
	CompiledShader     tescShader = loadShader("src/shaders/bin/terrain.tesc.spv", VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT);
	CompiledShader     teseShader = loadShader("src/shaders/bin/terrain.tese.spv", VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT);
	
	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1); // regular heightmap
	
	builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_TEXTURES); // albedo
	builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_TEXTURES); // normal
	builder.add_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_TEXTURES); // pom
	builder.add_binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1); // climate map (half resolution)
	builder.add_binding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1); // sky visibility (horizon AO)
	builder.add_binding(6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1);		  // terrain cache: where each level is baked
	builder.add_binding(7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1); // terrain cache: albedo
	builder.add_binding(8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1); // terrain cache: normal
	
	// tesc reads the heightmap for LOD, the terrain cache's bake (compute) the textures
	m_TerrainDescriptorLayout = builder.build(m_Device, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT);
	m_TerrainDescriptors = g_DescriptorAllocator.allocate(m_Device, m_TerrainDescriptorLayout);

	VkPushConstantRange bufferRange{};
	bufferRange.offset = 0;
	bufferRange.size = sizeof(TessellationPushConstants);
	bufferRange.stageFlags = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	
	VkPipelineLayoutCreateInfo pipelineLayoutInfo = vkinit::pipeline_layout_create_info();
	pipelineLayoutInfo.pPushConstantRanges = &bufferRange;
	pipelineLayoutInfo.pushConstantRangeCount = 1;
	VkDescriptorSetLayout terrainLayouts[] = { m_TerrainDescriptorLayout, m_AtmosphereManager.getRenderDescriptorLayout(), m_Shadows.getDescriptorLayout() };
	pipelineLayoutInfo.pSetLayouts = terrainLayouts;
	pipelineLayoutInfo.setLayoutCount = 3;
	
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
	pipelineBuilder.enable_depthtest(false, VK_COMPARE_OP_EQUAL); // after the prepass: only the visible surface

	setGBufferTargets(pipelineBuilder, true); // deferred, lit in lighting.comp
	pipelineBuilder.set_depth_format(m_DepthBuffer.imageFormat);

	m_TerrainPipeline = pipelineBuilder.build_pipeline(m_Device);

	// depth prepass: same geometry, only the edge discards. hills hide each other a lot and the material shader
	// is the expensive part of the frame
	CompiledShader depthShader = loadShader("src/shaders/bin/terrain_depth.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
	pipelineBuilder._shaderStages.clear();
	pipelineBuilder.set_shaders(vertexShader.shader, depthShader.shader);
	pipelineBuilder.set_tessellation_shaders(tescShader.shader, teseShader.shader);
	pipelineBuilder.enable_depthtest(true, VK_COMPARE_OP_GREATER); // reversed z
	setGBufferTargets(pipelineBuilder, false);
	pipelineBuilder._colorBlendAttachment.colorWriteMask = 0;
	m_TerrainDepthPipeline = pipelineBuilder.build_pipeline(m_Device);
	vkDestroyShaderModule(m_Device, depthShader.shader, nullptr);

	// shadow casting version: same geometry, no fragment shader, no colour
	PipelineBuilder shadowBuilder;
	shadowBuilder._pipelineLayout = m_TerrainLayout;
	shadowBuilder._shaderStages.push_back(vkinit::pipeline_shader_stage_create_info(VK_SHADER_STAGE_VERTEX_BIT, vertexShader.shader));
	shadowBuilder.set_tessellation_shaders(tescShader.shader, teseShader.shader);
	shadowBuilder.set_tessellation_patch(4);
	shadowBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_PATCH_LIST);
	shadowBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
	shadowBuilder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE); // the light projection isn't y flipped, just draw both sides
	shadowBuilder._rasterizer.depthBiasEnable = VK_TRUE;
	shadowBuilder._rasterizer.depthBiasConstantFactor = 2.0f;
	shadowBuilder._rasterizer.depthBiasSlopeFactor = 2.5f;
	shadowBuilder.set_vertex_input({vertexBinding}, attributes);
	shadowBuilder.set_multisampling_none();
	shadowBuilder.disable_blending();
	shadowBuilder.enable_depthtest(true, VK_COMPARE_OP_LESS);
	shadowBuilder.set_depth_format(m_Shadows.getFormat());

	m_TerrainShadowPipeline = shadowBuilder.build_pipeline(m_Device);

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

	VkDescriptorImageInfo climateMapInfo{ m_HeightmapSampler, m_ClimateMap.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	descriptorWrite.dstBinding = 4;
	descriptorWrite.pImageInfo = &climateMapInfo;
	vkUpdateDescriptorSets(m_Device, 1, &descriptorWrite, 0, nullptr);

	VkDescriptorImageInfo skyVisibilityInfo{ m_HeightmapSampler, m_SkyVisibility.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	descriptorWrite.dstBinding = 5;
	descriptorWrite.pImageInfo = &skyVisibilityInfo;
	vkUpdateDescriptorSets(m_Device, 1, &descriptorWrite, 0, nullptr);

	vkDestroyShaderModule(m_Device, fragmentShader.shader, nullptr);
	vkDestroyShaderModule(m_Device, vertexShader.shader, nullptr);
	vkDestroyShaderModule(m_Device, tescShader.shader, nullptr);
	vkDestroyShaderModule(m_Device, teseShader.shader, nullptr);
	
	
	m_DeletionQueue.push([&]() {
		vkDestroyPipelineLayout(m_Device, m_TerrainLayout, nullptr);
		vkDestroyPipeline(m_Device, m_TerrainPipeline, nullptr);
		vkDestroyPipeline(m_Device, m_TerrainShadowPipeline, nullptr);
		vkDestroyPipeline(m_Device, m_TerrainDepthPipeline, nullptr);
		vkDestroyDescriptorSetLayout(m_Device, m_TerrainDescriptorLayout, nullptr);
	});
}

void Application::initPipelines() {
	initComputePipelines();
	initTessellationPipeline();
	m_AtmosphereManager.initSkyPipeline(this);
	initLighting();
	initTAA();
	m_Bloom.init(this);
	m_AtmosphereManager.setBloom(m_Bloom.getView(), m_Bloom.getSampler());
	m_HiZ.init(this);
	m_TerrainCache.init(this, m_TerrainDescriptors, m_TerrainDescriptorLayout, m_AtmosphereManager.getRenderDescriptorLayout());
}

void Application::initImGui() {
	VkDescriptorPoolSize pool_sizes[] = {
		{ VK_DESCRIPTOR_TYPE_SAMPLER, 10 },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 10 },
		{ VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 10 },
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 10 }
	};
		
	VkDescriptorPoolCreateInfo pool_info = {};
	pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
	pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	pool_info.maxSets = 10;
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
	
	// bound where nothing is picked: always readable by the time the UI is drawn
	m_DebugView.init(this, m_AtmosphereManager.transmittance.imageView, m_Shadows.getArrayView(), m_AtmosphereManager.aerial.imageView);

	m_DeletionQueue.push([this, imguiPool]() { // imguiPool is a local, don't capture it by reference
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


void Application::initDefaultData() {
	m_TextureLoader.pass(m_Device, m_Allocator);
	
	loadTextures();
	
	generateHeightmap();

	initProbe();
	parseShots();

	m_PlayerTrail.init(this);
	m_Foliage.init(this, m_Heightmap, m_ClimateMap, m_HeightmapSampler, m_Shadows.getFormat(), m_BladeAtlas, m_FlowerAtlas, m_WheatAtlas, m_Sampler,
				   m_AtmosphereManager.getRenderDescriptorLayout(), m_Shadows.getDescriptorLayout(), m_PlayerTrail.buffer());
	m_Foliage.setTerrainCache(m_TerrainCache.getInfoBuffer(), m_TerrainCache.getAlbedoView(), m_TerrainCache.getSampler());
	m_Foliage.setCanopyMap(m_SkyVisibility.imageView, m_HeightmapSampler);
	m_Trees.init(this, m_TextureLoader, m_Heightmap, m_ClimateMap, m_HeightmapSampler, m_Sampler,
				 m_AtmosphereManager.getRenderDescriptorLayout(), m_Shadows.getDescriptorLayout(), m_Shadows.getFormat(), m_PlayerTrail.buffer());
	// the props bring their own scanned textures, the moss on them is the grass
	m_Props.init(this, m_TextureLoader, m_Heightmap, m_ClimateMap, m_HeightmapSampler, m_AlbedoMaps[0].imageView, m_Sampler, m_Shadows.getFormat());
	m_GroundCover.init(this, m_TextureLoader, m_Heightmap, m_ClimateMap, m_HeightmapSampler, m_Sampler, m_Shadows.getFormat(), m_PlayerTrail.buffer());
	m_Trees.setOcclusionSource(m_HiZ.getView(), m_HiZ.getSampler());
	m_Precipitation.init(this, m_OutputImage.imageFormat, m_AtmosphereManager.getRenderDescriptorLayout());
	m_Precipitation.setDepth(m_DepthBuffer.imageView, m_NearestSampler);

	m_TerrainPC.factor = m_WorldSize * TERRAIN_HEIGHT_SCALE * m_TerrainScale;
	m_Camera.position.y = m_TerrainPC.factor * 0.5f;

	// graphics settings. VULKAN2_PRESET=low|medium|high|ultra|off picks a preset (for the captures)
	if(const char* preset = std::getenv("VULKAN2_PRESET")) {
		std::string name = preset;
		if(name == "low") m_Quality = Quality::Low;
		else if(name == "medium") m_Quality = Quality::Medium;
		else if(name == "ultra") m_Quality = Quality::Ultra;
		m_Settings = qualityPreset(m_Quality);
		if(name == "off") { m_Settings = everythingOff(m_Settings); m_Quality = Quality::Custom; }
	}
	// VULKAN2_CLOUDRES=0/1/2: the clouds at quarter / half / full resolution (comparison captures)
	if(const char* res = std::getenv("VULKAN2_CLOUDRES")) m_Settings.cloudResolution = std::atoi(res);
	// VULKAN2_SCALE=<render scale>: (profiling captures: does a pass scale with the pixels or the geometry)
	if(const char* scale = std::getenv("VULKAN2_SCALE")) m_Settings.renderScale = static_cast<float>(std::atof(scale));
	// VULKAN2_TREEDIST=<x>: the tree detail distance (comparison captures)
	if(const char* treeDist = std::getenv("VULKAN2_TREEDIST")) m_Settings.treeDetailDistance = static_cast<float>(std::atof(treeDist));
	// VULKAN2_TAA=0: TAA off (flicker captures)
	if(const char* taa = std::getenv("VULKAN2_TAA")) m_Settings.taa = std::atoi(taa) != 0;
	// VULKAN2_GROUNDCOVER=0: ground cover off (comparison captures; how much grows is the world's call, not a setting)
	if(const char* groundCover = std::getenv("VULKAN2_GROUNDCOVER")) m_Settings.groundCover = std::atof(groundCover) > 0.0f;
	if(!m_Shots.empty()) m_Settings.vsync = false; // see parseShots
	applySettings();
}

void Application::initProbe() {
	m_ProbeBuffer = createBuffer(sizeof(ProbeData) * FRAME_OVERLAP, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
	// walking collisions against the latest snapshot (the camera's xz is continuous world space)
	m_Camera.collide = [this](glm::vec3& position, float feet) {
		if(!m_Collision.valid()) return m_Camera.groundHeight;
		float toUnits = 1.0f / m_Camera.metersPerUnit;
		glm::vec2 xz(position.x, position.z);
		float ground = m_Collision.resolve(xz, feet, 0.35f * toUnits, m_Camera.stepHeight * toUnits);
		position.x = xz.x;
		position.z = xz.y;
		return ground;
	};

	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
	builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // climate map (tree / rock placement)
	m_ProbeLayout = builder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
	m_ProbeSet = g_DescriptorAllocator.allocate(m_Device, m_ProbeLayout);

	VkDescriptorImageInfo imageInfo{ m_HeightmapSampler, m_Heightmap.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkDescriptorImageInfo climateInfo{ m_HeightmapSampler, m_ClimateMap.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkDescriptorBufferInfo bufferInfo{ m_ProbeBuffer.buffer, 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet writes[3] = {};
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstSet = m_ProbeSet;
	writes[0].dstBinding = 0;
	writes[0].descriptorCount = 1;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[0].pImageInfo = &imageInfo;
	writes[1] = writes[0];
	writes[1].dstBinding = 1;
	writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	writes[1].pImageInfo = nullptr;
	writes[1].pBufferInfo = &bufferInfo;
	writes[2] = writes[0];
	writes[2].dstBinding = 2;
	writes[2].pImageInfo = &climateInfo;
	vkUpdateDescriptorSets(m_Device, 3, writes, 0, nullptr);

	VkPushConstantRange range{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ProbePushConstants) };
	VkPipelineLayoutCreateInfo layoutCI = vkinit::pipeline_layout_create_info();
	layoutCI.setLayoutCount = 1;
	layoutCI.pSetLayouts = &m_ProbeLayout;
	layoutCI.pushConstantRangeCount = 1;
	layoutCI.pPushConstantRanges = &range;
	VkPipelineLayout layout;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &layoutCI, nullptr, &layout));
	m_ProbeEffect = loadComputeShader("src/shaders/bin/probe.comp.spv", "terrain height probe", layout);

	m_DeletionQueue.push([this]() {
		vkDestroyPipelineLayout(m_Device, m_ProbeEffect.layout, nullptr);
		vkDestroyDescriptorSetLayout(m_Device, m_ProbeLayout, nullptr);
		destroyBuffer(m_ProbeBuffer);
	});
}

void Application::loadTextures() {
	// one material per biome, in the order of the BIOME_ defines in biometools.glsl
	// all from ambientCG (CC0): grass Grass001 (Grass004 had rows that striped whole hillsides), soil, rock, sand, taiga = forest floor Ground037
	const char* materials[] = {
		"grass", // BIOME_PLAINS
		"soil",	 // BIOME_WHEAT, wheat textures later
		"rock",	 // BIOME_MOUNTAINS + cliffs
		"sand",	 // BIOME_DESERT
		"taiga", // BIOME_TAIGA
		"snow",	 // BIOME_SNOW + snow caps
		// bare rock's variety (terrainmaterial.glsl rockCover), Poly Haven (CC0): other kinds of rock in patches
		// (rock_06, dark_rock_02, rock_boulder_cracked), a photographed cliff face (cliff_side)
		"rock_layer_buff",
		"rock_layer_dark",
		"rock_layer_rust",
		"rock_cliff",
		// the forest floor under the crowns (terrainmaterial.glsl g_LitterOver), Poly Haven (CC0): fallen leaves
		// with twigs and moss (forest_leaves_02), pine needles (forrest_ground_03)
		"leaf_litter",
		"pine_needles",
	};
	static_assert(std::size(materials) == TERRAIN_MATERIALS);
	for(const char* name : materials) {
		std::string base = std::string("assets\\") + name;
		m_AlbedoMaps.push_back(m_TextureLoader.loadCompressed(base + ".jpg", this, TextureKind::Albedo));
		m_NormalMaps.push_back(m_TextureLoader.loadCompressed(base + "_normal.jpg", this, TextureKind::Normal));
		m_HeightMaps.push_back(m_TextureLoader.loadCompressed(base + "_pom.jpg", this, TextureKind::Height));
	}

	m_BladeAtlas = m_TextureLoader.loadCompressed("assets\\grass_blades.png", this, TextureKind::Atlas);
	m_FlowerAtlas = m_TextureLoader.loadCompressed("assets\\flowers.png", this, TextureKind::Atlas);
	m_WheatAtlas = m_TextureLoader.loadCompressed("assets\\oat.png", this, TextureKind::Atlas);
	m_MoonTexture = m_TextureLoader.loadCompressed("assets\\moon.jpg", this, TextureKind::Albedo);
	m_AtmosphereManager.setMoonTexture(m_MoonTexture.imageView, m_Sampler);
	{
		// the cirrus' noise lattice: cloudHash (clouds.glsl) at every integer point of a 256 x 256 tile. the sky reads
		// a whole value noise octave in one filtered lookup instead of hashing four corners (that was ~1.8ms at 1080p)
		constexpr uint32_t size = 256;
		std::vector<uint16_t> lattice(size * size);
		auto fract = [](float v) { return v - std::floor(v); };
		for(uint32_t y = 0; y < size; y++) {
			for(uint32_t x = 0; x < size; x++) {
				glm::vec3 p3(fract(x * 0.1031f), fract(y * 0.1031f), fract(x * 0.1031f));
				p3 += glm::dot(p3, glm::vec3(p3.y, p3.z, p3.x) + 33.33f);
				lattice[y * size + x] = static_cast<uint16_t>(fract((p3.x + p3.y) * p3.z) * 65535.0f + 0.5f);
			}
		}
		m_CirrusNoise = m_TextureLoader.createTexture(lattice.data(), this, { size, size, 1 }, VK_FORMAT_R16_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT);
		m_AtmosphereManager.setCirrusNoise(m_CirrusNoise.imageView, m_CloudNoiseSampler); // linear, repeating
	}

	int textureNumber = m_AlbedoMaps.size();
	std::vector<VkDescriptorImageInfo> albedoInfo(textureNumber);
	std::vector<VkDescriptorImageInfo> normalInfo(textureNumber);
	std::vector<VkDescriptorImageInfo>    pomInfo(textureNumber);
	
	for(int i = 0; i < textureNumber; i++) {
		albedoInfo[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		albedoInfo[i].imageView = m_AlbedoMaps[i].imageView;
		albedoInfo[i].sampler = m_Sampler;
		
		normalInfo[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		normalInfo[i].imageView = m_NormalMaps[i].imageView;
		normalInfo[i].sampler = m_Sampler;
		
		   pomInfo[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		   pomInfo[i].imageView = m_HeightMaps[i].imageView;
		   pomInfo[i].sampler = m_Sampler;
	}
	
	VkWriteDescriptorSet writes[3] = {};
	
	writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	writes[0].dstBinding = 1;
	writes[0].dstSet = m_TerrainDescriptors;
	writes[0].descriptorCount = textureNumber;
	writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	
	writes[0].pImageInfo = albedoInfo.data();
	
	writes[1] = writes[0];
	writes[1].dstBinding = 2;
	writes[1].pImageInfo = normalInfo.data();
	
	writes[2] = writes[0];
	writes[2].dstBinding = 3;
	writes[2].pImageInfo = pomInfo.data();
	
	vkUpdateDescriptorSets(m_Device, 3, writes, 0, nullptr);	
	
	m_DeletionQueue.push([&]() {
		for(auto& image : m_AlbedoMaps) m_TextureLoader.destroyImage(image);
		for(auto& image : m_NormalMaps) m_TextureLoader.destroyImage(image);
		for(auto& image : m_HeightMaps) m_TextureLoader.destroyImage(image);
		m_TextureLoader.destroyImage(m_BladeAtlas);
		m_TextureLoader.destroyImage(m_MoonTexture);
		m_TextureLoader.destroyImage(m_CirrusNoise);
		m_TextureLoader.destroyImage(m_FlowerAtlas);
		m_TextureLoader.destroyImage(m_WheatAtlas);
	});
}

void Application::initTerrainPatches() {
	m_PatchVertices.clear();
	Vertex vertex{};
	vertex.position.y = 0.0f;
	unsigned rez = 128; // must match TERRAIN_PATCHES in terrainheight.glsl, smaller patches = finer max tessellation up close
	float width = m_WorldSize * m_TerrainScale;
	float height = m_WorldSize * m_TerrainScale;
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

void Application::createSwapchain(uint32_t width, uint32_t height, bool vsync) {
	vkb::SwapchainBuilder swapchainBuilder{ m_GPU, m_Device, m_Surface };
	
	m_SwapchainFormat = VK_FORMAT_B8G8R8A8_UNORM;
	
	vkb::Swapchain vkbSwapchain = swapchainBuilder
								  //.use_default_format_selection()
								  .set_desired_format( VkSurfaceFormatKHR { .format = m_SwapchainFormat, .colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
								  .set_desired_present_mode(vsync ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR ) // fifo is vsync
								  .set_desired_extent(width, height)
								  .add_image_usage_flags(VK_IMAGE_USAGE_TRANSFER_DST_BIT)
								  .build()
								  .value();
	  
	m_SwapchainExtent = vkbSwapchain.extent;
	m_Swapchain = vkbSwapchain.swapchain;
	m_SwapchainImages = vkbSwapchain.get_images().value();
	m_SwapchainImageViews = vkbSwapchain.get_image_views().value();
	
	VkSemaphoreCreateInfo semaphoreCreateInfo = vkinit::semaphore_create_info();
	m_RenderSemaphores.resize(m_SwapchainImages.size());
	for(VkSemaphore& semaphore : m_RenderSemaphores) {
		VK_CHECK(vkCreateSemaphore(m_Device, &semaphoreCreateInfo, nullptr, &semaphore));
	}
}

void Application::resizeSwapchain(bool vsync) {
	vkDeviceWaitIdle(m_Device);
	destroySwapchain();
	
	int w,h;
	SDL_GetWindowSizeInPixels(m_pWindow, &w, &h);
	if(w == 0 || h == 0) return; // minimized, try again next frame
	m_Width = w * m_RenderScale;
	m_Height = h * m_RenderScale;

	initSwapchain(vsync);
	m_AtmosphereManager.setRenderTargets(m_OutputImage); // post processing, at the output resolution
	m_ShadowFilter.setTargets(m_DrawImage, m_DepthBuffer);
	writeLightingDescriptors();
	writeTAADescriptors();
	m_Bloom.setTargets(m_OutputImage);
	m_AtmosphereManager.setBloom(m_Bloom.getView(), m_Bloom.getSampler());
	m_Precipitation.setDepth(m_DepthBuffer.imageView, m_NearestSampler);
	m_HiZ.setSource(m_DepthBuffer);
	m_Trees.setOcclusionSource(m_HiZ.getView(), m_HiZ.getSampler());
	m_HiZValid = false;

	m_Resized = false;
}

void Application::destroySwapchain() {
	vkDestroySwapchainKHR(m_Device, m_Swapchain, nullptr);
	
	for(int i = 0; i < m_SwapchainImageViews.size(); i++) {
		vkDestroyImageView(m_Device, m_SwapchainImageViews[i], nullptr);
	}
	for(VkSemaphore semaphore : m_RenderSemaphores) {
		vkDestroySemaphore(m_Device, semaphore, nullptr);
	}
	m_RenderSemaphores.clear();
	
	vkDestroyImageView(m_Device, m_DrawImage.imageView, nullptr);
	vmaDestroyImage(m_Allocator, m_DrawImage.image, m_DrawImage.allocation);
		
	vkDestroyImageView(m_Device, m_DepthBuffer.imageView, nullptr);
	vmaDestroyImage(m_Allocator, m_DepthBuffer.image, m_DepthBuffer.allocation);

	for(AllocatedImage* target : { &m_GAlbedo, &m_GNormal, &m_GMaterial, &m_GVelocity, &m_History[0], &m_History[1], &m_SSAOImage, &m_CloudImage, &m_CloudRaw, &m_CloudHistory, &m_CloudDepth, &m_CloudDepthHistory, &m_OutputImage }) {
		vkDestroyImageView(m_Device, target->imageView, nullptr);
		vmaDestroyImage(m_Allocator, target->image, target->allocation);
	}
}

void Application::generateHeightmap() {
	// fresh images into the layout the generation expects, then the whole map in strips
	immediateSubmit([&](VkCommandBuffer cmd) {
		vkutil::transition_image(cmd, m_Heightmap.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		vkutil::transition_image(cmd, m_ClimateMap.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	});
	Uint64 start = SDL_GetPerformanceCounter();
	regenerateHeightmap();
	double ms = static_cast<double>(SDL_GetPerformanceCounter() - start) * 1000.0 / static_cast<double>(SDL_GetPerformanceFrequency());
	fmt::print("heightmap: {0}x{0} generated in {1:.0f} ms\n", m_HeightmapSize, ms);
}

void Application::updateHeightmap() {
	// heightmap scrolling works in texels, on whole unscaled terrain (grid) units
	glm::ivec2 pos = glm::ivec2(glm::round(glm::vec2(m_Camera.position.x, m_Camera.position.z) / m_TerrainScale)) * m_CoordinateMultiplier;

	glm::ivec2 delta = m_HeightmapPC.offset - pos; // texels
	glm::ivec2 moveDelta = m_MapOffset - pos;
	int minimumMovement = m_WorldSize / 64 * m_CoordinateMultiplier; // 64 is the "rez" used to generate the patchs
	glm::ivec2 oldMapOffset = m_MapOffset;
	if(std::abs(moveDelta.x) >= minimumMovement) m_MapOffset.x = pos.x;
	if(std::abs(moveDelta.y) >= minimumMovement) m_MapOffset.y = pos.y;
	// the world origin moved: the shadow cascades rendered with the old one are re-expressed in the new space (they
	// were all re-rendered, a stutter every few hundred metres)
	if(m_MapOffset != oldMapOffset) {
		float offsetScale = m_TerrainScale / static_cast<float>(m_CoordinateMultiplier);
		glm::vec2 d = glm::vec2(m_MapOffset - oldMapOffset) * offsetScale;
		m_Shadows.shiftOrigin(glm::vec3(d.x, 0.0f, d.y));
		m_FrameEvents |= FRAME_ORIGIN;
	}

	// the strips the camera moved into, once it has moved a few texels: every texel was its own strip, each one
	// with the horizon AO of ~200 texels around it (its rays reach that far), and a blocking submit between the
	// frames. 16 texels of lag at the far edge of the map is out past the render distance (the map origin lags 64)
	const int step = std::max(4, m_HeightmapSize / 256);
	bool far = std::abs(delta.x) >= m_HeightmapSize || std::abs(delta.y) >= m_HeightmapSize;
	if(!far && std::abs(delta.x) < step && std::abs(delta.y) < step) return;

	m_HeightmapPC.offset = pos;
	m_FrameEvents |= FRAME_HEIGHTMAP;
	{
		HeightmapPushConstants pc = m_HeightmapPC;
		
		glm::ivec2 dirtyMin;
		dirtyMin.x = pos.x % m_HeightmapSize;
		dirtyMin.y = pos.y % m_HeightmapSize;
		if(dirtyMin.x < 0) dirtyMin.x += m_HeightmapSize;
		if(dirtyMin.y < 0) dirtyMin.y += m_HeightmapSize;
		
		pc.dirtyMin = dirtyMin;
		if(delta.x != 0) {
			uint32_t dispatchWidth = std::min((uint32_t)m_HeightmapSize,(uint32_t)std::abs(delta.x));
			uint32_t dispatchHeight  = m_HeightmapSize;
			
			HeightmapPushConstants pcX = pc;
			
			if(delta.x < 0) {
				uint32_t offsetX = m_HeightmapSize - dispatchWidth;
				pcX.offset.x += offsetX;
				pcX.dirtyMin.x = (pcX.dirtyMin.x + offsetX) % m_HeightmapSize;
			}
			pcX.updateSize.x = dispatchWidth;
			pcX.updateSize.y = dispatchHeight;
			m_PendingHeightmap.push_back({ pcX, pcX.dirtyMin, pcX.updateSize });
		}
		if(delta.y != 0) {
			HeightmapPushConstants pcY = pc;
			
			uint32_t dispatchWidth  = m_HeightmapSize;
			uint32_t dispatchHeight = std::min((uint32_t)m_HeightmapSize,(uint32_t)std::abs(delta.y));
			
			if(delta.y < 0) {
				uint32_t offsetY = m_HeightmapSize - dispatchHeight;
				pcY.offset.y += offsetY;
				pcY.dirtyMin.y = (pcY.dirtyMin.y + offsetY) % m_HeightmapSize;
			}
			pcY.updateSize.x = dispatchWidth;
			pcY.updateSize.y = dispatchHeight;
			m_PendingHeightmap.push_back({ pcY, pcY.dirtyMin, pcY.updateSize });
		}
	}
}

void Application::recordHeightmapUpdate(VkCommandBuffer cmd) {
	if(m_PendingHeightmap.empty()) return;
	vkutil::transition_image(cmd, m_Heightmap.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
	vkutil::transition_image(cmd, m_ClimateMap.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_HeightmapEffect.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_HeightmapEffect.layout, 0, 1, &m_HeightmapDescriptors, 0, nullptr);
	for(const HeightmapStrip& strip : m_PendingHeightmap) {
		vkCmdPushConstants(cmd, m_HeightmapEffect.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(HeightmapPushConstants), &strip.pc);
		vkCmdDispatch(cmd, (strip.pc.updateSize.x + 15) / 16, (strip.pc.updateSize.y + 15) / 16, 1);
	}
	vkutil::transition_image(cmd, m_Heightmap.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	vkutil::transition_image(cmd, m_ClimateMap.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	for(const HeightmapStrip& strip : m_PendingHeightmap) dispatchHorizonAO(cmd, strip.aoStart, strip.aoSize);
	m_PendingHeightmap.clear();
}

void Application::regenerateHeightmap() {
	glm::ivec2 pos = glm::ivec2(glm::round(glm::vec2(m_Camera.position.x, m_Camera.position.z) / m_TerrainScale)) * m_CoordinateMultiplier;
	m_HeightmapPC.offset = pos;
	glm::ivec2 dirtyMin = ((pos % m_HeightmapSize) + m_HeightmapSize) % m_HeightmapSize;
	// ~2M texels a submit (512 rows of a 4096 map): the whole of an 8192 map in one submit would risk the driver's
	// timeout. the horizon AO only once every strip's heights are in
	const int rows = std::max(16, (4096 * 512 / m_HeightmapSize) / 16 * 16);
	for(int y = 0; y < m_HeightmapSize; y += rows) {
		immediateSubmit([&](VkCommandBuffer cmd) {
			vkutil::transition_image(cmd, m_Heightmap.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
			vkutil::transition_image(cmd, m_ClimateMap.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_HeightmapEffect.pipeline);
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_HeightmapEffect.layout, 0, 1, &m_HeightmapDescriptors, 0, nullptr);
			HeightmapPushConstants pc = m_HeightmapPC;
			pc.offset.y += y;
			pc.dirtyMin = glm::ivec2(dirtyMin.x, (dirtyMin.y + y) % m_HeightmapSize);
			pc.updateSize = glm::ivec2(m_HeightmapSize, std::min(rows, m_HeightmapSize - y));
			vkCmdPushConstants(cmd, m_HeightmapEffect.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(HeightmapPushConstants), &pc);
			vkCmdDispatch(cmd, (pc.updateSize.x + 15) / 16, (pc.updateSize.y + 15) / 16, 1);
			vkutil::transition_image(cmd, m_Heightmap.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			vkutil::transition_image(cmd, m_ClimateMap.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		});
	}
	for(int y = 0; y < m_HeightmapSize; y += rows * 4) { // the AO is half resolution, cheaper per texel
		immediateSubmit([&](VkCommandBuffer cmd) {
			dispatchHorizonAO(cmd, glm::ivec2(0, y), glm::ivec2(m_HeightmapSize, std::min(rows * 4, m_HeightmapSize - y)));
		});
	}
}

void Application::updateCamera() {
	float offsetScale = m_TerrainScale / static_cast<float>(m_CoordinateMultiplier);
	m_View = m_Camera.getViewMatrix({m_MapOffset.x * offsetScale, 0.0, m_MapOffset.y * offsetScale});
	m_CameraPos = glm::vec3(glm::inverse(m_View)[3]); // actual camera position in terrain space
	{
		float offsetScale = m_TerrainScale / static_cast<float>(m_CoordinateMultiplier);
		m_PlayerPos = m_Camera.position - glm::vec3(m_MapOffset.x * offsetScale, 0.0f, m_MapOffset.y * offsetScale);
	}
	
	// reversed z (near -> 1, far -> 0) so a near plane of a few centimetres still has plenty of depth precision
	// in the distance. the far plane only has to cover the loaded world, the sky is drawn at depth 0
	const float nearPlane = 0.02f, farPlane = 20000.0f;
	// 70 degrees, narrowed by the zoom (C, camera.cpp)
	float fov = 2.0f * std::atan(std::tan(glm::radians(35.0f)) / m_Camera.zoom);
	m_Projection = glm::perspective(fov, (float)m_DrawExtent.width / (float)m_DrawExtent.height, farPlane, nearPlane);
	m_Projection[1][1] *= -1; // vulkan had to go out of its way to break the standard and make the Y axis reversed ):
	m_TerrainPC.projScale = std::abs(m_Projection[1][1]) * m_DrawExtent.height * 0.5f;

	// TAA: last frame's camera (unjittered) rebuilt in this frame's terrain space (the map offset may have moved),
	// then this frame's sub pixel jitter (Halton 2, 3 over 8 frames) goes into the projection everything uses
	m_UnjitteredProjection = m_Projection;
	{
		Camera previous = m_Camera;
		previous.position = m_PreviousCameraPosition;
		previous.yaw = m_PreviousYaw;
		previous.pitch = m_PreviousPitch;
		// (last frame's projection: while zooming it changes, this frame's reprojected the history wrongly)
		if(m_PreviousProjection[3][2] == 0.0f) m_PreviousProjection = m_UnjitteredProjection;
		m_PreviousViewProj = m_PreviousProjection * previous.getViewMatrix({m_MapOffset.x * offsetScale, 0.0, m_MapOffset.y * offsetScale});
		m_PreviousProjection = m_UnjitteredProjection;
		m_PreviousCameraPosition = m_Camera.position;
		m_PreviousYaw = m_Camera.yaw;
		m_PreviousPitch = m_Camera.pitch;
	}
	if(m_TAA) {
		auto halton = [](uint32_t index, uint32_t base) {
			float f = 1.0f, r = 0.0f;
			for(; index > 0; index /= base) { f /= static_cast<float>(base); r += f * static_cast<float>(index % base); }
			return r;
		};
		// 16 steps: below render scale 1 the history rebuilds the full resolution out of 16 differently jittered frames
		uint32_t index = static_cast<uint32_t>(m_FrameNumber) % 16 + 1;
		// how far the image actually moves, in pixels: the offset goes into the z column and clip w = -z, so the
		// picture shifts by minus it (taa.comp samples the current frame at the shifted position)
		m_Jitter = -glm::vec2(halton(index, 2) - 0.5f, halton(index, 3) - 0.5f);
		m_Projection[2][0] += (halton(index, 2) - 0.5f) * 2.0f / static_cast<float>(m_DrawExtent.width);
		m_Projection[2][1] += (halton(index, 3) - 0.5f) * 2.0f / static_cast<float>(m_DrawExtent.height);
	}

	m_SunDir = m_AtmosphereManager.getSunDir();
	m_TerrainPC.sundir = m_SunDir;
	m_TerrainPC.campos = m_CameraPos;
}

// records the terrain draw, the pipeline has to be bound already (main or shadow)
void Application::drawTerrain(VkCommandBuffer cmd, const glm::mat4& viewproj, const ShadowCascadeDraw* shadow) {
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_TerrainLayout, 0, 1, &m_TerrainDescriptors, 0, nullptr);

	TessellationPushConstants pc = m_TerrainPC;
	pc.viewproj = viewproj; // LOD still comes from the camera so the shadow geometry matches what's on screen
	pc.worldoffset = m_MapOffset;
	// the terrain is drawn relative to the map offset, adding it back (modulo the period all the detail texture
	// layers repeat at, TERRAIN_DETAIL_PERIOD, so it stays small and precise) gives them continuous world coordinates
	// (modulo the far repeat alone shifted the rotated far / macro layers every time the map re-centred)
	const double DETAIL_PERIOD = 1050.0; // terrainmaterial.glsl
	double offsetScale = m_TerrainScale / static_cast<double>(m_CoordinateMultiplier);
	glm::dvec2 offsetWorld = glm::dvec2(m_MapOffset) * offsetScale;
	pc.detailOffset = glm::vec2(offsetWorld - DETAIL_PERIOD * glm::floor(offsetWorld / DETAIL_PERIOD));
	if(shadow) {
		pc.fixedTessLevel = shadow->tessLevel;
		pc.sundir = glm::vec3(shadow->center.x, shadow->radius, shadow->center.z); // see shadowTessFactor in terrain.tesc
	}
	
	vkCmdPushConstants(cmd, m_TerrainLayout, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT | VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(TessellationPushConstants), &pc);

	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &m_TerrainVertexBuffer.buffer, &offset);

	vkCmdDraw(cmd, static_cast<uint32_t>(m_PatchVertices.size()), 1, 0, 0);
}

void Application::markStart(VkCommandBuffer cmd) {
	vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_TimestampPool, (m_FrameNumber % FRAME_OVERLAP) * GPU_QUERIES);
}

void Application::mark(VkCommandBuffer cmd, const char* name) {
	std::vector<const char*>& names = m_MarkNames[m_FrameNumber % FRAME_OVERLAP];
	if(names.size() >= GPU_MAX_MARKS) return;
	names.push_back(name);
	vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, m_TimestampPool, (m_FrameNumber % FRAME_OVERLAP) * GPU_QUERIES + static_cast<uint32_t>(names.size()));
}

void Application::drawGeometry(VkCommandBuffer cmd) {
	VkClearValue depthClear{};
    depthClear.depthStencil.depth = 0.0f; // reversed z

	// G-buffer pass: the opaque materials only write what the surface is (gbuffer.glsl). nothing ever reads the
	// G-buffer where the sky is, so the first rendering doesn't load or clear it. after a split everything loads
	auto beginGBuffer = [&](bool first) {
		VkClearValue noMotion{};
		VkRenderingAttachmentInfo colorAttachments[4] = {
			vkinit::attachment_info(m_GAlbedo.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
			vkinit::attachment_info(m_GNormal.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
			vkinit::attachment_info(m_GMaterial.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
			vkinit::attachment_info(m_GVelocity.imageView, first ? &noMotion : nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
		};
		// TAA reads the velocity everywhere (the sky too), so that one is cleared
		// with the debug textures open the others are cleared too, so the sky shows as empty instead of stale frames
		if(first) for(int i = 0; i < 3; i++) {
			colorAttachments[i].loadOp = m_DebugView.m_Open ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
			colorAttachments[i].clearValue = noMotion; // zero
		}
		VkRenderingAttachmentInfo depthAttachment = vkinit::attachment_info(m_DepthBuffer.imageView, first ? &depthClear : nullptr, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
		VkRenderingInfo renderInfo = vkinit::rendering_info(m_DrawExtent, colorAttachments, &depthAttachment);
		renderInfo.colorAttachmentCount = 4;
		vkCmdBeginRendering(cmd, &renderInfo);
	};

	// GPU timer boundaries between the parts of the pass: with the breakdown on the rendering is ended for each
	// one (timestamps inside a render pass are useless on Intel), otherwise the whole pass is one timer
	auto split = [&](const char* name) {
		if(!m_DetailedOpaqueTimings) return;
		vkCmdEndRendering(cmd);
		mark(cmd, name);
		beginGBuffer(false);
	};

	beginGBuffer(true);
	// dynamic state, survives the splits
	VkViewport viewport = { 0.0f, 0.0f, static_cast<float>(m_DrawExtent.width), static_cast<float>(m_DrawExtent.height), 0.0f, 1.0f };
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	VkRect2D scissor = { {0, 0}, m_DrawExtent };
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	m_TerrainPC.sundir = m_SunDir;
	m_TerrainPC.campos = m_CameraPos;
	m_TerrainPC.time = m_Time;

	// ---
	VkDescriptorSet terrainSets[] = { m_TerrainDescriptors, m_AtmosphereManager.getRenderDescriptor(), m_Shadows.getDescriptor(m_FrameNumber) };
	// depth first, then the material shader only on the visible surface
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_TerrainDepthPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_TerrainLayout, 0, 3, terrainSets, 0, nullptr);
	drawTerrain(cmd, m_Projection * m_View);
	split("terrain depth prepass");
	// the trees' depth too before any material is shaded: the terrain is the most expensive material and a forest
	// hides most of the ground behind it
	m_Trees.drawPrepass(cmd);
	split("tree depth prepass");
	m_Foliage.drawPrepass(cmd);
	split("grass depth prepass");
	// the opaque small things (shaded as they're drawn, they write their own depth) before the terrain too, the
	// ground under them is never seen
	m_Props.draw(cmd);
	split("rocks & logs");
	m_GroundCover.draw(cmd);
	split("ground cover");
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_TerrainPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_TerrainLayout, 0, 3, terrainSets, 0, nullptr);
	drawTerrain(cmd, m_Projection * m_View);
	split("terrain");
	// --- trees | impostors
	m_Trees.draw(cmd, m_AtmosphereManager.getRenderDescriptor(), m_Shadows.getDescriptor(m_FrameNumber), [&]() { split("trees"); });
	split("tree impostors");
	m_Foliage.draw(cmd, m_AtmosphereManager.getRenderDescriptor(), m_Shadows.getDescriptor(m_FrameNumber));
	vkCmdEndRendering(cmd);
	mark(cmd, m_DetailedOpaqueTimings ? "grass" : "G-buffer pass (breakdown off)");

	// --- the sky where nothing drew (depth still 0), straight into the draw image + its direct light target
	VkRenderingAttachmentInfo skyAttachments[2] = {
		vkinit::attachment_info(m_DrawImage.imageView, nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
		vkinit::attachment_info(m_ShadowFilter.getDirectView(), nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
	};
	for(auto& attachment : skyAttachments) attachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // lighting.comp writes the rest
	VkRenderingAttachmentInfo depthAttachment = vkinit::attachment_info(m_DepthBuffer.imageView, nullptr, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
	VkRenderingInfo skyInfo = vkinit::rendering_info(m_DrawExtent, skyAttachments, &depthAttachment);
	skyInfo.colorAttachmentCount = 2;
	vkCmdBeginRendering(cmd, &skyInfo);
	m_AtmosphereManager.draw(cmd);
	vkCmdEndRendering(cmd);
	mark(cmd, "sky");

	// --- everything else gets lit, once per pixel
	drawLighting(cmd);
	mark(cmd, "deferred lighting");
}

void Application::initLighting() {
	VkSamplerCreateInfo samplerCI = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	samplerCI.magFilter = VK_FILTER_NEAREST;
	samplerCI.minFilter = VK_FILTER_NEAREST;
	samplerCI.addressModeU = samplerCI.addressModeV = samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VK_CHECK(vkCreateSampler(m_Device, &samplerCI, nullptr, &m_NearestSampler));

	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // albedo + AO
	builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // normal
	builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // material
	builder.add_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // depth
	builder.add_binding(4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);		   // draw image
	builder.add_binding(5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);		   // direct light + shadow (ShadowFilter)
	builder.add_binding(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // sky visibility (horizon AO)
	builder.add_binding(7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // SSAO
	builder.add_binding(8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // volumetric clouds (half resolution)
	builder.add_binding(9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // cloud coverage map
	builder.add_binding(10, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);		   // where the map is
	builder.add_binding(11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // the climate map (snow / wetness follow it)
	builder.add_binding(12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // the heightmap (the local climate: snow / wetness)
	m_LightingLayout = builder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
	m_LightingSet = g_DescriptorAllocator.allocate(m_Device, m_LightingLayout);

	VkDescriptorSetLayout layouts[] = { m_LightingLayout, m_AtmosphereManager.getRenderDescriptorLayout(), m_Shadows.getDescriptorLayout() };
	VkPushConstantRange range{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(LightingPushConstants) };
	VkPipelineLayoutCreateInfo layoutCI = vkinit::pipeline_layout_create_info();
	layoutCI.setLayoutCount = 3;
	layoutCI.pSetLayouts = layouts;
	layoutCI.pushConstantRangeCount = 1;
	layoutCI.pPushConstantRanges = &range;
	VkPipelineLayout layout;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &layoutCI, nullptr, &layout));
	m_LightingEffect = loadComputeShader("src/shaders/bin/lighting.comp.spv", "deferred lighting", layout);

	DescriptorLayoutBuilder ssaoBuilder;
	ssaoBuilder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // depth
	ssaoBuilder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // normals
	ssaoBuilder.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);		   // occlusion
	m_SSAOLayout = ssaoBuilder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
	m_SSAOSet = g_DescriptorAllocator.allocate(m_Device, m_SSAOLayout);
	VkPushConstantRange ssaoRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(glm::mat4) + sizeof(glm::vec4) * 3 };
	VkDescriptorSetLayout ssaoSetLayouts[] = { m_SSAOLayout, m_Shadows.getDescriptorLayout() }; // + the shadow maps (the lit air)
	VkPipelineLayoutCreateInfo ssaoLayoutCI = vkinit::pipeline_layout_create_info();
	ssaoLayoutCI.setLayoutCount = 2;
	ssaoLayoutCI.pSetLayouts = ssaoSetLayouts;
	ssaoLayoutCI.pushConstantRangeCount = 1;
	ssaoLayoutCI.pPushConstantRanges = &ssaoRange;
	VkPipelineLayout ssaoLayout;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &ssaoLayoutCI, nullptr, &ssaoLayout));
	m_SSAOEffect = loadComputeShader("src/shaders/bin/ssao.comp.spv", "ssao", ssaoLayout);

	initClouds();
	initVolumetricFog(); // needs the cloud map
	writeLightingDescriptors();
	m_DeletionQueue.push([&]() {
		vkDestroyPipelineLayout(m_Device, m_LightingEffect.layout, nullptr);
		vkDestroyPipelineLayout(m_Device, m_SSAOEffect.layout, nullptr);
		vkDestroyDescriptorSetLayout(m_Device, m_SSAOLayout, nullptr);
		vkDestroyDescriptorSetLayout(m_Device, m_LightingLayout, nullptr);
		vkDestroySampler(m_Device, m_NearestSampler, nullptr);
	});
}

// the volumetric clouds: their noise lookup table (generated once), the half resolution pass
void Application::initClouds() {
	// 64^3 tiling noise, RGBA8 (a storage format every GPU has)
	m_CloudNoise.imageFormat = VK_FORMAT_R8G8B8A8_UNORM;
	m_CloudNoise.imageExtent = { 64, 64, 64 };
	VkImageCreateInfo noiseCI = vkinit::image_create_info(m_CloudNoise.imageFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, m_CloudNoise.imageExtent);
	noiseCI.imageType = VK_IMAGE_TYPE_3D;
	VmaAllocationCreateInfo allocCI{ .usage = VMA_MEMORY_USAGE_GPU_ONLY };
	VK_CHECK(vmaCreateImage(m_Allocator, &noiseCI, &allocCI, &m_CloudNoise.image, &m_CloudNoise.allocation, nullptr));
	VkImageViewCreateInfo noiseViewCI = vkinit::imageview_create_info(m_CloudNoise.imageFormat, m_CloudNoise.image, VK_IMAGE_ASPECT_COLOR_BIT);
	noiseViewCI.viewType = VK_IMAGE_VIEW_TYPE_3D;
	VK_CHECK(vkCreateImageView(m_Device, &noiseViewCI, nullptr, &m_CloudNoise.imageView));
	VkSamplerCreateInfo samplerCI = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
	samplerCI.magFilter = samplerCI.minFilter = VK_FILTER_LINEAR;
	samplerCI.addressModeU = samplerCI.addressModeV = samplerCI.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT; // it tiles
	VK_CHECK(vkCreateSampler(m_Device, &samplerCI, nullptr, &m_CloudNoiseSampler));

	{
		DescriptorLayoutBuilder noiseBuilder;
		noiseBuilder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
		VkDescriptorSetLayout noiseLayout = noiseBuilder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
		VkDescriptorSet noiseSet = g_DescriptorAllocator.allocate(m_Device, noiseLayout);
		VkDescriptorImageInfo info{ VK_NULL_HANDLE, m_CloudNoise.imageView, VK_IMAGE_LAYOUT_GENERAL };
		VkWriteDescriptorSet write{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		write.dstSet = noiseSet; write.dstBinding = 0; write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; write.pImageInfo = &info;
		vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);
		VkPipelineLayoutCreateInfo layoutCI = vkinit::pipeline_layout_create_info();
		layoutCI.setLayoutCount = 1;
		layoutCI.pSetLayouts = &noiseLayout;
		VkPipelineLayout layout;
		VK_CHECK(vkCreatePipelineLayout(m_Device, &layoutCI, nullptr, &layout));
		ComputeEffect noise = loadComputeShader("src/shaders/bin/cloudnoise.comp.spv", "cloud noise", layout);
		immediateSubmit([&](VkCommandBuffer cmd) {
			vkutil::transition_image(cmd, m_CloudNoise.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, noise.pipeline);
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, noise.layout, 0, 1, &noiseSet, 0, nullptr);
			vkCmdDispatch(cmd, 16, 16, 16);
			vkutil::transition_image(cmd, m_CloudNoise.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		});
		vkDestroyPipelineLayout(m_Device, layout, nullptr); // the pipeline itself goes with the application's
		vkDestroyDescriptorSetLayout(m_Device, noiseLayout, nullptr);
	}

	// the coverage map: RGBA16F (r 3 octaves, g the cloud shadow, b weather field, a billows), GENERAL for good (written + sampled)
	m_CloudMap.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
	m_CloudMap.imageExtent = { 1024, 1024, 1 };
	VkImageCreateInfo mapCI = vkinit::image_create_info(m_CloudMap.imageFormat, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, m_CloudMap.imageExtent);
	VK_CHECK(vmaCreateImage(m_Allocator, &mapCI, &allocCI, &m_CloudMap.image, &m_CloudMap.allocation, nullptr));
	VkImageViewCreateInfo mapViewCI = vkinit::imageview_create_info(m_CloudMap.imageFormat, m_CloudMap.image, VK_IMAGE_ASPECT_COLOR_BIT);
	VK_CHECK(vkCreateImageView(m_Device, &mapViewCI, nullptr, &m_CloudMap.imageView));
	m_CloudMapInfo = createBuffer(sizeof(glm::vec4), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	immediateSubmit([&](VkCommandBuffer cmd) {
		vkutil::transition_image(cmd, m_CloudMap.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
		glm::vec4 invalid(0.0f); // w = 0: not generated yet, everything uses the formula
		vkCmdUpdateBuffer(cmd, m_CloudMapInfo.buffer, 0, sizeof(invalid), &invalid);
	});
	{
		DescriptorLayoutBuilder mapBuilder;
		mapBuilder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
		VkDescriptorSetLayout mapLayout = mapBuilder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
		m_CloudMapSet = g_DescriptorAllocator.allocate(m_Device, mapLayout);
		VkDescriptorImageInfo info{ VK_NULL_HANDLE, m_CloudMap.imageView, VK_IMAGE_LAYOUT_GENERAL };
		VkWriteDescriptorSet write{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		write.dstSet = m_CloudMapSet; write.dstBinding = 0; write.descriptorCount = 1;
		write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; write.pImageInfo = &info;
		vkUpdateDescriptorSets(m_Device, 1, &write, 0, nullptr);
		VkPushConstantRange mapRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CloudMapPushConstants) };
		VkDescriptorSetLayout mapLayouts[] = { mapLayout, m_AtmosphereManager.getRenderDescriptorLayout() };
		VkPipelineLayoutCreateInfo mapLayoutCI = vkinit::pipeline_layout_create_info();
		mapLayoutCI.setLayoutCount = 2;
		mapLayoutCI.pSetLayouts = mapLayouts;
		mapLayoutCI.pushConstantRangeCount = 1;
		mapLayoutCI.pPushConstantRanges = &mapRange;
		VkPipelineLayout layout;
		VK_CHECK(vkCreatePipelineLayout(m_Device, &mapLayoutCI, nullptr, &layout));
		m_CloudMapEffect = loadComputeShader("src/shaders/bin/cloudmap.comp.spv", "cloud coverage map", layout);
		// the shadow bake: the same map written (g) and read (the clouds' shape), and where it is
		DescriptorLayoutBuilder shadowBuilder;
		shadowBuilder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
		shadowBuilder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
		shadowBuilder.add_binding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
		VkDescriptorSetLayout shadowLayout = shadowBuilder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
		m_CloudShadowSet = g_DescriptorAllocator.allocate(m_Device, shadowLayout);
		VkDescriptorImageInfo shadowImage{ VK_NULL_HANDLE, m_CloudMap.imageView, VK_IMAGE_LAYOUT_GENERAL };
		VkDescriptorImageInfo shadowSampled{ m_HeightmapSampler, m_CloudMap.imageView, VK_IMAGE_LAYOUT_GENERAL }; // linear, repeating
		VkDescriptorBufferInfo shadowWhere{ m_CloudMapInfo.buffer, 0, sizeof(glm::vec4) };
		VkWriteDescriptorSet shadowWrites[3] = {};
		for(uint32_t i = 0; i < 3; i++) { shadowWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; shadowWrites[i].dstSet = m_CloudShadowSet; shadowWrites[i].dstBinding = i; shadowWrites[i].descriptorCount = 1; }
		shadowWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; shadowWrites[0].pImageInfo = &shadowImage;
		shadowWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; shadowWrites[1].pImageInfo = &shadowSampled;
		shadowWrites[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; shadowWrites[2].pBufferInfo = &shadowWhere;
		vkUpdateDescriptorSets(m_Device, 3, shadowWrites, 0, nullptr);
		VkPushConstantRange shadowRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CloudShadowPushConstants) };
		VkDescriptorSetLayout shadowLayouts[] = { shadowLayout, m_AtmosphereManager.getRenderDescriptorLayout() };
		VkPipelineLayoutCreateInfo shadowLayoutCI = vkinit::pipeline_layout_create_info();
		shadowLayoutCI.setLayoutCount = 2;
		shadowLayoutCI.pSetLayouts = shadowLayouts;
		shadowLayoutCI.pushConstantRangeCount = 1;
		shadowLayoutCI.pPushConstantRanges = &shadowRange;
		VkPipelineLayout shadowPipelineLayout;
		VK_CHECK(vkCreatePipelineLayout(m_Device, &shadowLayoutCI, nullptr, &shadowPipelineLayout));
		m_CloudShadowEffect = loadComputeShader("src/shaders/bin/cloudshadow.comp.spv", "cloud shadow bake", shadowPipelineLayout);
		m_DeletionQueue.push([this, mapLayout, shadowLayout]() {
			vkDestroyPipelineLayout(m_Device, m_CloudShadowEffect.layout, nullptr);
			vkDestroyDescriptorSetLayout(m_Device, shadowLayout, nullptr);
			vkDestroyPipelineLayout(m_Device, m_CloudMapEffect.layout, nullptr);
			vkDestroyDescriptorSetLayout(m_Device, mapLayout, nullptr);
			vkDestroyImageView(m_Device, m_CloudMap.imageView, nullptr);
			vmaDestroyImage(m_Allocator, m_CloudMap.image, m_CloudMap.allocation);
			destroyBuffer(m_CloudMapInfo);
		});
	}

	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // depth
	builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);		   // the clouds, half resolution
	builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // noise lookup table
	builder.add_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // coverage map
	builder.add_binding(4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);		   // where the map is
	builder.add_binding(5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // the heightmap (the far fog's ground)
	m_CloudLayout = builder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
	m_CloudSet = g_DescriptorAllocator.allocate(m_Device, m_CloudLayout);
	VkPushConstantRange range{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(glm::mat4) + sizeof(glm::vec4) * 4 };
	VkDescriptorSetLayout layouts[] = { m_CloudLayout, m_AtmosphereManager.getRenderDescriptorLayout() };
	VkPipelineLayoutCreateInfo layoutCI = vkinit::pipeline_layout_create_info();
	layoutCI.setLayoutCount = 2;
	layoutCI.pSetLayouts = layouts;
	layoutCI.pushConstantRangeCount = 1;
	layoutCI.pPushConstantRanges = &range;
	VkPipelineLayout layout;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &layoutCI, nullptr, &layout));
	m_CloudEffect = loadComputeShader("src/shaders/bin/clouds.comp.spv", "volumetric clouds", layout);

	DescriptorLayoutBuilder resolveBuilder;
	resolveBuilder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // this frame's march
	resolveBuilder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // last frame's result
	resolveBuilder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // depth
	resolveBuilder.add_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);		  // the result (m_CloudImage)
	resolveBuilder.add_binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // last frame's depth per texel
	resolveBuilder.add_binding(5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);		  // this frame's (m_CloudDepth)
	m_CloudResolveLayout = resolveBuilder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
	m_CloudResolveSet = g_DescriptorAllocator.allocate(m_Device, m_CloudResolveLayout);
	VkPushConstantRange resolveRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(glm::mat4) + sizeof(glm::vec4) };
	VkPipelineLayoutCreateInfo resolveLayoutCI = vkinit::pipeline_layout_create_info();
	resolveLayoutCI.setLayoutCount = 1;
	resolveLayoutCI.pSetLayouts = &m_CloudResolveLayout;
	resolveLayoutCI.pushConstantRangeCount = 1;
	resolveLayoutCI.pPushConstantRanges = &resolveRange;
	VkPipelineLayout resolveLayout;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &resolveLayoutCI, nullptr, &resolveLayout));
	m_CloudResolveEffect = loadComputeShader("src/shaders/bin/cloudresolve.comp.spv", "cloud accumulation", resolveLayout);

	m_DeletionQueue.push([&]() {
		vkDestroyPipelineLayout(m_Device, m_CloudResolveEffect.layout, nullptr);
		vkDestroyDescriptorSetLayout(m_Device, m_CloudResolveLayout, nullptr);
		vkDestroyPipelineLayout(m_Device, m_CloudEffect.layout, nullptr);
		vkDestroyDescriptorSetLayout(m_Device, m_CloudLayout, nullptr);
		vkDestroySampler(m_Device, m_CloudNoiseSampler, nullptr);
		vkDestroyImageView(m_Device, m_CloudNoise.imageView, nullptr);
		vmaDestroyImage(m_Allocator, m_CloudNoise.image, m_CloudNoise.allocation);
	});
}

void Application::writeLightingDescriptors() {
	if(m_LightingSet == VK_NULL_HANDLE) return; // the first swapchain exists before the pipelines
	const VkImageLayout read = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	VkDescriptorImageInfo infos[9] = {
		{ m_NearestSampler, m_GAlbedo.imageView, read },
		{ m_NearestSampler, m_GNormal.imageView, read },
		{ m_NearestSampler, m_GMaterial.imageView, read },
		{ m_NearestSampler, m_DepthBuffer.imageView, read },
		{ VK_NULL_HANDLE, m_DrawImage.imageView, VK_IMAGE_LAYOUT_GENERAL },
		{ VK_NULL_HANDLE, m_ShadowFilter.getDirectView(), VK_IMAGE_LAYOUT_GENERAL },
		{ m_HeightmapSampler, m_SkyVisibility.imageView, read },
		{ m_AtmosphereSampler, m_SSAOImage.imageView, read }, // linear, clamped
		{ m_AtmosphereSampler, m_CloudImage.imageView, read },
	};
	VkWriteDescriptorSet writes[9] = {};
	for(uint32_t i = 0; i < 9; i++) {
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = m_LightingSet;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = (i == 4 || i == 5) ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		writes[i].pImageInfo = &infos[i];
	}
	vkUpdateDescriptorSets(m_Device, 9, writes, 0, nullptr);

	// the coverage map in the lighting (9, 10) and the clouds (3, 4)
	VkDescriptorImageInfo mapInfo{ m_HeightmapSampler, m_CloudMap.imageView, VK_IMAGE_LAYOUT_GENERAL }; // linear, repeating: it wraps
	VkDescriptorBufferInfo mapWhere{ m_CloudMapInfo.buffer, 0, sizeof(glm::vec4) };
	auto writeMap = [&](VkDescriptorSet set, uint32_t mapBinding) {
		if(set == VK_NULL_HANDLE) return;
		VkWriteDescriptorSet mapWrites[2] = {};
		for(auto& w : mapWrites) { w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w.dstSet = set; w.descriptorCount = 1; }
		mapWrites[0].dstBinding = mapBinding; mapWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; mapWrites[0].pImageInfo = &mapInfo;
		mapWrites[1].dstBinding = mapBinding + 1; mapWrites[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; mapWrites[1].pBufferInfo = &mapWhere;
		vkUpdateDescriptorSets(m_Device, 2, mapWrites, 0, nullptr);
	};
	writeMap(m_LightingSet, 9);

	// the climate map (11): the weather on the ground follows the local climate. and the heightmap (12), the uv math
	// goes by its size (the map offset is in its texels)
	VkDescriptorImageInfo climateInfo{ m_HeightmapSampler, m_ClimateMap.imageView, read };
	VkDescriptorImageInfo heightmapInfo{ m_HeightmapSampler, m_Heightmap.imageView, read };
	VkWriteDescriptorSet climateWrite{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
	climateWrite.dstSet = m_LightingSet; climateWrite.dstBinding = 11; climateWrite.descriptorCount = 1;
	climateWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; climateWrite.pImageInfo = &climateInfo;
	vkUpdateDescriptorSets(m_Device, 1, &climateWrite, 0, nullptr);
	climateWrite.dstBinding = 12; climateWrite.pImageInfo = &heightmapInfo;
	vkUpdateDescriptorSets(m_Device, 1, &climateWrite, 0, nullptr);

	writeMap(m_CloudSet, 3);
	if(m_CloudSet != VK_NULL_HANDLE) {
		climateWrite.dstSet = m_CloudSet; climateWrite.dstBinding = 5; climateWrite.pImageInfo = &heightmapInfo;
		vkUpdateDescriptorSets(m_Device, 1, &climateWrite, 0, nullptr);
	}

	if(m_CloudSet != VK_NULL_HANDLE) {
		VkDescriptorImageInfo cloudInfos[3] = {
			{ m_NearestSampler, m_DepthBuffer.imageView, read },
			{ VK_NULL_HANDLE, m_CloudRaw.imageView, VK_IMAGE_LAYOUT_GENERAL },
			{ m_CloudNoiseSampler, m_CloudNoise.imageView, read },
		};
		VkWriteDescriptorSet cloudWrites[3] = {};
		for(uint32_t i = 0; i < 3; i++) {
			cloudWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			cloudWrites[i].dstSet = m_CloudSet;
			cloudWrites[i].dstBinding = i;
			cloudWrites[i].descriptorCount = 1;
			cloudWrites[i].descriptorType = i == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			cloudWrites[i].pImageInfo = &cloudInfos[i];
		}
		vkUpdateDescriptorSets(m_Device, 3, cloudWrites, 0, nullptr);
	}
	if(m_CloudResolveSet != VK_NULL_HANDLE) {
		VkDescriptorImageInfo resolveInfos[6] = {
			{ m_NearestSampler, m_CloudRaw.imageView, read },
			{ m_AtmosphereSampler, m_CloudHistory.imageView, read }, // linear, clamped
			{ m_NearestSampler, m_DepthBuffer.imageView, read },
			{ VK_NULL_HANDLE, m_CloudImage.imageView, VK_IMAGE_LAYOUT_GENERAL },
			{ m_NearestSampler, m_CloudDepthHistory.imageView, read },
			{ VK_NULL_HANDLE, m_CloudDepth.imageView, VK_IMAGE_LAYOUT_GENERAL },
		};
		VkWriteDescriptorSet resolveWrites[6] = {};
		for(uint32_t i = 0; i < 6; i++) {
			resolveWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			resolveWrites[i].dstSet = m_CloudResolveSet;
			resolveWrites[i].dstBinding = i;
			resolveWrites[i].descriptorCount = 1;
			resolveWrites[i].descriptorType = i == 3 || i == 5 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			resolveWrites[i].pImageInfo = &resolveInfos[i];
		}
		vkUpdateDescriptorSets(m_Device, 6, resolveWrites, 0, nullptr);
	}

	VkDescriptorImageInfo ssaoInfos[3] = {
		{ m_NearestSampler, m_DepthBuffer.imageView, read },
		{ m_NearestSampler, m_GNormal.imageView, read },
		{ VK_NULL_HANDLE, m_SSAOImage.imageView, VK_IMAGE_LAYOUT_GENERAL },
	};
	VkWriteDescriptorSet ssaoWrites[3] = {};
	for(uint32_t i = 0; i < 3; i++) {
		ssaoWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		ssaoWrites[i].dstSet = m_SSAOSet;
		ssaoWrites[i].dstBinding = i;
		ssaoWrites[i].descriptorCount = 1;
		ssaoWrites[i].descriptorType = i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		ssaoWrites[i].pImageInfo = &ssaoInfos[i];
	}
	vkUpdateDescriptorSets(m_Device, 3, ssaoWrites, 0, nullptr);
}

void Application::drawLighting(VkCommandBuffer cmd) {
	// the volumetric fog's froxels first, alone in their GPU timer (lit through this frame's shadow maps, needs
	// nothing from the G-buffer). the lighting composites them
	drawVolumetricFog(cmd); // (marks its two passes)

	VkImageSubresourceRange depthRange = vkinit::image_subresource_range(VK_IMAGE_ASPECT_DEPTH_BIT);
	for(VkImage target : { m_GAlbedo.image, m_GNormal.image, m_GMaterial.image, m_GVelocity.image })
		vkutil::transition_image(cmd, target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	vkutil::transition_image(cmd, m_DepthBuffer.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, depthRange);
	// the depth pyramid for next frame's occlusion culling, with the camera this depth was drawn with
	m_HiZ.build(cmd);
	m_HiZViewProj = m_Projection * m_View;
	m_HiZMapOffset = m_MapOffset;
	m_HiZValid = true;
	vkutil::transition_image(cmd, m_DrawImage.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
	vkutil::transition_image(cmd, m_ShadowFilter.getDirectImage(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);

	// the cloud shape lookup table (a torus around the camera): once the camera (or the wind) has moved a km, the
	// strips it moved into are redone, everything on a jump. the SSAO's shafts, the clouds, the fog and the lighting's
	// cloud shadows read it. the coverage is applied when it's read, the weather changing it costs nothing here
	{
		const AtmosphereUBO& atmosphere = m_AtmosphereManager.m_AtmosphereData;
		glm::vec2 camWorldKm = (glm::vec2(m_CameraPos.x, m_CameraPos.z) + glm::vec2(m_MapOffset) * (m_WorldSize * m_TerrainScale / static_cast<float>(m_HeightmapSize))) * atmosphere.worldScale
							 + m_AtmosphereManager.getCloudWind();
		const int size = static_cast<int>(m_CloudMap.imageExtent.width);
		const float texel = CLOUD_MAP_EXTENT / static_cast<float>(size);
		glm::ivec2 origin = glm::ivec2(glm::floor(camWorldKm / texel)) - size / 2;
		glm::ivec2 delta = origin - m_CloudMapOrigin;
		const int moveTexels = static_cast<int>(CLOUD_MAP_MOVE / texel);
		std::vector<CloudMapPushConstants> regions;
		if(atmosphere.clouds > 0.5f) {
			if(!m_CloudMapValid || std::abs(delta.x) >= size || std::abs(delta.y) >= size) {
				regions.push_back({ origin, glm::ivec2(size), texel });
			} else if(std::abs(delta.x) >= moveTexels || std::abs(delta.y) >= moveTexels) {
				if(delta.x > 0) regions.push_back({ glm::ivec2(m_CloudMapOrigin.x + size, origin.y), glm::ivec2(delta.x, size), texel });
				if(delta.x < 0) regions.push_back({ origin, glm::ivec2(-delta.x, size), texel });
				if(delta.y > 0) regions.push_back({ glm::ivec2(origin.x, m_CloudMapOrigin.y + size), glm::ivec2(size, delta.y), texel });
				if(delta.y < 0) regions.push_back({ origin, glm::ivec2(size, -delta.y), texel });
			}
		}
		if(!regions.empty()) {
			m_FrameEvents |= FRAME_CLOUD_MAP;
			m_CloudMapOrigin = origin;
			m_CloudMapValid = true;
			VkMemoryBarrier2 barrier{ .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
			VkDependencyInfo dependency{ .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
			dependency.memoryBarrierCount = 1;
			dependency.pMemoryBarriers = &barrier;
			// last frame's passes may still read the map / its info
			barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
			barrier.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT;
			barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			barrier.dstAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
			vkCmdPipelineBarrier2(cmd, &dependency);
			VkDescriptorSet mapSets[] = { m_CloudMapSet, m_AtmosphereManager.getRenderDescriptor() };
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_CloudMapEffect.pipeline);
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_CloudMapEffect.layout, 0, 2, mapSets, 0, nullptr);
			for(const CloudMapPushConstants& region : regions) {
				vkCmdPushConstants(cmd, m_CloudMapEffect.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(region), &region);
				vkCmdDispatch(cmd, (region.size.x + 7) / 8, (region.size.y + 7) / 8, 1);
			}
			glm::vec2 centre = (glm::vec2(origin) + static_cast<float>(size) * 0.5f) * texel;
			glm::vec4 where(centre, CLOUD_MAP_EXTENT, 1.0f);
			vkCmdUpdateBuffer(cmd, m_CloudMapInfo.buffer, 0, sizeof(where), &where);
			barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT;
			barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
			barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
			barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_UNIFORM_READ_BIT;
			vkCmdPipelineBarrier2(cmd, &dependency);
		}

		mark(cmd, "cloud map");

		// the cloud shadow, baked into g for the window around the camera: every frame (the sun moves, the weather
		// eases in), one dispatch instead of 4 density samples in every lit pixel, froxel and far fog step
		// (for whichever of the sun / moon is up: the moon behind a cloud has to darken the land too, or its glints
		// on the grass shimmered under a dark sky)
		glm::vec3 cloudLightDir = m_SunDir.y > 0.0f ? m_SunDir : -m_SunDir;
		if(m_CloudMapValid && atmosphere.clouds > 0.5f && atmosphere.cloudShadows > 0.5f && cloudLightDir.y > 0.03f) {
			VkMemoryBarrier2 barrier{ .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
			VkDependencyInfo dependency{ .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
			dependency.memoryBarrierCount = 1;
			dependency.pMemoryBarriers = &barrier;
			barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; // last frame's readers, the map / info just written
			barrier.srcAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT;
			barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
			barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_UNIFORM_READ_BIT;
			vkCmdPipelineBarrier2(cmd, &dependency);
			CloudShadowPushConstants shadow{};
			shadow.size = glm::ivec2(CLOUD_SHADOW_WINDOW);
			shadow.start = glm::ivec2(glm::floor(camWorldKm / texel)) - CLOUD_SHADOW_WINDOW / 2;
			shadow.sun = glm::vec4(cloudLightDir, 0.0f);
			shadow.texel = texel;
			VkDescriptorSet shadowSets[] = { m_CloudShadowSet, m_AtmosphereManager.getRenderDescriptor() };
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_CloudShadowEffect.pipeline);
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_CloudShadowEffect.layout, 0, 2, shadowSets, 0, nullptr);
			vkCmdPushConstants(cmd, m_CloudShadowEffect.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(shadow), &shadow);
			vkCmdDispatch(cmd, (CLOUD_SHADOW_WINDOW + 7) / 8, (CLOUD_SHADOW_WINDOW + 7) / 8, 1);
			barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
			barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
			barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
			barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
			vkCmdPipelineBarrier2(cmd, &dependency);
		}
	}

	mark(cmd, "cloud shadow bake");

	// SSAO first (half resolution), the lighting reads it. it runs with SSAO off too: its g channel is how much of
	// the air in front of each pixel the sun reaches, for the haze (ssao.comp)
	vkutil::transition_image(cmd, m_SSAOImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
	{
		struct { glm::mat4 invViewProj; glm::vec3 campos; float radius; float projScale, frame, intensity, pad; glm::vec4 lightDir; } ssao{};
		ssao.invViewProj = glm::inverse(m_Projection * m_View);
		ssao.campos = m_CameraPos;
		ssao.radius = 0.25f; // world units, ~1.5m
		ssao.projScale = m_TerrainPC.projScale;
		ssao.frame = static_cast<float>(m_FrameNumber % 64);
		ssao.intensity = m_SSAO ? 1.0f : 0.0f;
		glm::vec3 shadowDir = m_SunDir.y > 0.0f ? m_SunDir : -m_SunDir; // the shadow maps follow whichever is up
		ssao.lightDir = glm::vec4(shadowDir, m_AtmosphereManager.m_AerialPerspective ? 1.0f : 0.0f);
		VkDescriptorSet sets[] = { m_SSAOSet, m_Shadows.getDescriptor(m_FrameNumber) };
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_SSAOEffect.pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_SSAOEffect.layout, 0, 2, sets, 0, nullptr);
		vkCmdPushConstants(cmd, m_SSAOEffect.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ssao), &ssao);
		vkCmdDispatch(cmd, (m_SSAOImage.imageExtent.width + 7) / 8, (m_SSAOImage.imageExtent.height + 7) / 8, 1);
	}
	vkutil::transition_image(cmd, m_SSAOImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	mark(cmd, "SSAO");

	// the volumetric cumulus, half resolution (lighting.comp composites them): the march, then its accumulation
	// over frames into m_CloudImage
	vkutil::transition_image(cmd, m_CloudImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
	const AtmosphereUBO& atmosphere = m_AtmosphereManager.m_AtmosphereData;
	// (also for the far fog alone: it's marched in the same pass, lighting.comp composites it with the clouds)
	bool marchClouds = atmosphere.clouds > 0.5f || atmosphere.volumetricFog > 0.5f;
	if(!marchClouds) m_CloudHistoryValid = false;
	if(marchClouds) {
		vkutil::transition_image(cmd, m_CloudRaw.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
		struct {
			glm::mat4 invViewProj; glm::vec3 campos; float frame; glm::vec3 sundir; float terrainSize; glm::vec2 cloudWind, mapOffset;
			glm::vec4 fog; // the far fog: density, valley mist, the weather's haze, terrain height scale
		} clouds;
		// unjittered: the TAA's sub pixel jitter moved every half resolution texel's ray by up to a texel and a half
		// against the history, the cloud edges shook with it (the clouds are soft, they don't need the TAA's antialiasing)
		clouds.invViewProj = glm::inverse(m_UnjitteredProjection * m_View);
		clouds.campos = m_CameraPos;
		clouds.frame = static_cast<float>(m_FrameNumber % 1024); // the golden ratio sequence (clouds.comp): not reset every 64
		clouds.sundir = m_SunDir;
		clouds.terrainSize = m_WorldSize * m_TerrainScale;
		clouds.cloudWind = m_AtmosphereManager.getCloudWind();
		clouds.mapOffset = glm::vec2(m_MapOffset) * (4096.0f / static_cast<float>(m_HeightmapSize)); // texels of the 4096 map
		clouds.fog = glm::vec4(m_FogDensity, m_FogMist * m_Weather.state().mist, m_Weather.state().haze, m_TerrainPC.factor);
		VkDescriptorSet cloudSets[] = { m_CloudSet, m_AtmosphereManager.getRenderDescriptor() };
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_CloudEffect.pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_CloudEffect.layout, 0, 2, cloudSets, 0, nullptr);
		vkCmdPushConstants(cmd, m_CloudEffect.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(clouds), &clouds);
		vkCmdDispatch(cmd, (m_CloudImage.imageExtent.width + 7) / 8, (m_CloudImage.imageExtent.height + 7) / 8, 1);
		mark(cmd, "cloud march"); // (and the far fog, marched along)

		// the accumulation: ~10 frames of marches (each starts at a different random offset) average the noise out
		vkutil::transition_image(cmd, m_CloudRaw.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		if(!m_CloudHistoryValid) {
			vkutil::transition_image(cmd, m_CloudHistory.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
			vkutil::transition_image(cmd, m_CloudDepthHistory.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}
		vkutil::transition_image(cmd, m_CloudDepth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
		struct { glm::mat4 reproject; float valid, blend; glm::vec2 pad; } resolve{};
		resolve.reproject = m_PreviousViewProj * glm::inverse(m_UnjitteredProjection * m_View); // as clouds.comp marched (unjittered)
		// a lightning flash (and the frame after it): no history, or the strike's light lingered and smeared
		float flash = m_Weather.state().strikeFlash;
		bool flashing = flash > 0.002f || m_CloudFlashLastFrame > 0.002f;
		m_CloudFlashLastFrame = flash;
		resolve.valid = m_CloudHistoryValid && !flashing ? 1.0f : 0.0f;
		resolve.blend = 0.08f;
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_CloudResolveEffect.pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_CloudResolveEffect.layout, 0, 1, &m_CloudResolveSet, 0, nullptr);
		vkCmdPushConstants(cmd, m_CloudResolveEffect.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(resolve), &resolve);
		vkCmdDispatch(cmd, (m_CloudImage.imageExtent.width + 7) / 8, (m_CloudImage.imageExtent.height + 7) / 8, 1);

		// kept for the next frame
		vkutil::transition_image(cmd, m_CloudImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		vkutil::transition_image(cmd, m_CloudHistory.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		VkImageCopy copy{};
		copy.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		copy.dstSubresource = copy.srcSubresource;
		copy.extent = m_CloudImage.imageExtent;
		vkCmdCopyImage(cmd, m_CloudImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_CloudHistory.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		vkutil::transition_image(cmd, m_CloudHistory.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		vkutil::transition_image(cmd, m_CloudImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
		vkutil::transition_image(cmd, m_CloudDepth.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		vkutil::transition_image(cmd, m_CloudDepthHistory.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		vkCmdCopyImage(cmd, m_CloudDepth.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_CloudDepthHistory.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
		vkutil::transition_image(cmd, m_CloudDepthHistory.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		m_CloudHistoryValid = true;
	}
	vkutil::transition_image(cmd, m_CloudImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	mark(cmd, "cloud resolve");

	LightingPushConstants pc{};
	pc.invViewProj = glm::inverse(m_Projection * m_View);
	pc.viewProj = m_Projection * m_View;
	pc.frame = static_cast<float>(m_FrameNumber % 64);
	pc.effects = glm::vec4(m_ContactShadows ? 1.0f : 0.0f, 0.0f, m_SSAO ? 1.0f : 0.0f, m_BounceLight ? m_BounceStrength : 0.0f);
	pc.campos = m_CameraPos;
	pc.terrainSize = m_WorldSize * m_TerrainScale;
	pc.sundir = m_SunDir;
	pc.mapOffset = m_MapOffset;
	pc.cloudWind = m_AtmosphereManager.getCloudWind();
	pc.extra = glm::vec4(0.0f, 0.0f, m_DenoiseEffects ? 1.0f : 0.0f, 0.0f);
	VkDescriptorSet sets[] = { m_LightingSet, m_AtmosphereManager.getRenderDescriptor(), m_Shadows.getDescriptor(m_FrameNumber) };
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_LightingEffect.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_LightingEffect.layout, 0, 3, sets, 0, nullptr);
	vkCmdPushConstants(cmd, m_LightingEffect.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	vkCmdDispatch(cmd, (m_DrawExtent.width + 7) / 8, (m_DrawExtent.height + 7) / 8, 1);

	// back to how the shadow filter expects them
	vkutil::transition_image(cmd, m_DrawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	vkutil::transition_image(cmd, m_ShadowFilter.getDirectImage(), VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	vkutil::transition_image(cmd, m_DepthBuffer.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, depthRange);
}

void Application::run() {
	bool quit = m_QuitRequested; // closed during loading
	
	SDL_Event event;
	Uint64 frequency = SDL_GetPerformanceFrequency();
	Uint64 lastTime = SDL_GetPerformanceCounter();
	
	while(!quit) {
		Uint64 now = SDL_GetPerformanceCounter();
		float deltatime = static_cast<float>(now - lastTime) / static_cast<float>(frequency);
		lastTime = now;
		float deltatimeMs = deltatime * 1000.0f;
		// (pair captures freeze the wind and the weather too: the clouds drifting between the two frames read as flicker)
		float worldDeltatime = m_ShotPair && !m_ShotPairAnimated ? 0.0f : deltatime;
		m_AtmosphereManager.tick(worldDeltatime);
		updateWeather(worldDeltatime);
		if(!m_ShotPair || m_ShotPairAnimated) m_Time += deltatime; // frozen in the pair captures: only flicker changes between the frames
		m_Deltatime = deltatime;
		// VULKAN2_BENCH=1 prints the GPU timers every 2 seconds (for profiling without the UI)
		static const bool bench = std::getenv("VULKAN2_BENCH") != nullptr;
		static float benchTimer = 0.0f;
		// VULKAN2_BENCH=2 also cycles through turning trees, foliage and shadows off, 6 seconds each
		static const bool benchToggles = bench && std::string(std::getenv("VULKAN2_BENCH")) == "2";
		static int benchPhase = -1;
		if(benchToggles && static_cast<int>(m_Time / 6.0f) % 4 != benchPhase) {
			benchPhase = static_cast<int>(m_Time / 6.0f) % 4;
			m_Trees.m_Enabled = benchPhase != 1;
			m_Foliage.m_Enabled = benchPhase != 2;
			m_Shadows.m_Enabled = benchPhase != 3;
			const char* names[] = { "everything", "no trees", "no foliage", "no shadows" };
			fmt::print("--- {}\n", names[benchPhase]);
		}
		if(bench && (benchTimer += deltatime) > 2.0f) {
			benchTimer = 0.0f;
			fmt::print("frame {:.2f} ms |", deltatimeMs);
			for(const GpuTimer& t : m_GpuTimers) fmt::print(" {} {:.2f} |", t.name, t.ms);
			fmt::print(" shadow re-render {:.2f} ms {:.1f}/s\n", m_ShadowRenderMs, m_ShadowRendersPerSecond);
			std::fflush(stdout);
		}
		// stutters: a frame much slower than the ones around it (the median of the last second), with what happened in
		// the last few frames (a slow GPU frame makes the CPU wait for it a frame or two later)
		{
			m_FrameEventRing[m_FrameNumber % 4] = m_FrameEvents;
			m_FrameEvents = 0;
			float recent[60];
			int count = 0;
			for(int i = 1; i <= 60; i++) {
				float t = m_Frametimes[(m_FrameOffset - i + m_FrameHistorySize) % m_FrameHistorySize];
				if(t > 0.0f) recent[count++] = t;
			}
			if(count >= 30) {
				std::nth_element(recent, recent + count / 2, recent + count);
				float typical = recent[count / 2];
				if(deltatimeMs > std::max(typical * 1.6f, typical + 5.0f)) {
					uint32_t events = m_FrameEventRing[0] | m_FrameEventRing[1] | m_FrameEventRing[2] | m_FrameEventRing[3];
					m_Stutters.push_back({ m_Time, deltatimeMs, typical, events });
					if(m_Stutters.size() > 16) m_Stutters.erase(m_Stutters.begin());
					static const bool logStutters = std::getenv("VULKAN2_STUTTERLOG") != nullptr;
					if(logStutters) fmt::print("stutter {:.1f} ms (typical {:.1f}) shotframe {} events {}\n", deltatimeMs, typical, m_ShotFrames, events);
				}
			}
		}
		if(!m_PauseFrametimes) {
			m_Frametimes[m_FrameOffset] = deltatimeMs/* > 0 ? 1000.0f / deltatimeMs : 0.0f*/;
			m_FrameOffset = (m_FrameOffset + 1) % m_FrameHistorySize;
		}
		
		while(SDL_PollEvent(&event)) {
			if(event.type == SDL_EVENT_QUIT) quit = true;
			if(event.type == SDL_EVENT_WINDOW_RESIZED) m_Resized = true;
			if(event.type == SDL_EVENT_KEY_UP) {
				if(event.key.key == SDLK_ESCAPE) m_Mouselock = !m_Mouselock;
				if(event.key.key == SDLK_F) m_Camera.walking = !m_Camera.walking;
			}
			if(m_Mouselock) m_Camera.processSDLEvent(event);
			if(!m_Mouselock) ImGui_ImplSDL3_ProcessEvent(&event);
			SDL_SetWindowRelativeMouseMode(m_pWindow, m_Mouselock);
		}
		m_Camera.metersPerUnit = m_AtmosphereManager.m_AtmosphereData.worldScale * 1000.0f;
		m_Camera.update(deltatime);
		if(!m_Shots.empty() && !updateShots()) quit = true;
		if(!m_Mouselock) {
			// imgui new frame
			ImGui_ImplVulkan_NewFrame();
			ImGui_ImplSDL3_NewFrame();

			//some imgui UI
			ImGui::NewFrame();
			
			drawPerformanceWindow(deltatimeMs);
			
			if (ImGui::Begin("settings")) {
				drawSettings();
				ImGui::Separator();
				ImGui::Checkbox("Walking (F)", &m_Camera.walking);
				ImGui::Checkbox("debug textures", &m_DebugView.m_Open);
				ImGui::Checkbox("GPU draw counts", &m_ShowDrawStats);
			}
			ImGui::End();
			m_DebugView.drawImGui(debugTextures());
			if(m_ShowDrawStats) drawDrawStatsWindow();

			if (ImGui::Begin("atmosphere")) {
				m_AtmosphereManager.drawImGui();
				m_Weather.drawImGui();
			}
			ImGui::End();
			// (the heightmap is in the debug textures window)

			ImGui::Render();
		}
		updateHeightmap();
		if(SDL_GetWindowFlags(m_pWindow) & SDL_WINDOW_MINIMIZED) {
			SDL_Delay(16); // nothing to draw to
			continue;
		}
		Uint64 drawStart = SDL_GetPerformanceCounter();
		draw();
		float drawMs = static_cast<float>(SDL_GetPerformanceCounter() - drawStart) * 1000.0f / static_cast<float>(frequency);
		m_CpuFrameMs += (drawMs - m_CpuFrameMs) * 0.1f;
	}
}

void Application::draw() {
	// VULKAN2_DEBUGTEX_CYCLE=1: the debug texture viewer open on a different source every frame and the draw counts on,
	// so a capture run samples every source once (validation catches a wrong layout / view type)
	static const bool debugCycle = std::getenv("VULKAN2_DEBUGTEX_CYCLE") != nullptr;
	if(debugCycle) {
		m_DebugView.m_Open = true;
		m_DebugView.select(static_cast<int>(m_FrameNumber % debugTextures().size()));
		m_ShowDrawStats = true;
	}
	// VULKAN2_DRAWSTATS=1: the draw counts on (printed with each capture)
	static const bool drawStatsEnv = std::getenv("VULKAN2_DRAWSTATS") != nullptr;
	if(drawStatsEnv) m_ShowDrawStats = true;
	if(m_Resized) { resizeSwapchain(m_Vsync); m_FrameEvents |= FRAME_RESIZE; }
	
	// Wait for the GPU to finish, reset the fence
	VK_CHECK(vkWaitForFences(m_Device, 1, &getCurrentFrame().renderFence, true, 1000000000));
	
	getCurrentFrame().deletionQueue.flush();

	readDrawStats(); // this slot's copies from FRAME_OVERLAP frames ago have finished too

	// this slot's probe from FRAME_OVERLAP frames ago has finished, feed it to the walking camera
	uint32_t probeSlot = m_FrameNumber % FRAME_OVERLAP;
	if(m_ProbeWritten[probeSlot]) {
		vmaInvalidateAllocation(m_Allocator, m_ProbeBuffer.allocation, 0, VK_WHOLE_SIZE);
		m_Collision.ingest(static_cast<ProbeData*>(m_ProbeBuffer.info.pMappedData)[probeSlot]);
		// flying (and the captures) still want the ground under the camera
		if(!m_Camera.walking && m_Collision.valid()) m_Camera.groundHeight = m_Collision.groundAt(glm::vec2(m_Camera.position.x, m_Camera.position.z));
		// the flying camera stays above the terrain
		if(m_Collision.valid()) m_Camera.floorHeight = m_Collision.groundAt(glm::vec2(m_Camera.position.x, m_Camera.position.z));
		m_LastProbe = glm::vec4(m_Camera.groundHeight, 0.0f, 0.0f, 0.0f);
	}

	// Get a swapchain index for the next frame
	uint32_t swapchainImageIndex;
	// If the swapchain is full, it'll wait up to a second
	VkResult e = vkAcquireNextImageKHR(m_Device, m_Swapchain, 1000000000, getCurrentFrame().swapchainSemaphore, nullptr, &swapchainImageIndex);
	if(e == VK_ERROR_OUT_OF_DATE_KHR) {
		m_Resized = true;
		return; // the fence is still signaled so the next frame doesn't wait forever
	}
	
	VK_CHECK(vkResetFences(m_Device, 1, &getCurrentFrame().renderFence));
	
	VkCommandBuffer cmd = getCurrentFrame().commandBuffer;
	VK_CHECK(vkResetCommandBuffer(cmd, 0));
	
	VkCommandBufferBeginInfo cmdBeginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
	// the flag one time submit basically tells the driver that we're only submitting and executing the command once
	m_DrawExtent.width = m_DrawImage.imageExtent.width;
	m_DrawExtent.height = m_DrawImage.imageExtent.height;
	
	VK_CHECK(vkBeginCommandBuffer(cmd, &cmdBeginInfo));	// command buffer is now recording!

	// GPU timings of this slot's last frame (its fence was waited on), then reset them for this one
	{
		uint32_t slot = m_FrameNumber % FRAME_OVERLAP;
		std::vector<const char*>& names = m_MarkNames[slot];
		if(m_TimestampsWritten[slot] && !names.empty()) {
			uint64_t ticks[GPU_QUERIES];
			uint32_t count = static_cast<uint32_t>(names.size()) + 1;
			if(vkGetQueryPoolResults(m_Device, m_TimestampPool, slot * GPU_QUERIES, count, sizeof(uint64_t) * count, ticks, sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
				size_t insertAt = 0; // a feature seen for the first time goes after the one before it this frame
				for(uint32_t i = 0; i + 1 < count; i++) {
					float ms = static_cast<float>(ticks[i + 1] - ticks[i]) * m_TimestampPeriod * 1e-6f;
					auto it = std::find_if(m_GpuTimers.begin(), m_GpuTimers.end(), [&](const GpuTimer& t) { return std::strcmp(t.name, names[i]) == 0; });
					if(it == m_GpuTimers.end()) it = m_GpuTimers.insert(m_GpuTimers.begin() + std::min(insertAt, m_GpuTimers.size()), GpuTimer{ names[i], ms, 0 });
					it->ms += (ms - it->ms) * 0.1f;
					it->lastSeen = m_FrameNumber;
					insertAt = static_cast<size_t>(it - m_GpuTimers.begin()) + 1;
					if(std::strcmp(names[i], "shadow maps") == 0 && m_ShadowCascadesRendered[slot] > 0) m_ShadowRenderMs += (ms - m_ShadowRenderMs) * 0.3f;
				}
				m_ShadowRendersPerSecond += ((m_ShadowCascadesRendered[slot] > 0 ? 1.0f : 0.0f) / std::max(m_Deltatime, 1e-4f) - m_ShadowRendersPerSecond) * 0.02f;
			}
			// a feature that stopped running (turned off) drops out after ~2 seconds
			int frame = m_FrameNumber;
			m_GpuTimers.erase(std::remove_if(m_GpuTimers.begin(), m_GpuTimers.end(), [&](const GpuTimer& t) { return frame - t.lastSeen > 240; }), m_GpuTimers.end());
		}
		names.clear();
		vkCmdResetQueryPool(cmd, m_TimestampPool, slot * GPU_QUERIES, GPU_QUERIES);
		m_TimestampsWritten[slot] = true;
	}
	markStart(cmd);

	// per frame atmosphere LUTs (sky view + aerial perspective)
	updateCamera();
	m_AtmosphereManager.update(cmd, m_View, m_Projection, m_CameraPos, glm::vec2(m_CameraPos.x, m_CameraPos.z) + glm::vec2(m_MapOffset) * (m_TerrainScale / static_cast<float>(m_CoordinateMultiplier)));
	mark(cmd, "atmosphere LUTs");

	// the heightmap strips the camera moved into (updateHeightmap queued them), before anything reads the map
	recordHeightmapUpdate(cmd);
	mark(cmd, "terrain streaming");

	// terrain height under the camera, read back once this frame's fence is waited on again
	{
		ProbePushConstants probe{};
		probe.position = glm::vec2(m_PlayerPos.x, m_PlayerPos.z);
		probe.offset = m_MapOffset;
		probe.terrainSize = m_WorldSize * m_TerrainScale;
		probe.factor = m_TerrainPC.factor;
		probe.slot = probeSlot;
		probe.density = m_Trees.m_Density;
		probe.offsetWorld = glm::vec2(m_MapOffset) * (m_TerrainScale / static_cast<float>(m_CoordinateMultiplier));
		probe.spacing = 0.25f / m_Camera.metersPerUnit; // the 32x32 patch covers ~8m
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ProbeEffect.pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ProbeEffect.layout, 0, 1, &m_ProbeSet, 0, nullptr);
		vkCmdPushConstants(cmd, m_ProbeEffect.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ProbePushConstants), &probe);
		vkCmdDispatch(cmd, 1, 1, 1);

		VkMemoryBarrier2 toHost{ .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
		toHost.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
		toHost.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
		toHost.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
		toHost.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
		VkDependencyInfo dependency{ .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
		dependency.memoryBarrierCount = 1;
		dependency.pMemoryBarriers = &toHost;
		vkCmdPipelineBarrier2(cmd, &dependency);
		m_ProbeWritten[probeSlot] = true;
	}
	mark(cmd, "ground probe");

	// grass, wheat, flowers, trees and bushes around the camera (before the shadow pass, trees cast shadows)
	FoliagePushConstants foliagePC{};
	foliagePC.viewproj = m_Projection * m_View;
	foliagePC.campos = m_CameraPos;
	foliagePC.time = m_Time;
	foliagePC.sundir = glm::vec3(m_PreviousTime, 0.0f, 0.0f); // the wind last frame, for the motion vectors
	m_PreviousTime = m_Time;
	foliagePC.factor = m_TerrainPC.factor;
	foliagePC.offsetWorld = glm::vec2(m_MapOffset) * (m_TerrainScale / static_cast<float>(m_CoordinateMultiplier));
	foliagePC.offset = m_MapOffset;
	foliagePC.terrainSize = m_WorldSize * m_TerrainScale;
	foliagePC.projScale = m_TerrainPC.projScale;
	foliagePC.lodPixels = 40.0f / std::max(m_TerrainPC.tessellationFactor, 0.1f);
	foliagePC.extra = m_GrassInteraction && m_Camera.walking && m_Camera.onGround ? 1.7f / m_Camera.metersPerUnit : 0.0f; // eye height, grass bends away from your feet
	{
		// the trail through the vegetation, uploaded before anything draws with it
		float eye = m_Camera.eyeHeight / m_Camera.metersPerUnit;
		glm::vec3 feet = m_Camera.position - glm::vec3(0.0f, eye, 0.0f);
		m_PlayerTrail.update(feet, m_GrassInteraction && m_Camera.walking && m_Camera.onGround, eye, m_Deltatime);
		m_PlayerTrail.m_Wind = m_Weather.state().wind; // the vegetation sways in the weather's wind
		m_PlayerTrail.record(cmd, foliagePC.offsetWorld);
		mark(cmd, "grass trail");
	}
	m_Foliage.update(cmd, foliagePC);
	mark(cmd, "grass scatter");
	m_Props.update(cmd, foliagePC);
	mark(cmd, "rock scatter");
	m_GroundCover.update(cmd, foliagePC);
	mark(cmd, "ground cover scatter");
// last frame's depth pyramid: only while it's valid and the map hasn't re-centred since (the camera spaces differ)
	{
		VkExtent2D pyramid = m_HiZ.getSize();
		bool usable = m_OcclusionCulling && m_HiZValid && m_HiZMapOffset == m_MapOffset;
		m_Trees.m_Occlusion = { m_HiZViewProj, glm::vec4(pyramid.width, pyramid.height, m_HiZ.getMips(), usable ? 1.0f : 0.0f) };
		VkMemoryBarrier2 barrier{ .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
		barrier.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
		barrier.srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT;
		barrier.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
		barrier.dstAccessMask = VK_ACCESS_2_SHADER_READ_BIT;
		VkDependencyInfo dependency{ .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
		dependency.memoryBarrierCount = 1;
		dependency.pMemoryBarriers = &barrier;
		vkCmdPipelineBarrier2(cmd, &dependency);
	}
	m_Trees.update(cmd, foliagePC);
	mark(cmd, "tree placement");

	// the far field terrain cache: what came into its squares around the camera (terrain.frag reads it)
	if(m_TerrainCacheEnabled) {
		glm::dvec2 offsetWorld = glm::dvec2(m_MapOffset) * (static_cast<double>(m_TerrainScale) / m_CoordinateMultiplier);
		glm::dvec2 cameraWorld = glm::dvec2(m_CameraPos.x, m_CameraPos.z) + offsetWorld;
		if(m_TerrainCache.update(cmd, cameraWorld, m_TerrainPC.factor, m_AtmosphereManager.m_AtmosphereData.terrainMacro, m_TerrainDescriptors, m_AtmosphereManager.getRenderDescriptor()))
			m_FrameEvents |= FRAME_TERRAIN_CACHE;
	} else m_TerrainCache.publishOff(cmd); // (the grass falls back to its own colour)

	mark(cmd, "terrain cache");
	// shadow maps for whichever of the sun / moon is up
	ShadowLightKeys shadowKeys = m_AtmosphereManager.getShadowLight(m_Shadows.getLightStep());
	m_Shadows.update(m_View, shadowKeys, m_TerrainPC.factor, m_FrameNumber);
	m_ShadowCascadesRendered[m_FrameNumber % FRAME_OVERLAP] = m_Shadows.render(cmd, [&](VkCommandBuffer cmd, const ShadowCascadeDraw& draw) {
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_TerrainShadowPipeline);
		drawTerrain(cmd, draw.viewProj, &draw);
		if(draw.cascade < SHADOW_TERRAIN_ONLY) m_Trees.drawShadow(cmd, draw.viewProj); // the far ones are terrain only
		if(draw.cascade == 0) m_Foliage.drawShadow(cmd, draw.viewProj); // grass shadows only matter up close
		if(draw.cascade < SHADOW_TERRAIN_ONLY) m_Props.drawShadow(cmd, draw.viewProj);
		if(draw.cascade == 0) m_GroundCover.drawShadow(cmd, draw.viewProj); // near the camera only, like the grass
	});
	if(m_ShadowCascadesRendered[m_FrameNumber % FRAME_OVERLAP] > 0) m_FrameEvents |= FRAME_SHADOWS;


	mark(cmd, "shadow maps");
	// START DRAW COMMANDS
	// no clear needed, the sky draws everywhere the terrain doesn't
	vkutil::transition_image(cmd, m_DrawImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	vkutil::transition_image(cmd, m_DepthBuffer.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
	vkutil::transition_image(cmd, m_ShadowFilter.getDirectImage(), VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	for(VkImage target : { m_GAlbedo.image, m_GNormal.image, m_GMaterial.image, m_GVelocity.image })
		vkutil::transition_image(cmd, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

	drawGeometry(cmd); // (marks each part)
	m_ShadowFilter.resolve(cmd, m_DrawExtent, m_DrawImage.image, m_DepthBuffer.image);
	mark(cmd, "shadow filter");
	if(m_TAA) drawTAA(cmd); // TSR: into the output image at the window's resolution
	else {
		// no TAA: the render resolution frame stretched to the output (bilinear)
		m_HistoryValid = false;
		vkutil::transition_image(cmd, m_DrawImage.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
		vkutil::transition_image(cmd, m_OutputImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		vkutil::copy_image_to_image(cmd, m_DrawImage.image, m_OutputImage.image, m_DrawExtent, m_SwapchainExtent);
		vkutil::transition_image(cmd, m_OutputImage.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
		vkutil::transition_image(cmd, m_DrawImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		mark(cmd, "upscale (TAA off)");
	}

	// bloom + exposure + tonemapping, in place on the output image (GENERAL)
	vkutil::transition_image(cmd, m_DepthBuffer.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, vkinit::image_subresource_range(VK_IMAGE_ASPECT_DEPTH_BIT));

	// what's falling (weather.cpp): onto the TAA's output at the window's resolution, before the bloom + tonemapping
	drawPrecipitation(cmd);
	mark(cmd, "precipitation");

	m_Bloom.render(cmd);
	mark(cmd, "bloom");
	m_AtmosphereManager.postProcess(cmd, m_SwapchainExtent, [&](const char* name) { mark(cmd, name); });
	// END DRAW COMMANDS

	// set the output image to read mode
	vkutil::transition_image(cmd, m_OutputImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	if(m_CaptureThisFrame) {
		VkBufferImageCopy region{};
		region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
		region.imageExtent = { m_SwapchainExtent.width, m_SwapchainExtent.height, 1 };
		vkCmdCopyImageToBuffer(cmd, m_OutputImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_CaptureBuffer.buffer, 1, &region);
		mark(cmd, "capture readback");
	}
	// set the swapchain to write mode
	vkutil::transition_image(cmd, m_SwapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	vkutil::copy_image_to_image(cmd, m_OutputImage.image, m_SwapchainImages[swapchainImageIndex], m_SwapchainExtent, m_SwapchainExtent);

	vkutil::transition_image(cmd, m_SwapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	mark(cmd, "present copy");

	recordDrawStats(cmd);
	mark(cmd, "draw counts");
	if(!m_Mouselock) {
		m_DebugView.record(cmd, debugTextures(), m_FrameNumber); // the picked texture into what the window shows
		mark(cmd, "debug view");
		drawImGui(cmd, m_SwapchainImageViews[swapchainImageIndex]);
		mark(cmd, "UI");
	}

	vkutil::transition_image(cmd, m_SwapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
	VK_CHECK(vkEndCommandBuffer(cmd));
	
	VkCommandBufferSubmitInfo cmdinfo = vkinit::command_buffer_submit_info(cmd); // beautiful abstraction 🙏
	
	VkSemaphoreSubmitInfo waitInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR, getCurrentFrame().swapchainSemaphore);
	VkSemaphoreSubmitInfo signalInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, m_RenderSemaphores[swapchainImageIndex]);
	
	VkSubmitInfo2 submit = vkinit::submit_info(&cmdinfo, &signalInfo, &waitInfo);
	
	VK_CHECK(vkQueueSubmit2(m_GraphicsQueue, 1, &submit, getCurrentFrame().renderFence));
	if(m_CaptureThisFrame) {
		vkQueueWaitIdle(m_GraphicsQueue);
		writeCapture();
		m_CaptureThisFrame = false;
	}
	
	// present prep
	VkPresentInfoKHR presentInfo = {};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.pNext = nullptr;
	presentInfo.pSwapchains = &m_Swapchain;
	presentInfo.swapchainCount = 1;
	
	presentInfo.pWaitSemaphores = &m_RenderSemaphores[swapchainImageIndex];
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
	
	{
		std::lock_guard<std::mutex> lock(m_QueueMutex); // the loading screen presents from the main thread meanwhile
		VK_CHECK(vkQueueSubmit2(m_GraphicsQueue, 1, &submit, m_ImmFence));
	}
	VK_CHECK(vkWaitForFences(m_Device, 1, &m_ImmFence, true, 9999999999));	
}
void Application::parseShots() {
	// VULKAN2_SPYGLASS=<zoom> zooms in (as if C were held) at that magnification
	if(const char* spy = std::getenv("VULKAN2_SPYGLASS")) {
		m_Camera.forceSpyglass = true;
		m_Camera.zoomTarget = glm::clamp(static_cast<float>(std::atof(spy)), 2.0f, 64.0f);
	}
	const char* env = std::getenv("VULKAN2_SHOT");
	if(!env) return;
	std::stringstream all(env);
	std::string entry;
	while(std::getline(all, entry, ';')) {
		std::stringstream fields(entry);
		std::string field;
		std::vector<std::string> parts;
		while(std::getline(fields, field, ',')) parts.push_back(field);
		if(parts.size() < 7) { fmt::print("VULKAN2_SHOT: skipping '{}' (x,z,height,yaw,pitch,hour,file[,weather[,dx:dz strike]])\n", entry); continue; }
		m_Shots.push_back({ std::stof(parts[0]), std::stof(parts[1]), std::stof(parts[2]), std::stof(parts[3]), std::stof(parts[4]), std::stof(parts[5]), parts[6],
							parts.size() > 7 ? parts[7] : "", parts.size() > 8 ? parts[8] : "" });
	}
	if(m_Shots.empty()) return;
	const char* pair = std::getenv("VULKAN2_SHOT_PAIR");
	m_ShotPair = pair != nullptr;
	m_ShotPairAnimated = pair && std::string(pair) == "anim"; // =anim: the wind keeps going
	// no vsync while capturing: waiting on the display lets the GPU idle and clock down between frames, which
	// makes the pass timings slower and noisy
	m_Vsync = false;
	m_Settings.vsync = false;
	m_Resized = true;
	// RGBA16F readback of the draw image, big enough for any window size
	m_CaptureBuffer = createBuffer(size_t(4096) * 4096 * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
	m_DeletionQueue.push([&]() { destroyBuffer(m_CaptureBuffer); });
}

bool Application::updateShots() {
	if(m_ShotIndex >= m_Shots.size()) return false;
	const Shot& shot = m_Shots[m_ShotIndex];
	// a shot's own weather (fully set in), and a strike held in view (dx:dz km from the camera)
	if(m_ShotFrames == 0) {
		WeatherType type;
		if(!shot.weather.empty() && Weather::fromName(shot.weather.c_str(), type)) { m_Weather.m_Auto = false; m_Weather.forceType(type); }
		m_Weather.m_HoldStrike = m_Weather.m_PlaceStrike = !shot.strike.empty();
		if(size_t colon = shot.strike.find(':'); colon != std::string::npos)
			m_Weather.m_PlacedStrike = glm::vec2(std::stof(shot.strike.substr(0, colon)), std::stof(shot.strike.substr(colon + 1)));
	}
	// the ground height comes back from the probe a couple of frames late, keep following it while settling
	m_Camera.walking = false;
	m_Camera.velocity = glm::vec3(0.0f);
	m_Camera.position = glm::vec3(shot.x, m_Camera.groundHeight + shot.height, shot.z);
	// VULKAN2_SHOT_STRAFE=<metres per frame>: the camera walks sideways during the shot (trees passing in front of the
	// clouds with parallax: ghosting in the histories)
	static const float strafe = std::getenv("VULKAN2_SHOT_STRAFE") ? static_cast<float>(std::atof(std::getenv("VULKAN2_SHOT_STRAFE"))) : 0.0f;
	m_Camera.position += glm::vec3(std::cos(shot.yaw), 0.0f, std::sin(shot.yaw)) * strafe * static_cast<float>(m_ShotFrames);
	// VULKAN2_SHOT_PAN=<radians per frame>: the camera turns during the shot (trees sweeping past the clouds: ghosting)
	static const float pan = std::getenv("VULKAN2_SHOT_PAN") ? static_cast<float>(std::atof(std::getenv("VULKAN2_SHOT_PAN"))) : 0.0f;
	m_Camera.yaw = shot.yaw + pan * static_cast<float>(m_ShotFrames);
	m_Camera.pitch = shot.pitch;
	m_AtmosphereManager.setTimeOfDay(shot.hour, false);
	// enough frames for the heightmap, tree placement, shadow cascades and the smoothed GPU timers to settle
	if(++m_ShotFrames == 90 || (m_ShotPair && m_ShotFrames == 91)) m_CaptureThisFrame = true;
	return true;
}

void Application::writeCapture() {
	const Shot& shot = m_Shots[m_ShotIndex];
	vmaInvalidateAllocation(m_Allocator, m_CaptureBuffer.allocation, 0, VK_WHOLE_SIZE);
	const uint16_t* half = static_cast<const uint16_t*>(m_CaptureBuffer.info.pMappedData);
	uint32_t w = m_SwapchainExtent.width, h = m_SwapchainExtent.height; // the output image
	std::vector<uint8_t> pixels(size_t(w) * h * 3);
	for(size_t i = 0; i < size_t(w) * h; i++)
		for(int c = 0; c < 3; c++) pixels[i * 3 + c] = static_cast<uint8_t>(std::clamp(glm::unpackHalf1x16(half[i * 4 + c]), 0.0f, 1.0f) * 255.0f + 0.5f);
	// pair mode: frame 90 goes next to the shot as *_prev.png, frame 91 is the shot (diff them for flicker)
	bool first = m_ShotPair && m_ShotFrames == 90;
	std::string file = shot.file;
	if(first) file = file.substr(0, file.size() - 4) + "_prev.png";
	stbi_write_png(file.c_str(), w, h, 3, pixels.data(), w * 3);
	if(first) return;

	float total = gpuTotalMs();
	glm::vec2 exposure = m_AtmosphereManager.getExposureState();
	fmt::print("shot {} ground {:.3f} push {:.3f} {:.3f} | luminance {:.3f} exposure {:.3f} | GPU {:.2f} ms |", shot.file, m_Camera.groundHeight, m_LastProbe.y, m_LastProbe.z, exposure.x, exposure.y, total);
	for(const GpuTimer& t : m_GpuTimers) fmt::print(" {} {:.2f} |", t.name, t.ms);
	GpuMemory memory = gpuMemory();
	const double MB = 1024.0 * 1024.0;
	fmt::print(" memory {:.0f} / {:.0f} MB, ours {:.0f} MB\n", memory.used / MB, memory.available / MB, memory.ours / MB);
	if(m_ShowDrawStats) { // (VULKAN2_DEBUGTEX_CYCLE turns them on)
		fmt::print("  draws:");
		for(const DrawStat& s : m_DrawStats) fmt::print(" {} {} ({:.2f}M tris) |", s.name, s.instances, s.triangles / 1e6);
		fmt::print("\n");
	}
	std::fflush(stdout);
	m_ShotIndex++;
	m_ShotFrames = 0;
}

void Application::initHorizonAO() {
	// (the sky visibility image itself comes with the heightmap's, createHeightmapImages)
	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER);
	builder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
	builder.add_binding(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // the climate map (the canopy)
	m_SkyVisibilityLayout = builder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
	m_SkyVisibilitySet = g_DescriptorAllocator.allocate(m_Device, m_SkyVisibilityLayout);

	VkDescriptorImageInfo heightmapInfo{ m_HeightmapSampler, m_Heightmap.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkDescriptorImageInfo outputInfo{ VK_NULL_HANDLE, m_SkyVisibility.imageView, VK_IMAGE_LAYOUT_GENERAL };
	VkDescriptorImageInfo climateInfo{ m_HeightmapSampler, m_ClimateMap.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkWriteDescriptorSet writes[3] = {};
	for(auto& write : writes) { write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; write.dstSet = m_SkyVisibilitySet; write.descriptorCount = 1; }
	writes[0].dstBinding = 0; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].pImageInfo = &heightmapInfo;
	writes[1].dstBinding = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; writes[1].pImageInfo = &outputInfo;
	writes[2].dstBinding = 2; writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[2].pImageInfo = &climateInfo;
	vkUpdateDescriptorSets(m_Device, 3, writes, 0, nullptr);

	VkPushConstantRange range{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(glm::ivec4) + sizeof(glm::vec4) };
	VkPipelineLayoutCreateInfo layoutCI = vkinit::pipeline_layout_create_info();
	layoutCI.setLayoutCount = 1;
	layoutCI.pSetLayouts = &m_SkyVisibilityLayout;
	layoutCI.pushConstantRangeCount = 1;
	layoutCI.pPushConstantRanges = &range;
	VkPipelineLayout layout;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &layoutCI, nullptr, &layout));
	m_SkyVisibilityEffect = loadComputeShader("src/shaders/bin/horizonao.comp.spv", "horizon ao", layout);

	m_DeletionQueue.push([&]() {
		vkDestroyPipelineLayout(m_Device, m_SkyVisibilityEffect.layout, nullptr);
		vkDestroyDescriptorSetLayout(m_Device, m_SkyVisibilityLayout, nullptr);
	});
}

void Application::dispatchHorizonAO(VkCommandBuffer cmd, glm::ivec2 heightmapStart, glm::ivec2 heightmapSize) {
	// the rays reach ~100 heightmap texels, everything that close to the regenerated strip sees new terrain too
	const int reach = std::max(4, static_cast<int>(std::ceil(104.0f * m_HeightmapSize / 4096.0f))); // horizonao.comp marches in 4096 texels
	const int ratio = 2; // heightmap texels per sky visibility texel (half resolution)
	const int aoSize = m_HeightmapSize / ratio;
	struct { glm::ivec2 start, size; float factor, texelWorld, density, pad; } pc{};
	pc.start = (heightmapStart - glm::ivec2(reach)) / ratio;
	pc.size = glm::min((heightmapSize + glm::ivec2(2 * reach)) / ratio + 1, glm::ivec2(aoSize));
	pc.factor = m_WorldSize * TERRAIN_HEIGHT_SCALE * m_TerrainScale; // m_TerrainPC.factor, which isn't set yet on the first generation
	pc.texelWorld = static_cast<float>(m_WorldSize) * m_TerrainScale / static_cast<float>(m_HeightmapSize);
	pc.density = m_Trees.m_Enabled ? m_Trees.m_Density : 0.0f; // the forest canopy (g) follows the trees that are drawn

	vkutil::transition_image(cmd, m_SkyVisibility.image, m_SkyVisibilityReady ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_SkyVisibilityEffect.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_SkyVisibilityEffect.layout, 0, 1, &m_SkyVisibilitySet, 0, nullptr);
	vkCmdPushConstants(cmd, m_SkyVisibilityEffect.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	vkCmdDispatch(cmd, (pc.size.x + 7) / 8, (pc.size.y + 7) / 8, 1);
	vkutil::transition_image(cmd, m_SkyVisibility.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	m_SkyVisibilityReady = true;
}

// ------------------------------------------------------------------ heightmap resolution / filtering

void Application::createHeightmapImages() {
	VmaAllocationCreateInfo allocCI = {};
	allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	allocCI.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	auto create = [&](AllocatedImage& image, VkFormat format, VkImageUsageFlags usage, uint32_t size) {
		image.imageFormat = format;
		image.imageExtent = { size, size, 1 };
		VkImageCreateInfo imageCI = vkinit::image_create_info(format, usage, image.imageExtent);
		VK_CHECK(vmaCreateImage(m_Allocator, &imageCI, &allocCI, &image.image, &image.allocation, nullptr));
		VkImageViewCreateInfo viewCI = vkinit::imageview_create_info(format, image.image, VK_IMAGE_ASPECT_COLOR_BIT);
		VK_CHECK(vkCreateImageView(m_Device, &viewCI, nullptr, &image.imageView));
	};
	uint32_t size = static_cast<uint32_t>(m_HeightmapSize);
	// the height (heightmap.comp). its sampled view puts the height in .a, where every shader reads it from (they used
	// to share an RGBA16 texel with the climate), and zeros in rgb: a climate read left on it shows as snow everywhere.
	// heightmap.comp writes through a plain view (storage views can't swizzle)
	create(m_Heightmap, VK_FORMAT_R16_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, size);
	m_HeightmapStorageView = m_Heightmap.imageView;
	{
		VkImageViewCreateInfo viewCI = vkinit::imageview_create_info(VK_FORMAT_R16_UNORM, m_Heightmap.image, VK_IMAGE_ASPECT_COLOR_BIT);
		viewCI.components = { VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R };
		VK_CHECK(vkCreateImageView(m_Device, &viewCI, nullptr, &m_Heightmap.imageView));
	}
	// rgb = climate (temperature, moisture, ruggedness), half resolution: smooth over kilometres
	create(m_ClimateMap, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, size / 2);
	// horizon AO, half the resolution (horizonao.comp)
	create(m_SkyVisibility, VK_FORMAT_R16G16_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, size / 2);
}

void Application::destroyHeightmapImages() {
	vkDestroyImageView(m_Device, m_HeightmapStorageView, nullptr);
	for(AllocatedImage* image : { &m_Heightmap, &m_ClimateMap, &m_SkyVisibility }) {
		vkDestroyImageView(m_Device, image->imageView, nullptr);
		vmaDestroyImage(m_Allocator, image->image, image->allocation);
	}
}

void Application::initTAA() {
	DescriptorLayoutBuilder builder;
	builder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // this frame (the draw image, render resolution, linear)
	builder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // last frame's result
	builder.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);		   // this frame's result
	builder.add_binding(3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // depth
	builder.add_binding(4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // G-buffer velocity (wind)
	m_TAALayout = builder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
	for(VkDescriptorSet& set : m_TAASets) set = g_DescriptorAllocator.allocate(m_Device, m_TAALayout);

	VkPushConstantRange range{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TAAPushConstants) };
	VkPipelineLayoutCreateInfo layoutCI = vkinit::pipeline_layout_create_info();
	layoutCI.setLayoutCount = 1;
	layoutCI.pSetLayouts = &m_TAALayout;
	layoutCI.pushConstantRangeCount = 1;
	layoutCI.pPushConstantRanges = &range;
	VkPipelineLayout layout;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &layoutCI, nullptr, &layout));
	m_TAAEffect = loadComputeShader("src/shaders/bin/taa.comp.spv", "taa", layout);

	DescriptorLayoutBuilder sharpenBuilder;
	sharpenBuilder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // resolved history
	sharpenBuilder.add_binding(1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);		  // draw image
	m_SharpenLayout = sharpenBuilder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
	for(VkDescriptorSet& set : m_SharpenSets) set = g_DescriptorAllocator.allocate(m_Device, m_SharpenLayout);
	VkPushConstantRange sharpenRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float) };
	layoutCI.pSetLayouts = &m_SharpenLayout;
	layoutCI.pPushConstantRanges = &sharpenRange;
	VkPipelineLayout sharpenLayout;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &layoutCI, nullptr, &sharpenLayout));
	m_SharpenEffect = loadComputeShader("src/shaders/bin/sharpen.comp.spv", "taa sharpen", sharpenLayout);

	writeTAADescriptors();
	m_DeletionQueue.push([&]() {
		vkDestroyPipelineLayout(m_Device, m_TAAEffect.layout, nullptr);
		vkDestroyDescriptorSetLayout(m_Device, m_TAALayout, nullptr);
		vkDestroyPipelineLayout(m_Device, m_SharpenEffect.layout, nullptr);
		vkDestroyDescriptorSetLayout(m_Device, m_SharpenLayout, nullptr);
	});
}

void Application::writeTAADescriptors() {
	if(m_TAASets[0] == VK_NULL_HANDLE) return; // the first swapchain exists before the pipelines
	for(int i = 0; i < 2; i++) {
		VkDescriptorImageInfo infos[5] = {
			{ m_AtmosphereSampler, m_DrawImage.imageView, VK_IMAGE_LAYOUT_GENERAL }, // linear, clamped
			{ m_AtmosphereSampler, m_History[1 - i].imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }, // linear, clamped
			{ VK_NULL_HANDLE, m_History[i].imageView, VK_IMAGE_LAYOUT_GENERAL },
			{ m_NearestSampler, m_DepthBuffer.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
			{ m_NearestSampler, m_GVelocity.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		};
		VkWriteDescriptorSet writes[5] = {};
		for(uint32_t b = 0; b < 5; b++) {
			writes[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			writes[b].dstSet = m_TAASets[i];
			writes[b].dstBinding = b;
			writes[b].descriptorCount = 1;
			writes[b].descriptorType = b == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
			writes[b].pImageInfo = &infos[b];
		}
		vkUpdateDescriptorSets(m_Device, 5, writes, 0, nullptr);

		VkDescriptorImageInfo sharpenInfos[2] = {
			{ m_NearestSampler, m_History[i].imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
			{ VK_NULL_HANDLE, m_OutputImage.imageView, VK_IMAGE_LAYOUT_GENERAL },
		};
		VkWriteDescriptorSet sharpenWrites[2] = {};
		for(uint32_t b = 0; b < 2; b++) {
			sharpenWrites[b].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
			sharpenWrites[b].dstSet = m_SharpenSets[i];
			sharpenWrites[b].dstBinding = b;
			sharpenWrites[b].descriptorCount = 1;
			sharpenWrites[b].descriptorType = b == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
			sharpenWrites[b].pImageInfo = &sharpenInfos[b];
		}
		vkUpdateDescriptorSets(m_Device, 2, sharpenWrites, 0, nullptr);
	}
}

void Application::drawTAA(VkCommandBuffer cmd) {
	int write = m_FrameNumber % 2, read = 1 - write;
	VkImageSubresourceRange depthRange = vkinit::image_subresource_range(VK_IMAGE_ASPECT_DEPTH_BIT);
	// after a resize / with TAA just turned on both histories are fresh (UNDEFINED), the old one isn't used then
	VkImageLayout readFrom = m_HistoryValid ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
	if(!m_HistoryValid) vkutil::transition_image(cmd, m_History[read].image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	vkutil::transition_image(cmd, m_History[write].image, readFrom, VK_IMAGE_LAYOUT_GENERAL);
	vkutil::transition_image(cmd, m_DrawImage.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
	vkutil::transition_image(cmd, m_DepthBuffer.image, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, depthRange);

	TAAPushConstants pc{};
	// unjittered on both ends: the history holds the scene at pixel centres, so a still camera must map every pixel
	// onto itself. going through the jittered matrix shifted the history by the jitter each frame (the whole
	// image shook, worst at low render scales where a pixel is big)
	pc.reproject = m_PreviousViewProj * glm::inverse(m_UnjitteredProjection * m_View);
	pc.blend = 0.1f;
	pc.historyValid = m_HistoryValid ? 1 : 0;
	pc.jitter = m_Jitter;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_TAAEffect.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_TAAEffect.layout, 0, 1, &m_TAASets[write], 0, nullptr);
	vkCmdPushConstants(cmd, m_TAAEffect.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
	vkCmdDispatch(cmd, (m_SwapchainExtent.width + 7) / 8, (m_SwapchainExtent.height + 7) / 8, 1); // output resolution
	mark(cmd, "TAA");

	// the result stays around as next frame's history, a sharpened copy goes into the output image for post
	vkutil::transition_image(cmd, m_History[write].image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	vkutil::transition_image(cmd, m_OutputImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_SharpenEffect.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_SharpenEffect.layout, 0, 1, &m_SharpenSets[write], 0, nullptr);
	vkCmdPushConstants(cmd, m_SharpenEffect.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(float), &m_Sharpness);
	vkCmdDispatch(cmd, (m_SwapchainExtent.width + 7) / 8, (m_SwapchainExtent.height + 7) / 8, 1);
	mark(cmd, "sharpen");
	vkutil::transition_image(cmd, m_DrawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	vkutil::transition_image(cmd, m_DepthBuffer.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, depthRange);
	m_HistoryValid = true;
}

// everything the debug texture viewer can show, in the layout it's in once the frame's rendering is done (when the
// viewer copies it, right before the UI). rebuilt every frame: views and sizes change with resizes and settings
std::vector<DebugTexture> Application::debugTextures() {
	const VkImageLayout read = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	std::vector<DebugTexture> textures;
	auto add = [&](const char* name, VkImageView view, VkImageLayout layout, DebugTexHint hint, VkExtent3D extent) -> DebugTexture& {
		DebugTexture& texture = textures.emplace_back();
		texture.name = name;
		texture.view = view;
		texture.layout = layout;
		texture.hint = hint;
		texture.extent = extent;
		return texture;
	};
	auto extent2D = [](VkExtent2D e) { return VkExtent3D{ e.width, e.height, 1 }; };

	// shadows
	{
		uint32_t resolution = m_Shadows.getResolution();
		DebugTexture& shadows = add("shadow maps", m_Shadows.getArrayView(), read, DebugTexHint::Depth, { resolution, resolution, 1 });
		shadows.kind = DebugTexKind::Array;
		shadows.layers = SHADOW_SETS * SHADOW_CASCADES;
		shadows.depthFormat = true;
		shadows.layerName = [](uint32_t layer) -> const char* {
			static char label[48];
			snprintf(label, sizeof(label), "set %u, cascade %u%s", layer / SHADOW_CASCADES, layer % SHADOW_CASCADES,
					 layer % SHADOW_CASCADES >= SHADOW_TERRAIN_ONLY ? " (terrain only)" : "");
			return label;
		};
	}
	// (left in GENERAL by the shadow filter's resolve; it was never sampleable, the views were black)
	DebugTexture& direct = add("shadow term (a, before the screen space blur)", m_ShadowFilter.getDirectView(), read, DebugTexHint::Data, m_DrawImage.imageExtent);
	direct.image = m_ShadowFilter.getDirectImage();
	direct.transitionFrom = VK_IMAGE_LAYOUT_GENERAL;
	direct.channels = 4;
	DebugTexture& directLight = add("direct light (unshadowed)", m_ShadowFilter.getDirectView(), read, DebugTexHint::Hdr, m_DrawImage.imageExtent);
	directLight.image = m_ShadowFilter.getDirectImage();
	directLight.transitionFrom = VK_IMAGE_LAYOUT_GENERAL;

	// camera depth + G-buffer
	add("depth buffer", m_DepthBuffer.imageView, read, DebugTexHint::ReverseDepth, m_DepthBuffer.imageExtent).depthFormat = true;
	if(m_HiZValid) {
		DebugTexture& hiz = add("hi-z pyramid (last frame's depth)", m_HiZ.getView(), VK_IMAGE_LAYOUT_GENERAL, DebugTexHint::ReverseDepth, extent2D(m_HiZ.getSize()));
		hiz.mips = m_HiZ.getMips();
	}
	add("g-buffer albedo (a = AO)", m_GAlbedo.imageView, read, DebugTexHint::Color, m_GAlbedo.imageExtent);
	add("g-buffer normal", m_GNormal.imageView, read, DebugTexHint::Normal, m_GNormal.imageExtent);
	add("g-buffer material (id, roughness, direct)", m_GMaterial.imageView, read, DebugTexHint::Data, m_GMaterial.imageExtent);
	add("g-buffer velocity", m_GVelocity.imageView, read, DebugTexHint::Velocity, m_GVelocity.imageExtent);

	// lighting
	DebugTexture& scene = add("hdr scene (before TAA / post)", m_DrawImage.imageView, read, DebugTexHint::Hdr, m_DrawImage.imageExtent);
	scene.image = m_DrawImage.image;
	scene.transitionFrom = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	add("shadow term, horizontal blur pass", m_ShadowFilter.getTempView(), VK_IMAGE_LAYOUT_GENERAL, DebugTexHint::Data, m_ShadowFilter.getTempExtent());
	add("ssao (r = AO, g = lit air for the haze)", m_SSAOImage.imageView, read, DebugTexHint::Data, m_SSAOImage.imageExtent);
	for(int i = 0; i < 2; i++)
		add(i ? "TAA history B" : "TAA history A", m_History[i].imageView, read, DebugTexHint::Hdr, m_History[i].imageExtent);
	// volumetric fog (volfog.cpp): the froxel grid, always in GENERAL
	for(int i = 0; i < 2; i++)
		add(i ? "fog froxels B (rgb in-scattering, a = extinction)" : "fog froxels A (rgb in-scattering, a = extinction)", m_FogFroxels[i].imageView,
			VK_IMAGE_LAYOUT_GENERAL, DebugTexHint::Hdr, m_FogFroxels[i].imageExtent).kind = DebugTexKind::Tex3D;
	add("fog integrated (rgb towards the camera, a = transmittance)", m_FogIntegrated.imageView, VK_IMAGE_LAYOUT_GENERAL, DebugTexHint::Hdr,
		m_FogIntegrated.imageExtent).kind = DebugTexKind::Tex3D;
	// clouds
	add("clouds raw (this frame's march, before the reprojection)", m_CloudRaw.imageView, read, DebugTexHint::Hdr, m_CloudRaw.imageExtent);
	add("clouds history", m_CloudHistory.imageView, read, DebugTexHint::Hdr, m_CloudHistory.imageExtent);
	add("clouds (rgb = light, a = transmittance)", m_CloudImage.imageView, read, DebugTexHint::Hdr, m_CloudImage.imageExtent);
	add("cloud coverage map", m_CloudMap.imageView, VK_IMAGE_LAYOUT_GENERAL, DebugTexHint::Data, m_CloudMap.imageExtent);
	add("cloud noise", m_CloudNoise.imageView, read, DebugTexHint::Data, m_CloudNoise.imageExtent).kind = DebugTexKind::Tex3D;
	add("cirrus noise lattice", m_CirrusNoise.imageView, read, DebugTexHint::Data, m_CirrusNoise.imageExtent);
	DebugTexture& bloom = add("bloom chain", m_Bloom.getView(), VK_IMAGE_LAYOUT_GENERAL, DebugTexHint::Hdr, m_OutputImage.imageExtent);
	bloom.mips = Bloom::MIPS;

	// atmosphere
	add("transmittance LUT", m_AtmosphereManager.transmittance.imageView, read, DebugTexHint::Color, m_AtmosphereManager.transmittance.imageExtent);
	add("multiscattering LUT", m_AtmosphereManager.multiscattering.imageView, read, DebugTexHint::Hdr, m_AtmosphereManager.multiscattering.imageExtent);
	add("sky view LUT", m_AtmosphereManager.skyview.imageView, read, DebugTexHint::Hdr, m_AtmosphereManager.skyview.imageExtent);
	add("aerial perspective LUT", m_AtmosphereManager.aerial.imageView, read, DebugTexHint::Hdr, m_AtmosphereManager.aerial.imageExtent).kind = DebugTexKind::Tex3D;
	add("aerial perspective LUT, sun / moon single scattering", m_AtmosphereManager.aerialSun.imageView, read, DebugTexHint::Hdr,
		m_AtmosphereManager.aerialSun.imageExtent).kind = DebugTexKind::Tex3D;
	add("moon", m_MoonTexture.imageView, read, DebugTexHint::Color, m_MoonTexture.imageExtent);

	// vegetation
	add("tree impostors albedo (16 meshes across, 3 elevations each)", m_Trees.impostorAlbedo().imageView, read, DebugTexHint::Color, m_Trees.impostorAlbedo().imageExtent);
	add("tree impostors normal", m_Trees.impostorNormal().imageView, read, DebugTexHint::Normal, m_Trees.impostorNormal().imageExtent);

	// terrain
	add("heightmap (a = height)", m_Heightmap.imageView, read, DebugTexHint::Data, m_Heightmap.imageExtent);
	add("climate map (temperature, moisture, ruggedness)", m_ClimateMap.imageView, read, DebugTexHint::Data, m_ClimateMap.imageExtent);
	if(m_SkyVisibilityReady) add("sky visibility (r = horizon AO, g = forest canopy)", m_SkyVisibility.imageView, read, DebugTexHint::Data, m_SkyVisibility.imageExtent);
	for(int normal = 0; normal < 2; normal++) {
		DebugTexture& cache = add(normal ? "terrain cache normal" : "terrain cache albedo", normal ? m_TerrainCache.getNormalView() : m_TerrainCache.getAlbedoView(),
								  read, normal ? DebugTexHint::Data : DebugTexHint::Color, { TerrainCache::SIZE, TerrainCache::SIZE, 1 });
		cache.kind = DebugTexKind::Array;
		cache.layers = TerrainCache::LEVELS;
		cache.mips = m_TerrainCache.getMips();
		cache.layerName = [](uint32_t layer) -> const char* {
			static char label[24];
			snprintf(label, sizeof(label), "level %u", layer);
			return label;
		};
	}
	return textures;
}

// ------------------------------------------------------------------ GPU draw counts

std::vector<DrawStatBuffer> Application::drawStatBuffers() const {
	std::vector<DrawStatBuffer> buffers;
	for(const auto& list : { m_Foliage.drawStatBuffers(), m_Trees.drawStatBuffers(), m_Props.drawStatBuffers(), m_GroundCover.drawStatBuffers() })
		buffers.insert(buffers.end(), list.begin(), list.end());
	return buffers;
}

// at the end of the frame (every draw that reads them is recorded): the indirect buffers into this frame's slot
void Application::recordDrawStats(VkCommandBuffer cmd) {
	uint32_t slot = m_FrameNumber % FRAME_OVERLAP;
	m_DrawStatsWritten[slot] = false;
	if(!m_ShowDrawStats) return;
	std::vector<DrawStatBuffer> buffers = drawStatBuffers();
	if(!m_DrawStatsReadback.buffer) {
		for(const DrawStatBuffer& b : buffers) m_DrawStatsSlotBytes += b.bytes;
		m_DrawStatsReadback = createBuffer(m_DrawStatsSlotBytes * FRAME_OVERLAP, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_TO_CPU);
		m_DeletionQueue.push([this]() { destroyBuffer(m_DrawStatsReadback); });
	}
	vkutil::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
						  VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
	VkDeviceSize offset = slot * m_DrawStatsSlotBytes;
	for(const DrawStatBuffer& b : buffers) {
		VkBufferCopy copy{ 0, offset, b.bytes };
		vkCmdCopyBuffer(cmd, b.buffer, m_DrawStatsReadback.buffer, 1, &copy);
		offset += b.bytes;
	}
	// next frame's resets (vkCmdUpdateBuffer) and scatters write these buffers again: not before the copies read them
	vkutil::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_NONE);
	m_DrawStatsWritten[slot] = true;
}

// after this slot's fence: its copies are done, turn them into the window's rows
void Application::readDrawStats() {
	uint32_t slot = m_FrameNumber % FRAME_OVERLAP;
	if(!m_DrawStatsWritten[slot] || !m_DrawStatsReadback.buffer) return;
	vmaInvalidateAllocation(m_Allocator, m_DrawStatsReadback.allocation, 0, VK_WHOLE_SIZE);
	const uint8_t* data = static_cast<const uint8_t*>(m_DrawStatsReadback.info.pMappedData) + slot * m_DrawStatsSlotBytes;
	m_DrawStats.clear();
	auto take = [&](const std::vector<DrawStatBuffer>& buffers) {
		std::vector<const uint8_t*> pointers;
		for(const DrawStatBuffer& b : buffers) { pointers.push_back(data); data += b.bytes; }
		return pointers;
	};
	m_Foliage.drawStats(take(m_Foliage.drawStatBuffers()), m_DrawStats);
	m_Trees.drawStats(take(m_Trees.drawStatBuffers()), m_DrawStats);
	m_Props.drawStats(take(m_Props.drawStatBuffers()), m_DrawStats);
	m_GroundCover.drawStats(take(m_GroundCover.drawStatBuffers()), m_DrawStats);
}

void Application::drawDrawStatsWindow() {
	if(ImGui::Begin("GPU draw counts", &m_ShowDrawStats)) {
		ImGui::TextDisabled("what the GPU driven passes drew last frame (indirect draw buffers, read back)");
		if(ImGui::BeginTable("draws", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
			ImGui::TableSetupColumn("pass");
			ImGui::TableSetupColumn("instances");
			ImGui::TableSetupColumn("triangles");
			ImGui::TableHeadersRow();
			uint64_t instances = 0, triangles = 0;
			for(const DrawStat& s : m_DrawStats) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn(); ImGui::TextUnformatted(s.name);
				ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(s.instances));
				ImGui::TableNextColumn(); ImGui::Text("%.2fM", s.triangles / 1e6);
				instances += s.instances;
				triangles += s.triangles;
			}
			ImGui::TableNextRow();
			ImGui::TableNextColumn(); ImGui::TextUnformatted("total");
			ImGui::TableNextColumn(); ImGui::Text("%llu", static_cast<unsigned long long>(instances));
			ImGui::TableNextColumn(); ImGui::Text("%.2fM", triangles / 1e6);
			ImGui::EndTable();
		}
		ImGui::TextDisabled("(shadow casters are drawn once per cascade they're in)");
	}
	ImGui::End();
}

// the performance window: the frame (FPS, 1% low, CPU vs GPU), the GPU passes, the stutter log with what happened
// around each stutter, where you are (to share a spot as a VULKAN2_SHOT entry)
void Application::drawPerformanceWindow(float deltatimeMs) {
	if(!ImGui::Begin("performance")) { ImGui::End(); return; }

	float total = gpuTotalMs();

	// ---- the frame
	float sorted[m_FrameHistorySize];
	int count = 0;
	float sum = 0.0f;
	for(float t : m_Frametimes) if(t > 0.0f) { sorted[count++] = t; sum += t; }
	std::sort(sorted, sorted + count);
	float average = count > 0 ? sum / count : 0.0f;
	float low1 = count > 0 ? sorted[std::min(count - 1, static_cast<int>(count * 0.99f))] : 0.0f; // the 1% slowest frames
	float worst = count > 0 ? sorted[count - 1] : 0.0f;
	auto fps = [](float ms) { return ms > 0.0f ? 1000.0f / ms : 0.0f; };
	ImGui::Text("%.0f FPS", fps(average));
	ImGui::SameLine();
	ImGui::TextDisabled("avg %.1f ms   1%% low %.0f FPS (%.1f ms)   worst %.1f ms", average, fps(low1), low1, worst);
	// what limits it: the GPU's work vs the CPU recording the frame (the rest of a frame is waiting on one of them)
	bool gpuBound = total > m_CpuFrameMs;
	ImGui::Text("GPU %.2f ms   CPU %.2f ms", total, m_CpuFrameMs);
	ImGui::SameLine();
	ImGui::TextColored(gpuBound ? ImVec4(1.0f, 0.75f, 0.3f, 1.0f) : ImVec4(0.5f, 0.8f, 1.0f, 1.0f), gpuBound ? "(GPU bound)" : "(CPU bound)");
	ImGui::SetItemTooltip("GPU: the passes below. CPU: recording and submitting a frame (draw()), not counting the wait for the GPU");
	char overlay[48];
	snprintf(overlay, sizeof(overlay), "%.1f ms (%.0f FPS)", deltatimeMs, fps(deltatimeMs));
	float top = std::max(33.4f, worst * 1.1f);
	ImVec2 plotPos = ImGui::GetCursorScreenPos();
	ImGui::PlotLines("##frametimes", m_Frametimes, m_FrameHistorySize, m_FrameOffset, overlay, 0.0f, top, ImVec2(-1.0f, 110.0f));
	{
		// 60 and 30 FPS lines
		ImDrawList* drawList = ImGui::GetWindowDrawList();
		ImVec2 size = ImGui::GetItemRectSize();
		for(float ms : { 16.67f, 33.33f }) {
			if(ms > top) continue;
			float y = plotPos.y + size.y * (1.0f - ms / top);
			drawList->AddLine(ImVec2(plotPos.x, y), ImVec2(plotPos.x + size.x, y), IM_COL32(255, 255, 255, 50));
			drawList->AddText(ImVec2(plotPos.x + 2.0f, y - 13.0f), IM_COL32(255, 255, 255, 90), ms < 20.0f ? "60" : "30");
		}
	}
	ImGui::Checkbox("pause graph", &m_PauseFrametimes);

	// ---- the GPU passes
	if(ImGui::CollapsingHeader("GPU passes", ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::Checkbox("sort by cost", &m_SortPasses);
		ImGui::SameLine();
		ImGui::Checkbox("hide < 0.05 ms", &m_HideSmallPasses);
		ImGui::SameLine();
		ImGui::Checkbox("opaque breakdown", &m_DetailedOpaqueTimings);
		ImGui::SetItemTooltip("Splits the G-buffer pass into one rendering per part (terrain, trees, grass, ...) to time each (Intel reports ~0 inside a pass). Costs a little. Off: one G-buffer bar");
		auto passLine = [&](const char* name, float ms, int depth) {
			if(m_HideSmallPasses && ms < 0.05f) return;
			if(depth > 0) ImGui::Indent(depth * 12.0f);
			float share = total > 0.0f ? ms / total : 0.0f;
			// red for the big ones, yellow for the noticeable ones
			ImVec4 colour = share > 0.2f ? ImVec4(0.85f, 0.35f, 0.3f, 1.0f) : (share > 0.08f ? ImVec4(0.85f, 0.65f, 0.3f, 1.0f) : ImVec4(0.4f, 0.65f, 0.4f, 1.0f));
			ImGui::Text("%-30s %6.2f ms", name, ms);
			ImGui::SameLine();
			ImGui::PushStyleColor(ImGuiCol_PlotHistogram, colour);
			char percent[16];
			snprintf(percent, sizeof(percent), "%.0f%%", share * 100.0f);
			ImGui::ProgressBar(share, ImVec2(-1.0f, 0.0f), percent);
			ImGui::PopStyleColor();
			if(depth > 0) ImGui::Unindent(depth * 12.0f);
		};
		// one bar per feature, in the order they run (or slowest first)
		struct Line { const char* name; float ms; int depth; };
		std::vector<Line> lines;
		for(const GpuTimer& t : m_GpuTimers) lines.push_back({ t.name, t.ms, 0 });
		if(m_SortPasses) std::sort(lines.begin(), lines.end(), [](const Line& a, const Line& b) { return a.ms > b.ms; });
		for(const Line& line : lines) passLine(line.name, line.ms, line.depth);
		// shadow maps only re-render now and then, the average above hides what one re-render costs
		ImGui::TextDisabled("shadow re-render %.2f ms, %.1f per second", m_ShadowRenderMs, m_ShadowRendersPerSecond);
	}

	// ---- stutters
	char stutterHeader[48];
	snprintf(stutterHeader, sizeof(stutterHeader), "Stutters (%d)###stutters", static_cast<int>(m_Stutters.size()));
	if(ImGui::CollapsingHeader(stutterHeader, ImGuiTreeNodeFlags_DefaultOpen)) {
		ImGui::TextDisabled("frames 1.6x (and 5 ms) slower than the typical frame, with what happened in the frames around them");
		if(m_Stutters.empty()) ImGui::TextDisabled("none yet");
		else if(ImGui::SmallButton("clear")) m_Stutters.clear();
		if(!m_Stutters.empty() && ImGui::BeginTable("stutters", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
			ImGui::TableSetupColumn("ago");
			ImGui::TableSetupColumn("frame");
			ImGui::TableSetupColumn("around it", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();
			const std::pair<uint32_t, const char*> names[] = {
				{ FRAME_HEIGHTMAP, "terrain strip" }, { FRAME_ORIGIN, "map re-centred" }, { FRAME_SHADOWS, "shadow re-render" },
				{ FRAME_CLOUD_MAP, "cloud map" }, { FRAME_TERRAIN_CACHE, "terrain cache level" }, { FRAME_RESIZE, "resize" } };
			for(auto it = m_Stutters.rbegin(); it != m_Stutters.rend(); ++it) {
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::Text("%.0fs", m_Time - it->time);
				ImGui::TableNextColumn();
				ImGui::Text("%.1f ms (typ. %.1f)", it->ms, it->typical);
				ImGui::TableNextColumn();
				std::string causes;
				for(const auto& [bit, name] : names) if(it->events & bit) causes += (causes.empty() ? "" : ", ") + std::string(name);
				ImGui::TextUnformatted(causes.empty() ? "(nothing tracked: CPU side, driver or window)" : causes.c_str());
			}
			ImGui::EndTable();
		}
	}

	// ---- where you are, to share a spot: the copy is a VULKAN2_SHOT entry (x,z,height above ground,yaw,pitch,hour)
	if(ImGui::CollapsingHeader("Location", ImGuiTreeNodeFlags_DefaultOpen)) {
		const glm::vec3& pos = m_Camera.position;
		float aboveGround = pos.y - m_Camera.groundHeight;
		float hour = m_AtmosphereManager.getTimeOfDay();
		ImGui::Text("position  x %.1f  y %.1f  z %.1f  (%.0fm above ground)", pos.x, pos.y, pos.z, aboveGround * m_Camera.metersPerUnit);
		ImGui::Text("rotation  yaw %.3f  pitch %.3f   time %.2f h", m_Camera.yaw, m_Camera.pitch, hour);
		char location[160];
		snprintf(location, sizeof(location), "%.1f,%.1f,%.2f,%.3f,%.3f,%.2f", pos.x, pos.z, aboveGround, m_Camera.yaw, m_Camera.pitch, hour);
		if(ImGui::Button("Copy location")) ImGui::SetClipboardText(location);
		ImGui::SameLine();
		ImGui::TextDisabled("%s", location);
	}
	ImGui::End();
}
