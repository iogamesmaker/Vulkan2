#ifndef GBUFFER_HPP
#define GBUFFER_HPP

#include <vk_types.h>
#include <vk_pipelines.h>

// deferred shading targets, see shaders/gbuffer.glsl for what's in them
constexpr VkFormat GBUFFER_ALBEDO = VK_FORMAT_R8G8B8A8_SRGB;		  // albedo + ambient occlusion
constexpr VkFormat GBUFFER_NORMAL = VK_FORMAT_R16G16B16A16_SFLOAT;  // world space normal
constexpr VkFormat GBUFFER_MATERIAL = VK_FORMAT_R8G8B8A8_UNORM;	  // material id, roughness, direct light scale
constexpr VkFormat GBUFFER_VELOCITY = VK_FORMAT_R16G16_SFLOAT;	  // screen motion of animated geometry (TAA)

// the four G-buffer targets on a pipeline. write = false for the depth prepasses (same targets bound, untouched)
inline void setGBufferTargets(PipelineBuilder& builder, bool write) {
	const VkColorComponentFlags all = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
	builder.set_color_attachment_format(GBUFFER_ALBEDO);
	if(!write) builder._colorBlendAttachment.colorWriteMask = 0;
	builder.add_color_attachment(GBUFFER_NORMAL, write ? all : 0);
	builder.add_color_attachment(GBUFFER_MATERIAL, write ? all : 0);
	builder.add_color_attachment(GBUFFER_VELOCITY, write ? all : 0); // the same mask on every target (no independentBlend)
}

#endif
