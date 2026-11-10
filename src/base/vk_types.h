// vulkan_guide.h : Include file for standard system include files,
// or project specific include files.
//> intro
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <span>
#include <array>
#include <functional>
#include <deque>

#include <vulkan/vulkan.h>
#include <vulkan/vk_enum_string_helper.h>
#include <vk_mem_alloc.h>

#include <fmt/core.h>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
//< intro 



// we will add our main reusable types here
struct AllocatedImage {
    VkImage image;
    VkImageView imageView;
    VmaAllocation allocation;
    VkExtent3D imageExtent;
    VkFormat imageFormat;
};

struct AllocatedBuffer {
    VkBuffer buffer;
    VmaAllocation allocation;
    VmaAllocationInfo info;
};

struct ComputeEffect {
	std::string name;
	
	VkPipeline pipeline;
	VkPipelineLayout layout;
};

struct GPUGLTFMaterial {
    glm::vec4 colorFactors;
    glm::vec4 metal_rough_factors;
    glm::vec4 extra[14];
};

static_assert(sizeof(GPUGLTFMaterial) == 256);

struct GPUSceneData {
    glm::mat4 view;
    glm::mat4 proj;
    glm::mat4 viewproj;
    glm::vec4 ambientColor;
    glm::vec4 sunlightDirection; // w for sun power
    glm::vec4 sunlightColor;
};

//> mat_types
enum class MaterialPass :uint8_t {
    MainColor,
    Transparent,
    Other
};
struct MaterialPipeline {
	VkPipeline pipeline;
	VkPipelineLayout layout;
};

struct MaterialInstance {
    MaterialPipeline* pipeline;
    VkDescriptorSet materialSet;
    MaterialPass passType;
};
//< mat_types
//> vbuf_types
struct Vertex {

	glm::vec3 position;
	glm::vec2 uv;
	glm::vec3 normal;
	glm::vec4 color;
};

// holds the resources needed for a mesh
struct GPUMeshBuffers {

    AllocatedBuffer indexBuffer;
    AllocatedBuffer vertexBuffer;
    VkDeviceAddress vertexBufferAddress;
};

// push constants for our mesh object draws
struct GPUDrawPushConstants {
    glm::mat4 worldMatrix;
    VkDeviceAddress vertexBuffer;
};
//< vbuf_types

//> node_types
struct DrawContext;

// base class for a renderable dynamic object
class IRenderable {

    virtual void Draw(const glm::mat4& topMatrix, DrawContext& ctx) = 0;
};

// implementation of a drawable scene node.
// the scene node can hold children and will also keep a transform to propagate
// to them
struct Node : public IRenderable {

    // parent pointer must be a weak pointer to avoid circular dependencies
    std::weak_ptr<Node> parent;
    std::vector<std::shared_ptr<Node>> children;

    glm::mat4 localTransform;
    glm::mat4 worldTransform;

    void refreshTransform(const glm::mat4& parentMatrix)
    {
        worldTransform = parentMatrix * localTransform;
        for (auto c : children) {
            c->refreshTransform(worldTransform);
        }
    }

    virtual void Draw(const glm::mat4& topMatrix, DrawContext& ctx)
    {
        // draw children
        for (auto& c : children) {
            c->Draw(topMatrix, ctx);
        }
    }
};
//< node_types
//> intro
#define VK_CHECK(x)                                                     \
    do {                                                                \
        VkResult err = x;                                               \
        if (err) {                                                      \
             fmt::print("Detected Vulkan error: {}", string_VkResult(err)); \
            abort();                                                    \
        }                                                               \
    } while (0)
//< intro
// the performance window's pass switches (Application::mark): while a switched off pass is being recorded every
// draw and dispatch is dropped, the barriers, layout transitions, render pass begins / clears and copies stay, so the
// frame is still valid and you see what the image looks like without that pass. defined after the wrappers, so they
// call the real functions
namespace passdebug { inline bool g_Skip = false; }
inline void passdebugDispatch(VkCommandBuffer c, uint32_t x, uint32_t y, uint32_t z) { if(!passdebug::g_Skip) vkCmdDispatch(c, x, y, z); }
inline void passdebugDispatchIndirect(VkCommandBuffer c, VkBuffer b, VkDeviceSize o) { if(!passdebug::g_Skip) vkCmdDispatchIndirect(c, b, o); }
inline void passdebugDraw(VkCommandBuffer c, uint32_t v, uint32_t i, uint32_t fv, uint32_t fi) { if(!passdebug::g_Skip) vkCmdDraw(c, v, i, fv, fi); }
inline void passdebugDrawIndexed(VkCommandBuffer c, uint32_t n, uint32_t i, uint32_t fi, int32_t vo, uint32_t fin) { if(!passdebug::g_Skip) vkCmdDrawIndexed(c, n, i, fi, vo, fin); }
inline void passdebugDrawIndirect(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) { if(!passdebug::g_Skip) vkCmdDrawIndirect(c, b, o, n, s); }
inline void passdebugDrawIndexedIndirect(VkCommandBuffer c, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t s) { if(!passdebug::g_Skip) vkCmdDrawIndexedIndirect(c, b, o, n, s); }
#define vkCmdDispatch passdebugDispatch
#define vkCmdDispatchIndirect passdebugDispatchIndirect
#define vkCmdDraw passdebugDraw
#define vkCmdDrawIndexed passdebugDrawIndexed
#define vkCmdDrawIndirect passdebugDrawIndirect
#define vkCmdDrawIndexedIndirect passdebugDrawIndexedIndirect
