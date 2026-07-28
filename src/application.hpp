// Application.hpp
#ifndef APPLICATION_HPP
#define APPLICATION_HPP

#include <iostream>
#include <string>
#include <vector>
#include <stdexcept>

#include "utils.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <vk_types.h> 			// vkguide.dev
#include <vk_initializers.h> 	// vkguide.dev
#include <vk_images.h> 			// vkguide.dev
#include <vk_descriptors.h>		// guess lol
#include <vk_pipelines.h>		// vkguide.dev

#include "VkBootstrap.h" // I fucking love this header file lol, makes setting up Vulkan a breeze.
#include "vk_mem_alloc.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

// Pretty much all of the initialisation code is based on / taken from vkguide.dev.

struct ComputePushConstants {
	glm::vec4 data1;
	glm::vec4 data2;
	glm::vec4 data3;
	glm::vec4 data4;
};

struct DeletionQueue { // high IQ play by the writer of vkguide.dev
					   // they tell us this implementation isn't really scalable though
	std::deque<std::function<void()>> deletors;
	
	void push(std::function<void()>&& func) {
		deletors.push_back(func);
	}
	
	void flush() {
		for(auto it = deletors.rbegin(); it != deletors.rend(); it++) {
			(*it)();
		}
		deletors.clear();
	}
};

struct FrameData {
	VkCommandPool commandPool;
	VkCommandBuffer commandBuffer;
	
	VkSemaphore swapchainSemaphore, renderSemaphore;
	VkFence renderFence;
	
	DeletionQueue deletionQueue;
};

struct ComputeEffect {
	std::string name;
	
	VkPipeline pipeline;
	VkPipelineLayout layout;
	
	ComputePushConstants data;
};

constexpr unsigned int FRAME_OVERLAP = 2;

class Application {
public:
	Application(uint32_t width = 1280, uint32_t height = 720);
	~Application();
	
	void run();
	
	void draw();
	
	void immediateSubmit(std::function<void(VkCommandBuffer cmd)>&& function);
	
	ComputeEffect loadComputeShader(std::string path, std::string name, ComputePushConstants data);

	// vulkan shit
	// initialising and physical device shit
	VkInstance m_Instance;// Vulkan library handle
	VkDebugUtilsMessengerEXT m_Debug;// Vulkan debug output handle
	VkPhysicalDevice m_GPU;// GPU chosen as the default device
	VkDevice m_Device; // Vulkan device for commands
	VkSurfaceKHR m_Surface;// Vulkan window surface -- I think this is our render target
	// swapchain
	VkSwapchainKHR m_Swapchain;
	VkFormat m_SwapchainFormat;
	std::vector<VkImage> m_SwapchainImages;
	std::vector<VkImageView> m_SwapchainImageViews;
	VkExtent2D m_SwapchainExtent; // Resolution I guess?
	
	// imgui shit
	
	VkFence m_ImmFence;
	VkCommandBuffer m_ImmCommandBuffer;
	VkCommandPool m_ImmCommandPool;
	
	// variables / storages
	bool m_InitDone = false;
	int m_FrameNumber = 0;
	
	FrameData m_Frames[FRAME_OVERLAP];
	FrameData& getCurrentFrame() { return m_Frames[m_FrameNumber % FRAME_OVERLAP]; };
	
	VkQueue m_GraphicsQueue;
	uint32_t m_GraphicsQueueFamily{0};
	
	VkPipeline m_ComputePipeline;
	VkPipelineLayout m_ComputeLayout;
	
	DescriptorAllocator g_DescriptorAllocator;
	VkDescriptorSet m_DrawImageDescriptors;
	VkDescriptorSetLayout m_DrawImageDescriptorLayout;
	
	std::vector<ComputeEffect> shaders;
	int m_CurrentShader{0};
	
private:
	// functions
	void initWindow();

	void initVulkan();
	void initSwapchain();
	void initCommands();
	void initSyncStructures();
	
	void initDescriptors();
	
	void initPipelines();
	void initBackgroundPipelines();
	
	void initImGui();
	
	void drawImGui(VkCommandBuffer cmd, VkImageView targetImageView);
	
	void createSwapchain(uint32_t width, uint32_t height);
	void destroySwapchain();
	
	
	void clearImage(VkCommandBuffer cmd);
		
	// variables	
	uint32_t m_Width, m_Height;
	SDL_Window* m_pWindow;
	
	DeletionQueue m_DeletionQueue;
	VmaAllocator m_Allocator;
	
	AllocatedImage m_DrawImage;
	VkExtent2D m_DrawExtent;
};
#endif