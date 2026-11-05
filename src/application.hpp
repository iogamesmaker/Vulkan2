// Application.hpp
#ifndef APPLICATION_HPP
#define APPLICATION_HPP

#include <iostream>
#include <string>
#include <vector>
#include <stdexcept>
#include <deque>
#include <functional>
#include <memory>
#include <span>
#include <mutex>

#include "utils.hpp"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <vk_types.h> 			// vkguide.dev
#include <vk_initializers.h> 	// vkguide.dev
#include <vk_images.h> 			// vkguide.dev
#include <vk_descriptors.h>		// guess lol
#include <vk_pipelines.h>		// vkguide.dev
#include <camera.h> 			// vkguide.dev

#include "VkBootstrap.h" 		// saves like 100 lines of extra setup

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

#include "textures.hpp"
#include "atmosphere.hpp"
#include "shadows.hpp"
#include "foliage.hpp"
#include "trees.hpp"
#include "gbuffer.hpp"
#include "bloom.hpp"
#include "props.hpp"
#include "groundcover.hpp"
#include "collision.hpp"
#include "settings.hpp"
#include "hiz.hpp"
#include "terraincache.hpp"
#include "debugview.hpp"
#include "interaction.hpp"
#include "precipitation.hpp"
#include "weather.hpp"

// Pretty much all of the initialisation code is based on / taken from vkguide.dev.

struct HeightmapPushConstants {
	glm::ivec2 offset;
	glm::ivec2 dirtyMin;
	glm::ivec2 updateSize;
};

struct TessellationPushConstants {
	glm::mat4 viewproj;
	glm::vec3 campos;
	float tessellationFactor = 5.f; // 8px triangles: smaller ones waste most of the (expensive) terrain shading on 2x2 quads
	glm::vec3 sundir;
	float factor = 1.0f;
	glm::ivec2 worldoffset;
	float projScale;	   // screen height / 2 * projection[1][1], for screen space LOD
	float worldRadius;	   // the loaded world is a circle of this radius around the camera
	float fixedTessLevel = 0.0f; // > 0 in the shadow pass, overrides the camera based LOD
	float time;					 // seconds, the wind over the wheat canopy (terrain.frag)
	glm::vec2 detailOffset;		 // map offset in world units modulo TERRAIN_DETAIL_PERIOD (terrainmaterial.glsl)
};

struct DeletionQueue { // high IQ play by the author of vkguide.dev
					   // they tell us this implementation isn't really scalable though
					   // oh well works good enough
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
	
	VkSemaphore swapchainSemaphore; // render finished semaphores are per swapchain image, see m_RenderSemaphores
	VkFence renderFence;
	
	DeletionQueue deletionQueue;
};

struct CompiledShader {
	VkShaderModule shader;
	VkPipelineShaderStageCreateInfo stageInfo;
};

constexpr unsigned int FRAME_OVERLAP = 2;
// world units of height per unit of heightmap height, per world unit of map width: how tall the terrain is for
// how wide it is (was 0.175, mountains and dunes came out too steep for their size)
constexpr float TERRAIN_HEIGHT_SCALE = 0.12f;

constexpr int BIOME_ROCK_INDEX = 2; // BIOME_MOUNTAINS, the rock material (biometools.glsl / loadTextures)
constexpr uint32_t TERRAIN_MATERIALS = 10; // 6 biomes, 3 rock kinds, the cliff photo (terrainmaterial.glsl rockCover)

class Application {
public:
	Application(uint32_t width = 1280, uint32_t height = 720);
	~Application();
	
	void run();
	
	void draw();
	
	void immediateSubmit(std::function<void(VkCommandBuffer cmd)>&& function);
	std::mutex m_QueueMutex; // the graphics queue is shared with the loading screen while the worker loads (loading.cpp)
	
	ComputeEffect loadComputeShader(std::string path, std::string name, VkPipelineLayout layout);
	CompiledShader loadShader(std::string path, VkShaderStageFlagBits stage);

