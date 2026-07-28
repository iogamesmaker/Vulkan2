// Application.cpp
#define VMA_IMPLEMENTATION
#include "vk_mem_alloc.h" // VMA my goat
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
	
	initImGui();
	
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
		{ VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 }
	};
	
	g_DescriptorAllocator.init_pool(m_Device, 10, sizes); // 10 sets, 1 image per set
	
	{
		DescriptorLayoutBuilder builder;
		builder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE);
		m_DrawImageDescriptorLayout = builder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
	}
	
	m_DrawImageDescriptors = g_DescriptorAllocator.allocate(m_Device, m_DrawImageDescriptorLayout);
	
	VkDescriptorImageInfo imgInfo{};
	imgInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
	imgInfo.imageView = m_DrawImage.imageView;
	
	VkWriteDescriptorSet drawImageWrite = {};
	drawImageWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
	drawImageWrite.pNext = nullptr;
	
	drawImageWrite.dstBinding = 0;
	drawImageWrite.dstSet = m_DrawImageDescriptors;
	drawImageWrite.descriptorCount = 1;
	drawImageWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	drawImageWrite.pImageInfo = &imgInfo;
	
	vkUpdateDescriptorSets(m_Device, 1, &drawImageWrite, 0, nullptr);
	
	m_DeletionQueue.push([&]() {
		g_DescriptorAllocator.destroy_pool(m_Device);
		vkDestroyDescriptorSetLayout(m_Device, m_DrawImageDescriptorLayout, nullptr);
	});
}

ComputeEffect Application::loadComputeShader(std::string path, std::string name, ComputePushConstants data) {
	VkShaderModule shader;
	std::string shaderPath = util::getpath(path);
	
	if(!vkutil::load_shader_module(shaderPath.c_str(), m_Device, &shader)) { fmt::print("Failed to create shader {} at {}\n", name, shaderPath); }
	
	VkPipelineShaderStageCreateInfo stageinfo{};
	stageinfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stageinfo.pNext = nullptr;
	stageinfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
	stageinfo.module = shader;
	stageinfo.pName = "main";
	
	VkComputePipelineCreateInfo computePipelineCreateInfo{};
	computePipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
	computePipelineCreateInfo.pNext = nullptr;
	computePipelineCreateInfo.layout = m_ComputeLayout;
	computePipelineCreateInfo.stage = stageinfo;
	
	ComputeEffect computeEffect; // 99 iq naming
	computeEffect.layout = m_ComputeLayout;
	computeEffect.name = name;
	computeEffect.data = data;
	
	VK_CHECK(vkCreateComputePipelines(m_Device, VK_NULL_HANDLE, 1, &computePipelineCreateInfo, nullptr, &computeEffect.pipeline));

	vkDestroyShaderModule(m_Device, shader, nullptr);
	
	VkPipeline pipeline = computeEffect.pipeline;
	m_DeletionQueue.push([this, pipeline]() {
		vkDestroyPipeline(m_Device, pipeline, nullptr);
	});

	return computeEffect;
}

void Application::initBackgroundPipelines() {
	// set up compute shader shit
	VkPipelineLayoutCreateInfo computeLayout{};
	
	computeLayout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
	computeLayout.pNext = nullptr;
	computeLayout.pSetLayouts = &m_DrawImageDescriptorLayout;
	computeLayout.setLayoutCount = 1;
	
	VkPushConstantRange pushConstant{};
	
	pushConstant.offset = 0;
	pushConstant.size = sizeof(ComputePushConstants);
	pushConstant.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	
	computeLayout.pPushConstantRanges = &pushConstant;
	computeLayout.pushConstantRangeCount = 1;
	
	VK_CHECK(vkCreatePipelineLayout(m_Device, &computeLayout, nullptr, &m_ComputeLayout));
	// load shaders
	
	ComputePushConstants pc{};
	pc.data1 = glm::vec4(1,0,0,1);
	pc.data2 = glm::vec4(0,0,1,1);
	
	shaders.push_back(loadComputeShader( "src/shaders/bin/gradient.comp.spv", "gradient", pc));
	pc.data1 = glm::vec4(0.1,0.2,0.4,0.97);
	shaders.push_back(loadComputeShader("src/shaders/bin/gradient2.comp.spv", "sky", pc));
	
	
	m_DeletionQueue.push([&]() {
		vkDestroyPipelineLayout(m_Device, m_ComputeLayout, nullptr);
	});
}

void Application::initPipelines() {	
	initBackgroundPipelines();
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
	
	m_DeletionQueue.push([&]() {
		ImGui_ImplVulkan_Shutdown();
		vkDestroyDescriptorPool(m_Device, imguiPool, nullptr);
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
			ImGui_ImplSDL3_ProcessEvent(&event);
		}
		// imgui new frame
		ImGui_ImplVulkan_NewFrame();
		ImGui_ImplSDL3_NewFrame();

		//some imgui UI
		ImGui::NewFrame();
		
		if (ImGui::Begin("background")) {
			
			ComputeEffect& selected = shaders[m_CurrentShader];
		
			//ImGui::Text("Selected effect: ", selected.name);
		
			ImGui::SliderInt("Effect Index", &m_CurrentShader,0, shaders.size() - 1);
		
			ImGui::InputFloat4("data1",(float*)& selected.data.data1);
			ImGui::InputFloat4("data2",(float*)& selected.data.data2);
			ImGui::InputFloat4("data3",(float*)& selected.data.data3);
			ImGui::InputFloat4("data4",(float*)& selected.data.data4);
		}
		ImGui::End();

		ImGui::Render();
		
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
	
	ComputeEffect effect = shaders[m_CurrentShader];
	
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, effect.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_ComputeLayout, 0, 1, &m_DrawImageDescriptors, 0, nullptr);
	
	// Push the constants to shader
	
	vkCmdPushConstants(cmd, m_ComputeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ComputePushConstants), &effect.data);
	
	vkCmdDispatch(cmd, std::ceil(m_DrawExtent.width / 16.0f), std::ceil(m_DrawExtent.height / 16.0f), 1);
	
	// END DRAW COMMANDS

	// set the draw image to read mode
	vkutil::transition_image(cmd, m_DrawImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
	// set the swapchain to write mode
	vkutil::transition_image(cmd, m_SwapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	vkutil::copy_image_to_image(cmd, m_DrawImage.image, m_SwapchainImages[swapchainImageIndex], m_DrawExtent, m_SwapchainExtent);

	vkutil::transition_image(cmd, m_SwapchainImages[swapchainImageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);

	drawImGui(cmd, m_SwapchainImageViews[swapchainImageIndex]);
		
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
	
	VK_CHECK(vkQueuePresentKHR(m_GraphicsQueue, &presentInfo));
	
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