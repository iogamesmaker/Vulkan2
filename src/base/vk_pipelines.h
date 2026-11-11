#pragma once

#include <vk_types.h>

class PipelineBuilder {
//> pipeline
public:
    std::vector<VkPipelineShaderStageCreateInfo> _shaderStages;
   
    VkPipelineInputAssemblyStateCreateInfo _inputAssembly;
    VkPipelineRasterizationStateCreateInfo _rasterizer;
    VkPipelineColorBlendAttachmentState _colorBlendAttachment;
    VkPipelineMultisampleStateCreateInfo _multisampling;
    VkPipelineLayout _pipelineLayout;
    VkPipelineDepthStencilStateCreateInfo _depthStencil;
    VkPipelineRenderingCreateInfo _renderInfo;
    VkFormat _colorAttachmentformat;
	VkPipelineTessellationStateCreateInfo _tessellationState;

	std::vector<VkVertexInputBindingDescription> _vertexBindings;
    std::vector<VkVertexInputAttributeDescription> _vertexAttributes;
    VkPipelineVertexInputStateCreateInfo _vertexInputInfo;

	PipelineBuilder(){ clear(); }
    void clear();

    VkPipeline build_pipeline(VkDevice device);
//< pipeline
	void set_tessellation_shaders(VkShaderModule tcs, VkShaderModule tes);
	void set_tessellation_patch(uint32_t controlPoints);
    void set_shaders(VkShaderModule vertexShader, VkShaderModule fragmentShader);
	void set_vertex_input(const std::vector<VkVertexInputBindingDescription>& bindings, const std::vector<VkVertexInputAttributeDescription>& attributes);
    void set_input_topology(VkPrimitiveTopology topology);
    void set_polygon_mode(VkPolygonMode mode);
    void set_cull_mode(VkCullModeFlags cullMode, VkFrontFace frontFace);
    void set_multisampling_none();
    void disable_blending();
    void enable_blending_additive();
    void enable_blending_alphablend();

    void set_color_attachment_format(VkFormat format);
	// another colour attachment after the first one, same blend state, writeMask 0 = left alone
	void add_color_attachment(VkFormat format, VkColorComponentFlags writeMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
	std::vector<VkFormat> _colorFormats;
	std::vector<VkColorComponentFlags> _extraWriteMasks;
	void set_depth_format(VkFormat format);
	void disable_depthtest();
    void enable_depthtest(bool depthWriteEnable,VkCompareOp op);
	// the wireframe setting: every scene geometry pipeline (drawn into the reversed z depth buffer, not a shadow
	// map) gets a line drawing twin built alongside it, bindGraphics picks it while the setting is on. a fullscreen
	// pass that happens to look like one (the sky) opts out
	bool _noWireframe = false;
};

namespace vkutil {
bool load_shader_module(const char* filePath, VkDevice device, VkShaderModule* outShaderModule);

// the wireframe setting (PipelineBuilder::build_pipeline makes the twins)
extern bool g_Wireframe;
// binds a graphics pipeline, or its wireframe twin while the setting is on
void bindGraphics(VkCommandBuffer cmd, VkPipeline pipeline);
void destroyWireframeTwins(VkDevice device);
}
