#ifndef DEBUGVIEW_HPP
#define DEBUGVIEW_HPP

#include <vk_types.h>
#include <vector>

class Application;

// debug texture viewer: whichever render target / LUT is picked in its imgui window gets copied into one RGBA8
// image (debugview.comp) right before the UI is drawn, remapped into something visible (a channel, a value range,
// depth as distance, normals). imgui only ever samples that copy, so it doesn't care about depth formats, array
// layers, 3D textures or what layout the source was left in

enum class DebugTexKind { Tex2D, Array, Tex3D };

// how a source shows up by default, the window can change all of it
enum class DebugTexHint { Color, Hdr, Normal, Depth, ReverseDepth, Data, Velocity };

struct DebugTexture {
	const char* name;
	VkImageView view;
	VkImageLayout layout;		 // what it's sampled in
	DebugTexKind kind = DebugTexKind::Tex2D;
	DebugTexHint hint = DebugTexHint::Color;
	VkExtent3D extent{ 1, 1, 1 };
	uint32_t mips = 1;
	uint32_t layers = 1;
	bool depthFormat = false;	 // nearest sampling only, depth formats don't have to support linear filtering
	VkImage image = VK_NULL_HANDLE;
	VkImageLayout transitionFrom = VK_IMAGE_LAYOUT_UNDEFINED; // != UNDEFINED: moved from this into layout first (and left there)
	const char* (*layerName)(uint32_t layer) = nullptr;
	int channels = -1; // >= 0: overrides the hint's (rgb, r, g, b, a, rg)
};

class DebugView {
public:
	void init(Application* engine, VkImageView fallback2D, VkImageView fallbackArray, VkImageView fallback3D);
	void setFallbackArray(VkImageView view) { m_FallbackArray = view; } // the shadow maps were recreated
	void destroy();

	// the window (call between ImGui::NewFrame and ImGui::Render). the list is rebuilt every frame by the application
	void drawImGui(const std::vector<DebugTexture>& textures);
	// copies the picked texture, outside of rendering, right before imgui draws. does nothing while the window is closed
	void record(VkCommandBuffer cmd, const std::vector<DebugTexture>& textures, uint32_t frameNumber);

	bool m_Open = false;
	void select(int index) { m_Selected = index; } // (VULKAN2_DEBUGTEX_CYCLE)
	float m_Near = 0.02f, m_Far = 20000.0f; // the camera's, for depth as distance

private:
	void applyDefaults(const DebugTexture& texture);

	static constexpr uint32_t SIZE = 1024; // the copy's resolution, the longer side of the source fits it
	static constexpr uint32_t FRAMES = 2;  // FRAME_OVERLAP

	VkDevice m_Device;
	VmaAllocator m_Allocator;
	AllocatedImage m_Output{};
	bool m_OutputReady = false; // out of UNDEFINED
	VkSampler m_Linear, m_Nearest;
	VkDescriptorPool m_Pool;
	VkDescriptorSetLayout m_Layout;
	VkDescriptorSet m_Sets[FRAMES];
	ComputeEffect m_Effect;
	VkDescriptorSet m_ImGuiTexture;
	VkImageView m_Fallback2D, m_FallbackArray, m_Fallback3D; // always in SHADER_READ_ONLY, bound where nothing is picked

	// what's shown
	int m_Selected = 0;
	int m_Defaulted = -1; // the source the settings below were set up for
	int m_Mip = 0, m_Layer = 0;
	float m_Slice = 0.5f; // 3D: depth 0-1
	int m_Channels = 0;	  // rgb, r, g, b, a, rg
	float m_Min = 0.0f, m_Max = 1.0f;
	bool m_Signed = false, m_Gamma = true, m_Invert = false, m_DepthDistance = false, m_Filter = true;
	glm::uvec2 m_Region{ SIZE, SIZE }; // the part of the copy that was written
};

#endif