	AllocatedBuffer createBuffer(size_t size, VkBufferUsageFlags usage, VmaMemoryUsage memoryUsage);
	VmaAllocator getAllocator() const { return m_Allocator; }
	void destroyBuffer(const AllocatedBuffer& buffer) { vmaDestroyBuffer(m_Allocator, buffer.buffer, buffer.allocation); }
	
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
	std::vector<VkSemaphore> m_RenderSemaphores; // one per swapchain image, the presentation engine holds on to them
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
	
	
	DescriptorAllocator g_DescriptorAllocator;
	
	VkDescriptorSet m_HeightmapDescriptors;
	VkDescriptorSetLayout m_HeightmapDescriptorLayout;
	ComputeEffect m_HeightmapEffect;
	HeightmapPushConstants m_HeightmapPC;
	
	VkPipeline m_TerrainPipeline;
	VkPipeline m_TerrainShadowPipeline; // depth only, same layout as the terrain
	VkPipeline m_TerrainDepthPipeline;	// camera depth prepass, the shading pass then runs with depth EQUAL
	VkPipelineLayout m_TerrainLayout;
	VkDescriptorSet m_TerrainDescriptors;
	VkDescriptorSetLayout m_TerrainDescriptorLayout;
	
	AllocatedImage m_DrawImage;
	// the output resolution image (the window's size): the TSR / TAA result, bloom, exposure and tonemapping run on
	// it, then it's copied to the swapchain. the draw image is the render resolution (window * render scale)
	AllocatedImage m_OutputImage;
	AllocatedImage m_DepthBuffer;
	// deferred shading (gbuffer.hpp / gbuffer.glsl), recreated with the swapchain
	AllocatedImage m_GAlbedo, m_GNormal, m_GMaterial, m_GVelocity;
	float m_PreviousTime = 0.0f; // the wind time last frame (foliage / tree motion vectors)
	
	VkSampler m_AtmosphereSampler;
	
private:
	// functions
	void initWindow();

	void initVulkan();
	void initSwapchain(bool vsync = false);
	void initCommands();
	void initSyncStructures();
	
	void initDescriptors();

	// the startup screen (loading.cpp): a spinner drawn by this thread while work runs on a worker
	struct LoadingScreen {
		VkPipelineLayout layout;
		VkPipeline pipeline;
		VkCommandPool commandPool;
		VkCommandBuffer commandBuffer;
		VkFence fence;
		VkSemaphore acquired;
		Uint64 start;
	} m_Loading{};
	bool m_QuitRequested = false; // the window was closed while loading
	void initLoadingScreen();
	void destroyLoadingScreen();
	void runWhileLoading(const std::function<void()>& work);
	void drawLoadingFrame();
	
	void initPipelines();
	void initSkyPipeline();
	void initComputePipelines();
	void initTessellationPipeline();
	
	void initTerrainPatches();
	
	void initImGui();
	
	void initDefaultData();
	
	void loadTextures();
	
	void drawImGui(VkCommandBuffer cmd, VkImageView targetImageView);
	
	void createSwapchain(uint32_t width, uint32_t height, bool vsync = true);
	void resizeSwapchain(bool vsync = true);
	void destroySwapchain();
	
	void drawTerrain(VkCommandBuffer cmd, const glm::mat4& viewproj, const ShadowCascadeDraw* shadow = nullptr);
	void drawGeometry(VkCommandBuffer cmd);

