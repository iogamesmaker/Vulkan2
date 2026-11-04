#ifndef SHADOWS_HPP
#define SHADOWS_HPP

#include <vk_types.h>
#include <climits>

class Application;
struct ShadowLightKeys;

// cascaded shadow maps for the sun / moon
// rendered from the actual geometry so anything we draw later (trees, foliage) can cast shadows too.
//
// a shadow map whose light direction changes every frame re-rasterises every edge every frame, which shimmers.
// instead there are two sets of cascades, rendered for two keyframes along the sun's path (the one just behind
// the sun and the one just ahead of it), and the terrain crossfades between them. a set only gets re-rendered
// when its keyframe changes or the camera moved out of its padding, so most frames render nothing at all.
// the last two cascades only have the terrain in them (SHADOW_TERRAIN_ONLY), the last one covers the whole world, so
// far mountains still shade valleys

constexpr uint32_t SHADOW_CASCADES = 4;
constexpr uint32_t SHADOW_TERRAIN_ONLY = 2; // this cascade and the ones after it only have the terrain in them
constexpr uint32_t SHADOW_SETS = 2;
constexpr uint32_t SHADOW_FRAMES = 2; // must match FRAME_OVERLAP

// must match ShadowUBO in terrain.frag (std140)
struct ShadowUBO {
	glm::mat4 lightViewProj[SHADOW_SETS * SHADOW_CASCADES]; // layer = set * SHADOW_CASCADES + cascade
	glm::vec4 splits; // far view distance of each cascade
	glm::vec4 widths; // world size of each cascade
	glm::vec4 params; // x = filter radius (world units), y = normal offset (texels), z = enabled, w = far shadow distance
	glm::vec4 blend;  // x = weight of set B, y = shadow strength, z = set A, w = set B
	glm::vec4 depthRange[SHADOW_SETS]; // world units the depth of each layer covers, per cascade (for PCSS)
	glm::vec4 cascadeBlend; // x = fraction of each cascade's range that crossfades into the next one
};

// what one cascade's draw needs
struct ShadowCascadeDraw {
	glm::mat4 viewProj;
	glm::vec3 center; // what the cascade was rendered around
	float radius;
	float tessLevel;  // terrain tessellation within the radius, coarser outside of it
	uint32_t cascade;
};

class ShadowMaps {
public:
	void init(VkDevice device, VmaAllocator allocator, Application* engine);
	void destroy();

	// picks the keyframe sets and works out which cascades need re-rendering
	void update(const glm::mat4& view, const ShadowLightKeys& keys, float terrainHeightRange, uint32_t frameNumber);

	// renders the cascades that are due (drawScene records the shadow casters) and writes this frame's UBO.
	// the set fading in gets at most one cascade per frame so a new keyframe doesn't render 4 at once, it has
	// next to no weight then anyway. call outside of rendering. returns how many cascades were rendered
	uint32_t render(VkCommandBuffer cmd, const std::function<void(VkCommandBuffer, const ShadowCascadeDraw&)>& drawScene);

	// re-render every cascade next frame, e.g. when the world origin moved under the old matrices
	void invalidate() { m_ForceAll = true; }
	// how far the visible world reaches from the camera (the terrain's round edge): the far cascade covers that
	void setWorldRadius(float radius) { if(radius != m_FarDistance) m_ForceAll = true; m_FarDistance = radius; }
	// graphics settings
	void setQuality(bool enabled, bool soft) {
		if(enabled && !m_Enabled) m_ForceAll = true;
		m_Enabled = enabled; m_Soft = soft;
	}

	// texels per side of every cascade (1024 / 2048 / 4096). recreates the maps: the GPU has to be idle, the debug
	// viewer needs getArrayView() again after. true if it changed
	bool setResolution(uint32_t resolution);
	// how far cascades 0-2 reach, world units. cascade 1 is also as far as trees and props cast shadows
	void setNearSplits(float c0, float c1, float c2);
	// the PCSS light size multiplier (1 = the real sun): how fast penumbras widen with the distance to the caster
	void setSoftness(float lightSize) { m_FilterRadius = lightSize; }

	float getLightStep() const { return m_LightStep; }

	VkDescriptorSetLayout getDescriptorLayout() const { return m_DescriptorLayout; }
	VkDescriptorSet getDescriptor(uint32_t frameNumber) const { return m_Descriptors[frameNumber % SHADOW_FRAMES]; }
	VkFormat getFormat() const { return m_Format; }
	// the whole array, SHADER_READ_ONLY outside of render() (the debug texture viewer)
	VkImageView getArrayView() const { return m_Image.imageView; }
	uint32_t getResolution() const { return m_Resolution; }

	bool m_Enabled = true;
private:
	struct ShadowSet {
		long long key = LLONG_MIN; // keyframe this set was rendered for
		glm::vec3 lightDir{0.0f, 1.0f, 0.0f};
		glm::vec3 centers[SHADOW_CASCADES];		  // camera position each cascade gets rendered around
		float radius[SHADOW_CASCADES] = {};
		glm::mat4 lightViewProj[SHADOW_CASCADES];
		float depthRange[SHADOW_CASCADES] = { 1.0f, 1.0f, 1.0f, 1.0f };
		bool due[SHADOW_CASCADES] = { true, true, true, true };
		// what's actually in the maps (a due cascade keeps being sampled with the matrix it was rendered with)
		glm::mat4 renderedViewProj[SHADOW_CASCADES];
		float renderedDepthRange[SHADOW_CASCADES] = { 1.0f, 1.0f, 1.0f, 1.0f };
	};

