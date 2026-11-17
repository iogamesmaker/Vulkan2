
#pragma once 

#include <vk_types.h>

namespace vkutil {

void transition_image(VkCommandBuffer cmd, VkImage image, VkImageLayout currentLayout, VkImageLayout newLayout);
void transition_image(VkCommandBuffer cmd, VkImage image, VkImageLayout currentLayout, VkImageLayout newLayout, VkImageSubresourceRange subresourceRange);

void copy_image_to_image(VkCommandBuffer cmd, VkImage source, VkImage destination,VkExtent2D srcSize, VkExtent2D dstSize);

void generate_mipmaps(VkCommandBuffer cmd, VkImage image, VkExtent2D imageSize);

// a global memory barrier (synchronization2): everything srcStage wrote with srcAccess becomes visible to dstStage's dstAccess
void memoryBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess);
// a GPU only image (one mip, one layer, 2D or 3D from the extent's depth) and its default view
AllocatedImage create_image(VkDevice device, VmaAllocator allocator, VkFormat format, VkImageUsageFlags usage, VkExtent3D extent,
                            VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT, VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D);
} // namespace vkutil
