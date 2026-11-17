#include "application.hpp"
#include <atomic>
#include <cstring>
#include <thread>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

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

void Application::runWhileLoading(const std::function<void()>& work, bool seedScreen) {
	std::atomic<bool> done = false;
	std::exception_ptr failure;
	std::thread worker([&]() {
		try { work(); } catch(...) { failure = std::current_exception(); }
		m_PreloadDone = true;
		done = true;
	});
	bool asking = seedScreen;
	while(!done) {
		SDL_Event event;
		while(SDL_PollEvent(&event)) {
			if(asking) ImGui_ImplSDL3_ProcessEvent(&event);
			if(event.type == SDL_EVENT_QUIT) m_QuitRequested = true; // quits once loading is done (run)
			handleWindowEvent(event); // (F11 works here too)
		}
		// the screen follows the window right away: the render targets wait for the main loop (m_Resized stays set),
		// but a swapchain of the old size put the seed prompt (laid out for the new size) off centre or out of view
		if(m_Resized) resizeLoadingSwapchain();
		if(asking && (seedScreenFrame() || m_QuitRequested)) {
			// (closed on the prompt: the default world, so the worker can finish and the app quit)
			m_SeedText = m_QuitRequested ? "0" : m_SeedScreen.text;
			applySeed();
			m_SeedChosen = true;
			asking = false;
		}
		if(!asking) drawLoadingFrame();
		SDL_Delay(8);
	}
	worker.join();
	if(failure) std::rethrow_exception(failure);
}

void Application::resizeLoadingSwapchain() {
	int w, h;
	SDL_GetWindowSizeInPixels(m_pWindow, &w, &h);
	if(w == 0 || h == 0) return; // minimized
	uint32_t width = static_cast<uint32_t>(w * m_RenderScale), height = static_cast<uint32_t>(h * m_RenderScale); // (as resizeSwapchain)
	if(width == m_SwapchainExtent.width && height == m_SwapchainExtent.height) return;
	// the worker keeps loading meanwhile: the queue lock holds off its submits, idling the queue finishes our last
	// frame and its present before the swapchain goes
	std::lock_guard<std::mutex> lock(m_QueueMutex);
	VK_CHECK(vkQueueWaitIdle(m_GraphicsQueue));
	vkDestroySwapchainKHR(m_Device, m_Swapchain, nullptr);
	for(VkImageView view : m_SwapchainImageViews) vkDestroyImageView(m_Device, view, nullptr);
	for(VkSemaphore semaphore : m_RenderSemaphores) vkDestroySemaphore(m_Device, semaphore, nullptr);
	m_RenderSemaphores.clear();
	createSwapchain(width, height, m_Vsync);
}

// ---------------------------------------------------------------- the world seed

void Application::applySeed() {
	// a number is the seed itself, anything else is hashed (FNV-1a): "my world" works as well as 12345
	std::string text = m_SeedText;
	text.erase(0, text.find_first_not_of(" \t"));
	text.erase(text.find_last_not_of(" \t") + 1);
	bool number = !text.empty() && text.size() <= 10 && text.find_first_not_of("0123456789") == std::string::npos;
	if(number) m_Seed = static_cast<uint32_t>(std::stoull(text) & 0xffffffffull);
	else {
		uint32_t h = 2166136261u;
		for(unsigned char c : text) h = (h ^ c) * 16777619u;
		m_Seed = h;
	}
	// where in the noise the world is: 0 the original, any other seed somewhere else within +-40000 texels (even, so
	// the half resolution climate map's texels line up the same way). further out the noise loses precision
	glm::ivec2 offset(0);
	if(m_Seed != 0) {
		uint64_t z = static_cast<uint64_t>(m_Seed) + 0x9e3779b97f4a7c15ull; // splitmix64
		auto next = [&]() {
			z += 0x9e3779b97f4a7c15ull;
			uint64_t x = z;
			x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull;
			x = (x ^ (x >> 27)) * 0x94d049bb133111ebull;
			return x ^ (x >> 31);
		};
		offset = glm::ivec2(static_cast<int>(next() % 40000u) * 2 - 40000, static_cast<int>(next() % 40000u) * 2 - 40000);
	}
	m_HeightmapPC.seedOffset = offset;
	fmt::print("world seed: {} ({}) -> noise offset {}, {}\n", m_Seed, text.empty() ? "0" : text, offset.x, offset.y);
}