	void createImage();
	void destroyImage();
	void writeImageDescriptors();
	void transitionLayer(VkCommandBuffer cmd, uint32_t layer, VkImageLayout from, VkImageLayout to);
	void computeCascade(ShadowSet& set, uint32_t cascade, glm::vec3 center, float terrainHeightRange);
	float cascadeRadius(uint32_t cascade) const;
	// the far cascade barely pads: the world moves with the camera, so anything past its edge is never seen, and
	// re-rendering only the terrain at the coarsest tessellation is cheap
	float cascadePadding(uint32_t cascade) const { return cascade == SHADOW_CASCADES - 1 ? m_FarPadding : m_Padding; }

	VkDevice m_Device = VK_NULL_HANDLE;
	VmaAllocator m_Allocator;
	Application* m_Engine = nullptr;

	VkFormat m_Format = VK_FORMAT_D16_UNORM; // plenty with these depth ranges, half the bandwidth of D32
	uint32_t m_Resolution = 2048;			 // per cascade, cheap since they're rarely re-rendered

	AllocatedImage m_Image;	// 2D array, SHADOW_SETS * SHADOW_CASCADES layers
	VkImageView m_LayerViews[SHADOW_SETS * SHADOW_CASCADES];
	VkSampler m_Sampler;	// comparison sampler, hardware 2x2 PCF
	VkSampler m_DepthSampler; // plain nearest sampler for reading the depth (PCSS blocker search)

	AllocatedBuffer m_UBOs[SHADOW_FRAMES];
	VkDescriptorSetLayout m_DescriptorLayout;
	VkDescriptorSet m_Descriptors[SHADOW_FRAMES];

	ShadowSet m_Sets[SHADOW_SETS];
	uint32_t m_SetA = 0, m_SetB = 1;
	float m_Splits[SHADOW_CASCADES];
	bool m_ForceAll = true;
	bool m_Urgent = true; // everything due gets rendered this frame (the old maps are unusable)
	ShadowUBO m_UBOData{};
	uint32_t m_FrameSlot = 0;

	// settings
	float m_NearSplits[SHADOW_CASCADES - 1] = { 14.0f, 90.0f, 500.0f }; // world units, cascades 0-2 (setNearSplits)
	float m_FarDistance = 3200.0f;	  // the terrain only cascade, out to the edge of the world (setWorldRadius)
	float m_FilterRadius = 1.5f;	  // PCSS light size multiplier, 1 = the real sun
	float m_NormalOffset = 1.5f;	  // texels
	float m_LightStep = 1.0f;		  // degrees between keyframes
	bool m_Soft = true;				  // contact hardening penumbras (PCSS) in the first cascade
	float m_CascadeBlend = 0.5f;	  // fraction of each cascade's range that crossfades into the next one
	float m_Padding = 0.25f;		  // cascades cover this much more than needed so the camera can move a bit
	float m_FarPadding = 0.04f;		  // the same for the far cascade, ~110 units at the default world size
	// per cascade, so casters don't morph as the camera moves (only depend on where the cascade was rendered)
	// 48 unit patches: 32 made cascade 0's casters 1.5 unit (9m) triangles, coarser than the heightmap's detail bumps
	// and crevices up close (they self shadowed). 64 is the hardware maximum, only the few patches within each cascade's
	// radius get the full level
	float m_CascadeTessLevels[SHADOW_CASCADES] = { 64.0f, 48.0f, 12.0f, 4.0f };
};

// the opaque pass's second colour target: sun / moon light without shadows (rgb) and the shadow term (a)
constexpr VkFormat DIRECT_LIGHT_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;

// screen space shadow filter: blurs the shadow term the opaque pass wrote with a depth aware gaussian and adds the
// shadowed light to the draw image (shadowresolve.comp). smooths out the noisy PCSS penumbras, which more shadow
// map taps per pixel would only do at a much higher cost
class ShadowFilter {
public:
	void init(Application* engine);
	void destroy();
	// (re)creates the targets for this draw image size, after the swapchain was (re)created
	void setTargets(const AllocatedImage& drawImage, const AllocatedImage& depthBuffer);

	// the draw image and depth buffer go in as attachments (after the opaque pass) and come out as attachments
	void resolve(VkCommandBuffer cmd, VkExtent2D extent, VkImage drawImage, VkImage depthBuffer);

	VkImage getDirectImage() const { return m_Direct.image; }
	VkImageView getDirectView() const { return m_Direct.imageView; }

	int m_Radius = 3; // pixels, 0 = no blur
private:
	void destroyTargets();

	VkDevice m_Device;
	VmaAllocator m_Allocator;
	AllocatedImage m_Direct{};
	AllocatedImage m_Temp{}; // horizontally blurred shadow
	VkSampler m_DepthSampler;
	VkDescriptorSetLayout m_Layout;
	VkDescriptorSet m_Set;
	VkPipelineLayout m_PipelineLayout;
	VkPipeline m_Pipeline;
};

#endif
