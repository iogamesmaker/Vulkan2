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
#include <camera.h> 			// vkguide.dev

#include "VkBootstrap.h" // saves like 100 lines of extra setup

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include "loader.hpp"

// Pretty much all of the initialisation code is based on / taken from vkguide.dev.

float test = 0.5f;
struct HeightmapPushConstants {
	glm::ivec2 offset;
	glm::ivec2 updateRegion;
	float test;
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
	
	HeightmapPushConstants data;
};

struct CompiledShader {
	VkShaderModule shader;
	VkPipelineShaderStageCreateInfo stageInfo;
};

constexpr unsigned int FRAME_OVERLAP = 2;

class Application {
public:
	Application(uint32_t width = 1280, uint32_t height = 720);
	~Application();
	
	void run();
	
	void draw();
	
	void immediateSubmit(std::function<void(VkCommandBuffer cmd)>&& function);
	
	ComputeEffect loadComputeShader(std::string path, std::string name, HeightmapPushConstants data);
	CompiledShader loadShader(std::string path, VkShaderStageFlagBits stage);

	AllocatedBuffer createBuffer(size_t size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage);
	void destroyBuffer(const AllocatedBuffer& buffer) { vmaDestroyBuffer(m_Allocator, buffer.buffer, buffer.allocation); }
	
	GPUMeshBuffers uploadMesh(std::span<uint32_t> indices, std::span<Vertex> vertices);
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
	VkExtent2D m_SwapchainExtent;
	
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
	
	VkDescriptorSet m_HeightmapDescriptors;
	VkDescriptorSetLayout m_HeightmapDescriptorLayout;
	ComputeEffect m_HeightmapEffect;
	VkDescriptorSet m_HeightmapImGuiDescriptors;
		
	VkPipeline m_MainPipeline;
	VkPipelineLayout m_MainLayout;
		
	std::vector<std::shared_ptr<MeshAsset>> testMeshes;
	
private:
	// functions
	void initWindow();

	void initVulkan();
	void initSwapchain();
	void initCommands();
	void initSyncStructures();
	
	void initDescriptors();
	
	void initPipelines();
	void initMainPipeline();
	void initComputePipelines();
	
	void initImGui();
	
	void initDefaultData();
	
	void drawImGui(VkCommandBuffer cmd, VkImageView targetImageView);
	
	void createSwapchain(uint32_t width, uint32_t height);
	void resizeSwapchain();
	void destroySwapchain();
	
	void clearImage(VkCommandBuffer cmd);
	void drawGeometry(VkCommandBuffer cmd);
	
	void generateHeightmap();

	// variables	
	uint32_t m_Width, m_Height;
	
	float m_RenderScale = 1.f;
	
	bool m_Resized = true;
	bool m_Mouselock = false;
	SDL_Window* m_pWindow;
	
	DeletionQueue m_DeletionQueue;
	VmaAllocator m_Allocator;
	
	AllocatedImage m_DrawImage;
	AllocatedImage m_DepthBuffer;
	
	AllocatedImage m_Heightmap;
	
	VkExtent2D m_DrawExtent;
	
	VkSampler m_Sampler;
	VkSampler m_HeightmapSampler;
	
	Camera m_Camera;
	
	int meshIndex = 0;
};
#endif