bool Application::prepareSeed() {
	m_SeedChosen = false;
	m_PreloadDone = false;
	// VULKAN2_SEED=<seed>: no screen. captures (VULKAN2_SHOT) take the default world unattended, unless
	// VULKAN2_SEED_AUTOSTART=<frames> asks for the screen for that many frames, Start pressed by itself (testing it)
	const char* autostart = std::getenv("VULKAN2_SEED_AUTOSTART");
	const char* env = std::getenv("VULKAN2_SEED");
	if(env || (std::getenv("VULKAN2_SHOT") && !autostart)) {
		if(env) m_SeedText = env;
		applySeed();
		m_SeedChosen = true;
		return false;
	}
	std::strncpy(m_SeedScreen.text, m_SeedText.c_str(), sizeof(m_SeedScreen.text) - 1);
	m_SeedScreen.random = static_cast<uint32_t>(SDL_GetPerformanceCounter());
	m_SeedScreen.framesLeft = autostart ? std::atoi(autostart) : -1;
	return true;
}

void Application::waitForSeed() {
	m_PreloadDone = true; // (the prompt says so)
	auto seconds = [&]() { return static_cast<double>(SDL_GetPerformanceCounter() - m_Loading.start) / static_cast<double>(SDL_GetPerformanceFrequency()); };
	double loaded = seconds();
	while(!m_SeedChosen) std::this_thread::sleep_for(std::chrono::milliseconds(5));
	fmt::print("loading: textures + models ready after {:.1f}s, the seed after {:.1f}s\n", loaded, seconds());
}

bool Application::seedScreenFrame() {
	SeedScreen& screen = m_SeedScreen;
	bool start = screen.framesLeft >= 0 && screen.framesLeft-- == 0;
	ImGui_ImplVulkan_NewFrame();
	ImGui_ImplSDL3_NewFrame();
	ImGui::NewFrame();
	ImGuiIO& io = ImGui::GetIO();
	ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
	ImGui::Begin("New world", nullptr, ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse
									   | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
	ImGui::TextUnformatted("World seed");
	ImGui::SetNextItemWidth(260.0f);
	if(screen.focus) { ImGui::SetKeyboardFocusHere(); screen.focus = false; }
	bool enter = ImGui::InputText("##seed", screen.text, sizeof(screen.text), ImGuiInputTextFlags_EnterReturnsTrue);
	ImGui::TextDisabled("a number or any text, 0 is the default world");
	if(ImGui::Button("Default")) { std::strncpy(screen.text, "0", sizeof(screen.text)); screen.focus = true; }
	ImGui::SameLine();
	if(ImGui::Button("Random")) {
		screen.random = screen.random * 1664525u + 1013904223u;
		std::snprintf(screen.text, sizeof(screen.text), "%u", screen.random % 1000000000u);
		screen.focus = true;
	}
	ImGui::SameLine();
	if(ImGui::Button("Start") || enter) start = true;
	// what's loading behind it
	if(m_PreloadDone) ImGui::TextDisabled("loaded, the terrain is generated on Start");
	else {
		static const char* dots[4] = { "", ".", "..", "..." };
		int t = static_cast<int>(static_cast<float>(SDL_GetPerformanceCounter() - m_Loading.start) / static_cast<float>(SDL_GetPerformanceFrequency()) * 3.0f) % 4;
		ImGui::TextDisabled("loading textures and models%s", dots[t]);
	}
	ImGui::End();
	ImGui::Render();
	drawLoadingFrame(true);
	return start;
}

void Application::drawLoadingFrame(bool seedScreen) {
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
							 seedScreen ? -1.0f : static_cast<float>(SDL_GetPerformanceCounter() - m_Loading.start) / static_cast<float>(SDL_GetPerformanceFrequency()) };
	vkCmdPushConstants(cmd, m_Loading.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRendering(cmd);
	if(seedScreen) drawImGui(cmd, m_SwapchainImageViews[index]); // the seed prompt on top
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
