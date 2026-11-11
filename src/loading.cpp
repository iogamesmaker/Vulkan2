#include "application.hpp"
#include <atomic>
#include <thread>

// the startup screen. everything after the window, device and swapchain (textures, pipelines, the heightmap) loads on
// a worker thread while this one keeps pumping the window's messages, which Windows otherwise flags as "not
// responding" after a few seconds, and draws a spinner on dark grey (loading.frag) whenever the GPU has a moment.
// the worker's only queue use is immediateSubmit, both sides take m_QueueMutex around theirs

struct LoadingPushConstants {
	glm::vec2 size;
	float time;
};

void Application::initLoadingScreen() {
	CompiledShader vertex = loadShader("src/shaders/bin/fullscreen.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
	CompiledShader fragment = loadShader("src/shaders/bin/loading.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
	VkPushConstantRange range{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(LoadingPushConstants) };
	VkPipelineLayoutCreateInfo layoutCI = vkinit::pipeline_layout_create_info();
	layoutCI.pushConstantRangeCount = 1;
	layoutCI.pPushConstantRanges = &range;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &layoutCI, nullptr, &m_Loading.layout));

	PipelineBuilder builder;
	builder._pipelineLayout = m_Loading.layout;
	builder.set_shaders(vertex.shader, fragment.shader);
	builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
	builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	builder.set_multisampling_none();
	builder.disable_blending();
	builder.disable_depthtest();
	builder.set_color_attachment_format(m_SwapchainFormat);
	builder.set_depth_format(VK_FORMAT_UNDEFINED);
	m_Loading.pipeline = builder.build_pipeline(m_Device);
	vkDestroyShaderModule(m_Device, vertex.shader, nullptr);
	vkDestroyShaderModule(m_Device, fragment.shader, nullptr);

	// its own command buffer, fence and acquire semaphore: the frames' are untouched until the main loop starts
	VkCommandPoolCreateInfo poolCI{ .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
	poolCI.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolCI.queueFamilyIndex = m_GraphicsQueueFamily;
	VK_CHECK(vkCreateCommandPool(m_Device, &poolCI, nullptr, &m_Loading.commandPool));
	VkCommandBufferAllocateInfo allocInfo = vkinit::command_buffer_allocate_info(m_Loading.commandPool, 1);
	VK_CHECK(vkAllocateCommandBuffers(m_Device, &allocInfo, &m_Loading.commandBuffer));
	VkFenceCreateInfo fenceCI = vkinit::fence_create_info(VK_FENCE_CREATE_SIGNALED_BIT);
	VK_CHECK(vkCreateFence(m_Device, &fenceCI, nullptr, &m_Loading.fence));
	VkSemaphoreCreateInfo semaphoreCI = vkinit::semaphore_create_info();
	VK_CHECK(vkCreateSemaphore(m_Device, &semaphoreCI, nullptr, &m_Loading.acquired));
	m_Loading.start = SDL_GetPerformanceCounter();
}

void Application::destroyLoadingScreen() {
	VK_CHECK(vkWaitForFences(m_Device, 1, &m_Loading.fence, VK_TRUE, UINT64_MAX)); // the last spinner frame
	vkDestroySemaphore(m_Device, m_Loading.acquired, nullptr);
	vkDestroyFence(m_Device, m_Loading.fence, nullptr);
	vkDestroyCommandPool(m_Device, m_Loading.commandPool, nullptr);
	vkDestroyPipeline(m_Device, m_Loading.pipeline, nullptr);
	vkDestroyPipelineLayout(m_Device, m_Loading.layout, nullptr);
}

void Application::runWhileLoading(const std::function<void()>& work) {
	std::atomic<bool> done = false;
	std::exception_ptr failure;
	std::thread worker([&]() {
		try { work(); } catch(...) { failure = std::current_exception(); }
		done = true;
	});
	while(!done) {
		SDL_Event event;
		while(SDL_PollEvent(&event)) {
			if(event.type == SDL_EVENT_QUIT) m_QuitRequested = true; // quits once loading is done (run)
			if(event.type == SDL_EVENT_WINDOW_RESIZED) m_Resized = true; // the swapchain is rebuilt by the main loop
		}
		drawLoadingFrame();
		SDL_Delay(8);
	}
	worker.join();
	if(failure) std::rethrow_exception(failure);
}

void Application::drawLoadingFrame() {
	// never block on the GPU: while it works through a big loading job (a heightmap strip) the spinner just pauses,
	// the window keeps answering
	if(vkGetFenceStatus(m_Device, m_Loading.fence) != VK_SUCCESS) return;
	if(SDL_GetWindowFlags(m_pWindow) & SDL_WINDOW_MINIMIZED) return;
	uint32_t index;
	VkResult acquired = vkAcquireNextImageKHR(m_Device, m_Swapchain, 0, m_Loading.acquired, VK_NULL_HANDLE, &index);
	if(acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) return; // none free right now, or out of date (resized)
	VK_CHECK(vkResetFences(m_Device, 1, &m_Loading.fence));

	VkCommandBuffer cmd = m_Loading.commandBuffer;
	VK_CHECK(vkResetCommandBuffer(cmd, 0));
	VkCommandBufferBeginInfo beginInfo = vkinit::command_buffer_begin_info(VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
	VK_CHECK(vkBeginCommandBuffer(cmd, &beginInfo));
	vkutil::transition_image(cmd, m_SwapchainImages[index], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	VkRenderingAttachmentInfo color = vkinit::attachment_info(m_SwapchainImageViews[index], nullptr, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
	color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE; // every pixel gets drawn
	VkRenderingInfo renderInfo = vkinit::rendering_info(m_SwapchainExtent, &color, nullptr);
	vkCmdBeginRendering(cmd, &renderInfo);
	VkViewport viewport = { 0.0f, 0.0f, static_cast<float>(m_SwapchainExtent.width), static_cast<float>(m_SwapchainExtent.height), 0.0f, 1.0f };
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	VkRect2D scissor = { { 0, 0 }, m_SwapchainExtent };
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	vkutil::bindGraphics(cmd, m_Loading.pipeline);
	LoadingPushConstants pc{ glm::vec2(m_SwapchainExtent.width, m_SwapchainExtent.height),
							 static_cast<float>(SDL_GetPerformanceCounter() - m_Loading.start) / static_cast<float>(SDL_GetPerformanceFrequency()) };
	vkCmdPushConstants(cmd, m_Loading.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRendering(cmd);
	vkutil::transition_image(cmd, m_SwapchainImages[index], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
	VK_CHECK(vkEndCommandBuffer(cmd));

	VkCommandBufferSubmitInfo cmdInfo = vkinit::command_buffer_submit_info(cmd);
	VkSemaphoreSubmitInfo waitInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT_KHR, m_Loading.acquired);
	VkSemaphoreSubmitInfo signalInfo = vkinit::semaphore_submit_info(VK_PIPELINE_STAGE_2_ALL_GRAPHICS_BIT, m_RenderSemaphores[index]);
	VkSubmitInfo2 submit = vkinit::submit_info(&cmdInfo, &signalInfo, &waitInfo);
	VkPresentInfoKHR present{ .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
	present.swapchainCount = 1;
	present.pSwapchains = &m_Swapchain;
	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores = &m_RenderSemaphores[index];
	present.pImageIndices = &index;
	std::lock_guard<std::mutex> lock(m_QueueMutex);
	VK_CHECK(vkQueueSubmit2(m_GraphicsQueue, 1, &submit, m_Loading.fence));
	vkQueuePresentKHR(m_GraphicsQueue, &present); // out of date is fine, the main loop rebuilds the swapchain
}