	// deferred lighting (lighting.comp): reads the G-buffer + depth, writes the draw image and the direct light
	struct LightingPushConstants {
		glm::mat4 invViewProj;
		glm::mat4 viewProj;
		glm::vec3 campos;
		float terrainSize;
		glm::vec3 sundir;
		float frame;
		glm::ivec2 mapOffset;
		glm::vec2 cloudWind; // km the clouds have drifted (cloud shadows)
		glm::vec4 effects;	 // contact shadows, light shaft strength, SSAO, bounce light
		glm::vec4 extra{ 0.0f }; // z = 1: denoise the SSAO (x, y, w unused)
	};
	static_assert(sizeof(LightingPushConstants) <= 256, "lighting push constants");
	// lighting extras, each can go for the Iris Xe
	bool m_ContactShadows = true;
	bool m_SSAO = true;
	bool m_BounceLight = true;
	// full instead of half resolution (Ultra): the images are recreated with the swapchain when these change
	bool m_FullResSSAO = false, m_FullResClouds = false;
	float m_BounceStrength = 1.0f;
	bool m_DenoiseEffects = true;
	// SSAO (ssao.comp), half resolution, recreated with the swapchain
	AllocatedImage m_SSAOImage;
	// volumetric cumulus (clouds.comp), half resolution like the SSAO, plus their noise lookup table (cloudnoise.comp)
	AllocatedImage m_CloudImage, m_CloudNoise;
	// the clouds' temporal accumulation (cloudresolve.comp): the raw march, last frame's result
	AllocatedImage m_CloudRaw, m_CloudHistory;
	VkDescriptorSetLayout m_CloudResolveLayout;
	VkDescriptorSet m_CloudResolveSet = VK_NULL_HANDLE;
	ComputeEffect m_CloudResolveEffect;
	bool m_CloudHistoryValid = false;
	// the coverage lookup table around the camera (cloudmap.comp) + where it is (a small uniform buffer)
	AllocatedImage m_CloudMap;
	AllocatedBuffer m_CloudMapInfo;
	VkDescriptorSet m_CloudMapSet = VK_NULL_HANDLE;
	ComputeEffect m_CloudMapEffect;
	struct CloudMapPushConstants { glm::ivec2 start, size; float texel; };
	glm::ivec2 m_CloudMapOrigin{ 0 }; // global texel (world km / texel) of the map's lowest corner, it wraps (cloudmap.comp)
	bool m_CloudMapValid = false;
	static constexpr float CLOUD_MAP_EXTENT = 96.0f, CLOUD_MAP_MOVE = 1.0f; // km across, km of movement before the strips moved into are redone
	VkSampler m_CloudNoiseSampler;
	VkDescriptorSetLayout m_CloudLayout;
	VkDescriptorSet m_CloudSet = VK_NULL_HANDLE;
	ComputeEffect m_CloudEffect;
	void initClouds();

	// volumetric fog (volfog.cpp, volfog_inject.comp + volfog_integrate.comp): a froxel grid fitted to the view.
	// the integrated result sits in the atmosphere's render set (binding 7) for lighting.comp
	AllocatedImage m_FogFroxels[2]{};	// rgb in-scattered light, a = extinction; ping-pong, the other is last frame's
	AllocatedImage m_FogIntegrated{}; // rgb scattered towards the camera so far, a = transmittance
	VkDescriptorSetLayout m_FogInjectLayout = VK_NULL_HANDLE, m_FogIntegrateLayout = VK_NULL_HANDLE;
	VkDescriptorSet m_FogInjectSets[2]{}, m_FogIntegrateSets[2]{};
	ComputeEffect m_FogInjectEffect, m_FogIntegrateEffect;
	glm::uvec3 m_FogGrid{ 0 };
	glm::mat4 m_FogPrevViewProj{ 1.0f };
	glm::vec3 m_FogPrevCampos{ 0.0f };
	glm::ivec2 m_FogPrevMapOffset{ 0 };
	bool m_FogHistoryValid = false;
	bool m_VolumetricFog = false;
	float m_FogDensity = 1.0f, m_FogMist = 1.0f;
	void initVolumetricFog();
	void createFogVolumes(glm::uvec3 grid); // (re)creates the volumes at this grid size, the GPU has to be idle
	void destroyFogVolumes();
	void drawVolumetricFog(VkCommandBuffer cmd);
	VkDescriptorSetLayout m_SSAOLayout;
	VkDescriptorSet m_SSAOSet = VK_NULL_HANDLE;
	ComputeEffect m_SSAOEffect;
	Bloom m_Bloom;
	Props m_Props; // rocks, boulders, fallen logs
	GroundCover m_GroundCover; // ferns, bushes, weeds, dead branches
	VkDescriptorSetLayout m_LightingLayout;
	VkDescriptorSet m_LightingSet = VK_NULL_HANDLE;
	ComputeEffect m_LightingEffect;
	VkSampler m_NearestSampler;
	void initLighting();
	void writeLightingDescriptors(); // after the targets get recreated
	void drawLighting(VkCommandBuffer cmd, uint32_t firstTimestamp); // writes firstTimestamp (after the fog), + 1 (after SSAO) and + 2 (after the clouds)

