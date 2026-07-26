// Application.cpp
#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h"
#include "application.hpp"

constexpr bool useValidationLayers = false; // ?

Application::Application(uint32_t width, uint32_t height) : m_Width(width), m_Height(height) {
	initWindow();

	initVulkan();
	initSwapchain();
	initCommands();
	initSyncStructures();
	
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
		
		m_DeletionQueue.flush();
	}
	
	SDL_Vulkan_UnloadLibrary();
	if (m_pWindow) {
        SDL_DestroyWindow(m_pWindow);
    }
    SDL_Quit();
	std::cout << "brr brr patapim" << std::endl;
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
	/* // I entered a small portion of code into ChatGPT because I'm using SDL3 instead of the tutorial's SDL2 implementation, it told me I'd have problems and this block of code would fix it alledgedly.
	Uint32 extensionCount = 0;

    const char* const* sdlExtensions = SDL_Vulkan_GetInstanceExtensions(&extensionCount);

    vkb::InstanceBuilder builder;
    
    for (Uint32 i = 0; i < extensionCount; i++) {
        builder.enable_extension(sdlExtensions[i]);
    }*/
	
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
	
	// GPU selection
	vkb::PhysicalDeviceSelector selector { vkb_instance };
	vkb::PhysicalDevice physicalDevice = selector
										 .set_minimum_version(1, 3)
										 .set_required_features_13(features13)
										 .set_required_features_12(features12)
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
	createSwapchain(static_cast<uint32_t>(m_Width), static_cast<uint32_t>(m_Height));
	
	VkExtent3D drawImageExtent = { m_Width, m_Height, 1 };
	
	m_DrawImage.imageFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
	m_DrawImage.imageExtent = drawImageExtent;
	
	VkImageUsageFlags drawImageUsages{};
	drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	drawImageUsages |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	drawImageUsages |= VK_IMAGE_USAGE_STORAGE_BIT;
	drawImageUsages |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	
	VkImageCreateInfo rimg_info = vkinit::image_create_info(m_DrawImage.imageFormat, drawImageUsages, drawImageExtent);

	VmaAllocationCreateInfo rimg_allocinfo = {};
	rimg_allocinfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	rimg_allocinfo.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	
	vmaCreateImage(m_Allocator, &rimg_info, &rimg_allocinfo, &m_DrawImage.image, &m_DrawImage.allocation, nullptr);

	VkImageViewCreateInfo rview_info = vkinit::imageview_create_info(m_DrawImage.imageFormat, m_DrawImage.image, VK_IMAGE_ASPECT_COLOR_BIT);
	
	VK_CHECK(vkCreateImageView(m_Device, &rview_info, nullptr, &m_DrawImage.imageView));
	
	m_DeletionQueue.push([&]() {
		vkDestroyImageView(m_Device, m_DrawImage.imageView, nullptr);
		vmaDestroyImage(m_Allocator, m_DrawImage.image, m_DrawImage.allocation);	
	});
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
}

void Application::initSyncStructures() {
	VkFenceCreateInfo fenceCreateInfo = vkinit::fence_create_info(VK_FENCE_CREATE_SIGNALED_BIT);
	VkSemaphoreCreateInfo semaphoreCreateInfo = vkinit::semaphore_create_info();
	
	for(int i = 0; i < FRAME_OVERLAP; i++) {
		VK_CHECK(vkCreateFence(m_Device, &fenceCreateInfo, nullptr, &m_Frames[i].renderFence));
		
		VK_CHECK(vkCreateSemaphore(m_Device, &semaphoreCreateInfo, nullptr, &m_Frames[i].renderSemaphore));
		VK_CHECK(vkCreateSemaphore(m_Device, &semaphoreCreateInfo, nullptr, &m_Frames[i].swapchainSemaphore));
	}
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
void Application::destroySwapchain() {
	vkDestroySwapchainKHR(m_Device, m_Swapchain, nullptr);
	
	for(int i = 0; i < m_SwapchainImageViews.size(); i++) {
		vkDestroyImageView(m_Device, m_SwapchainImageViews[i], nullptr);
	}
}

void Application::clearImage(VkCommandBuffer cmd) {
	VkClearColorValue clearValue = {{0.0f, 0.0f, 0.0f, 1.0f}};
	
	VkImageSubresourceRange clearRange = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);
	
	vkCmdClearColorImage(cmd, m_DrawImage.image, VK_IMAGE_LAYOUT_GENERAL, &clearValue, 1, &clearRange);
}

void Application::run() {
	bool quit = false;
	
	SDL_Event event;
	
	// SDL_SetWindowRelativeMouseMode(m_pWindow, true); // lock the mouse, set to false to unlock
	Uint64 lastTime = SDL_GetTicks();
	
	while(!quit) {
		Uint64 now = SDL_GetTicks();
		float deltatime = (now - lastTime) / 1000.0f;
		lastTime = now;
		
		while(SDL_PollEvent(&event)) {
			if(event.type == SDL_EVENT_QUIT) quit = true;
			if(event.type == SDL_EVENT_WINDOW_RESIZED) {
				int w,h;
				SDL_GetWindowSizeInPixels(m_pWindow, &w, &h);
				
				m_Width = w;
				m_Height = h;
				
			}
		}
		draw();
	}
}

void Application::draw() {
	// Wait for the GPU to finish, reset the fence
	// 1000000000 is 1 second in nanoseconds. If the CPU has been stalling for more than 1000000000 nanoseconds, it tells the GPU to go fuck itself and then goes on anyways
	VK_CHECK(vkWaitForFences(m_Device, 1, &getCurrentFrame().renderFence, true, 1000000000));
	
	getCurrentFrame().deletionQueue.flush();
	
	VK_CHECK(vkResetFences(m_Device, 1, &getCurrentFrame().renderFence));
	
	// Get a swapchain index for the next frame
	uint32_t swapchainImageIndex;
	// If the swapchain is full, it'll wait up to a second
	VK_CHECK(vkAcquireNextImageKHR(m_Device, m_Swapchain, 1000000000, getCurrentFrame().swapchainSemaphore, nullptr, &swapchainImageIndex));
	
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
	
	// END DRAW COMMANDS

	vkutil::transition_image(cmd, m_DrawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	vkutil::transition_image(cmd, m_SwapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
	
	vkutil::copy_image_to_image(cmd, m_DrawImage.image, m_SwapchainImages[swapchainImageIndex], m_DrawExtent, m_SwapchainExtent);
	
	vkutil::transition_image(cmd, m_SwapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
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
	
	VK_CHECK(vkQueuePresentKHR(m_GraphicsQueue, &presentInfo));
	
	m_FrameNumber++;
}