	// temporal anti-aliasing (taa.comp): the projection is jittered by a sub pixel offset every frame, the resolve
	// blends with the reprojected history. history images ping-pong, recreated with the swapchain
	struct TAAPushConstants {
		glm::mat4 reproject; // current jittered NDC -> last frame's unjittered clip space
		float blend;
		int historyValid;
		glm::vec2 jitter; // render resolution pixels
	};
	glm::vec2 m_Jitter{ 0.0f }; // this frame's (updateCamera), render resolution pixels
	bool m_TAA = true;
	AllocatedImage m_History[2];
	bool m_HistoryValid = false;
	VkDescriptorSetLayout m_TAALayout;
	VkDescriptorSet m_TAASets[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE }; // [i] writes history i, reads the other
	ComputeEffect m_TAAEffect;
	// sharpening after the resolve (sharpen.comp), reads history i, writes the draw image
	float m_Sharpness = 0.5f;
	VkDescriptorSetLayout m_SharpenLayout;
	VkDescriptorSet m_SharpenSets[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
	ComputeEffect m_SharpenEffect;
	glm::mat4 m_UnjitteredProjection{1.0f};
	glm::mat4 m_PreviousViewProj{1.0f}; // last frame's camera (unjittered), in this frame's terrain space
	glm::vec3 m_PreviousCameraPosition{0.0f};
	float m_PreviousYaw = 0.0f, m_PreviousPitch = 0.0f;
	glm::mat4 m_PreviousProjection{ 0.0f }; // unjittered, last frame's: the zoom changes it (0 = none yet)
	void initTAA();
	void writeTAADescriptors();
	void drawTAA(VkCommandBuffer cmd);
	void updateCamera(); // fills the view / projection / camera position for this frame
	
	void generateHeightmap();
	// sky visibility (horizon AO) for a region of the heightmap, in heightmap texels (wraps, the ring buffer)
	void dispatchHorizonAO(VkCommandBuffer cmd, glm::ivec2 heightmapStart, glm::ivec2 heightmapSize);
	void initHorizonAO();
	void updateHeightmap(); // the strips the camera moved into
	void createHeightmapImages(); // heightmap, climate map and sky visibility
	void destroyHeightmapImages();
	void regenerateHeightmap(); // the whole map in strips, a few submits (a 6144 map in one took seconds)

	// window management things	
	uint32_t m_Width, m_Height;
	
	float m_RenderScale = 1.f;
	
	bool m_Resized = true;
	bool m_Mouselock = false;
	SDL_Window* m_pWindow;
	
	bool m_Vsync = true;
	// imgui frametime things
	static constexpr int m_FrameHistorySize = 180;
	inline static float m_Frametimes[m_FrameHistorySize] = { 0.0f };
	inline static int m_FrameOffset = 0;
	bool m_PauseFrametimes = false;

	// GPU time per pass, from timestamp queries (one set per frame in flight, read back after its fence)
	// timestamps inside a render pass come out as ~0 on Intel, so for the opaque breakdown the pass gets split
	// into one rendering per part (see drawGeometry, m_DetailedOpaqueTimings)
	static constexpr uint32_t GPU_TIMESTAMPS = 18;
	static constexpr const char* GPU_PASS_NAMES[GPU_TIMESTAMPS - 1] = {
		"atmosphere LUTs", "foliage, trees, terrain cache", "shadow maps",
		"terrain", "tree depth prepass", "trees", "tree impostors", "grass, ground cover, rocks", "sky", "volumetric fog", "SSAO", "volumetric clouds",
		"deferred lighting", "shadow filter", "TAA / upscale", "bloom + post", "blit + ui"
	};
	static constexpr uint32_t GPU_OPAQUE_FIRST = 3, GPU_OPAQUE_LAST = 13; // intervals that make up the opaque pass
	static constexpr uint32_t GPU_SHADOW_INTERVAL = 2;
	bool m_DetailedOpaqueTimings = true; // splitting the pass costs a little (attachments get stored + loaded again)
	uint32_t m_ShadowCascadesRendered[FRAME_OVERLAP] = {}; // per frame slot, for the shadow timer
	float m_ShadowRenderMs = 0.0f; // smoothed over the frames that re-rendered cascades only
	float m_ShadowRendersPerSecond = 0.0f;
	VkQueryPool m_TimestampPool = VK_NULL_HANDLE;
	float m_TimestampPeriod = 1.0f; // ns per tick
	bool m_TimestampsWritten[FRAME_OVERLAP] = {};
	float m_GpuPassTimes[GPU_TIMESTAMPS - 1] = {}; // ms, smoothed
	void timestamp(VkCommandBuffer cmd, uint32_t index);

	// VULKAN2_SHOT="x,z,height,yaw,pitch,hour,file.png;..." renders each shot from a fixed camera (height above
	// the ground, angles in radians, hour of the day), lets it settle, writes the image + the GPU timers, then quits
	struct Shot { float x, z, height, yaw, pitch, hour; std::string file; };
	std::vector<Shot> m_Shots;
	size_t m_ShotIndex = 0;
	int m_ShotFrames = 0;
	bool m_CaptureThisFrame = false;
	glm::vec4 m_LastProbe{0.0f}; // ground height + push, printed with the captures
	AllocatedBuffer m_CaptureBuffer{};
	void parseShots();
	bool updateShots(); // false once every shot is written
	void writeCapture();
	// rest 
	
	DeletionQueue m_DeletionQueue;
	VmaAllocator m_Allocator;
	bool m_MemoryBudgetExtension = false; // VK_EXT_memory_budget: real usage / budget numbers, VMA estimates without it
	struct GpuMemory { double used, available, ours; }; // bytes, the device local heaps
	GpuMemory gpuMemory() const; // settings.cpp
	void drawMemoryBudget();	 // settings.cpp
	
	std::vector<AllocatedImage> m_AlbedoMaps;
	std::vector<AllocatedImage> m_NormalMaps;
	std::vector<AllocatedImage> m_HeightMaps;
	
	AllocatedImage m_Heightmap;		  // R16 height, its view puts it in .a (createHeightmapImages)
	VkImageView m_HeightmapStorageView; // the plain view heightmap.comp writes through
	AllocatedImage m_ClimateMap;	  // RGBA8, half resolution: rgb = temperature, moisture, ruggedness
	AllocatedImage m_SkyVisibility; // RG16F, half the heightmap's resolution: r = horizon AO, g = forest canopy (horizonao.comp)
	VkDescriptorSetLayout m_SkyVisibilityLayout;
	VkDescriptorSet m_SkyVisibilitySet;
	ComputeEffect m_SkyVisibilityEffect;
	bool m_SkyVisibilityReady = false; // false until the first dispatch moved it out of UNDEFINED
	
	VkExtent2D m_DrawExtent;
	
	VkSampler m_HeightmapSampler;
	VkSampler m_Sampler;
	
	Camera m_Camera{};
	glm::ivec2 m_MapOffset{};
	
	std::vector<Vertex> m_PatchVertices;
	AllocatedBuffer m_TerrainVertexBuffer;
	VkDeviceAddress m_TerrainVertexAddress;
	
	TessellationPushConstants m_TerrainPC;
	
	glm::mat4 m_View{1.0f};
	glm::mat4 m_Projection{1.0f};
	glm::vec3 m_CameraPos{};
	glm::vec3 m_PlayerPos{}; // terrain space
	glm::vec3 m_SunDir{0.0f, 1.0f, 0.0f};
	
	TextureLoader m_TextureLoader{};
	Atmosphere m_AtmosphereManager{};
	ShadowMaps m_Shadows{};
	Foliage m_Foliage{};
	PlayerTrail m_PlayerTrail{}; // where your feet went: grass and low leaves bend away (interaction.glsl)
	Weather m_Weather; // weather.hpp: rain, storms, snow, sandstorms
	void updateWeather(float dt); // every frame (settings.cpp)
	Precipitation m_Precipitation; // what falls, precipitation.hpp
	void drawPrecipitation(VkCommandBuffer cmd); // settings.cpp
	bool m_GrassInteraction = true;	   // settings: the trail above
	bool m_OcclusionCulling = true;	   // settings: trees behind hills skipped (the depth pyramid)
	bool m_TerrainCacheEnabled = true; // settings: the far terrain cache is kept up to date and read
	Trees m_Trees{};
	ShadowFilter m_ShadowFilter;
	AllocatedImage m_MoonTexture; // NASA SVS CGI Moon Kit (LRO colour mosaic), public domain
	AllocatedImage m_CirrusNoise; // the cirrus' value noise lattice (volclouds.glsl), hashes baked so the sky doesn't compute them
	AllocatedImage m_BladeAtlas;  // grass blades (ambientCG Foliage001), colour + opacity
	AllocatedImage m_FlowerAtlas; // daisies (ambientCG FlowerSet001)
	AllocatedImage m_WheatAtlas;  // the grain fields, oat plants (ShareTextures AvenaSativa_1, CC0) repacked one per card
	float m_Time = 0.0f;		 // seconds since start, drives the wind
	float m_Deltatime = 0.016f;

	// terrain height under the camera, read back from the GPU for walking mode (one float per frame in flight)
	struct ProbePushConstants {
		glm::vec2 position;
		glm::ivec2 offset;
		float terrainSize;
		float factor;
		uint32_t slot;
		float density;
		glm::vec2 offsetWorld;
		float spacing; // terrain patch sample spacing
		float pad;
	};
	void initProbe();
	Collision m_Collision;
	PlayerTrail m_Trail; // where you walked, the vegetation bends away from it
	bool m_ShotPair = false; // VULKAN2_SHOT_PAIR: two consecutive frames per shot, animation frozen
	bool m_ShotPairAnimated = false;
	GraphicsSettings m_Settings;
	// occlusion culling: the depth pyramid built after the G-buffer, and the camera / map offset it was drawn with
	HiZ m_HiZ;
	// the far field terrain surface, baked around the camera (terrain.frag reads it past ~330m)
	TerrainCache m_TerrainCache;
	// the "debug textures" window: render targets, shadow maps, LUTs, and the layouts they're in at the end of a frame
	DebugView m_DebugView;
	// "GPU draw counts": the indirect draw buffers of the GPU driven passes copied back every frame (drawstats.hpp)
	bool m_ShowDrawStats = false;
	AllocatedBuffer m_DrawStatsReadback{}; // FRAME_OVERLAP slots of all the systems' buffers
	VkDeviceSize m_DrawStatsSlotBytes = 0;
	bool m_DrawStatsWritten[FRAME_OVERLAP] = {};
	std::vector<DrawStat> m_DrawStats;
	std::vector<DrawStatBuffer> drawStatBuffers() const;
	void recordDrawStats(VkCommandBuffer cmd);
	void readDrawStats();
	void drawDrawStatsWindow();
	std::vector<DebugTexture> debugTextures();
	bool m_HiZValid = false;
	glm::mat4 m_HiZViewProj{ 1.0f };
	glm::ivec2 m_HiZMapOffset{ 0 };
	Quality m_Quality = Quality::High;
	void drawSettings();  // settings.cpp
	void applySettings(); // pushes m_Settings into the renderer
	AllocatedBuffer m_ProbeBuffer;
	VkDescriptorSetLayout m_ProbeLayout;
	VkDescriptorSet m_ProbeSet;
	ComputeEffect m_ProbeEffect;
	bool m_ProbeWritten[FRAME_OVERLAP] = {};
	
	const int m_WorldSize = 2048;
	// render side scale of the terrain, the generated heightmap is untouched but everything is drawn this much
	// bigger (horizontally and vertically) so mountains feel like mountains
	const float m_TerrainScale = 3.0f;
	const float m_TextureRepeatNear = 1.2f; // world units per detail texture repeat up close (~7m)
	const float m_TextureRepeatFar = 6.0f;	// and further away, a multiple of the near one
	// the heightmap: 6144 texels a side (~6m each), RGBA16 unorm (height + climate, all 0..1: 65536 height steps,
	// ~7cm. half floats banded on the gentle slopes). the shaders measure texel based things in texels of a 4096 map
	// (heightmapscale.glsl), what it used to be
	const int m_HeightmapSize = 6144;
	const int m_CoordinateMultiplier = m_HeightmapSize / m_WorldSize; // heightmap texels per world grid unit (m_MapOffset is in texels)
};
#endif