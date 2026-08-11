#include "textures.hpp"
#include "application.hpp"
#include <vk_initializers.h>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

void TextureLoader::pass(VkDevice device, VmaAllocator allocator) {
	m_Device = device;
	m_Allocator = allocator;
}

AllocatedImage TextureLoader::createImage(VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped) {
	AllocatedImage newImage;
	newImage.imageFormat = format;
	newImage.imageExtent = size;
	
	VkImageCreateInfo imageCI = vkinit::image_create_info(format, usage, size);
	if(mipmapped) imageCI.mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(size.width, size.height)))) + 1;
	
	VmaAllocationCreateInfo allocCI = {};
	allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;
	allocCI.requiredFlags = VkMemoryPropertyFlags(VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	
	VK_CHECK(vmaCreateImage(m_Allocator, &imageCI, &allocCI, &newImage.image, &newImage.allocation, nullptr));
	
	VkImageAspectFlags aspectFlag = VK_IMAGE_ASPECT_COLOR_BIT;
	if (format == VK_FORMAT_D32_SFLOAT) aspectFlag = VK_IMAGE_ASPECT_DEPTH_BIT;
	
	VkImageViewCreateInfo viewCI = vkinit::imageview_create_info(format, newImage.image, aspectFlag);
	viewCI.subresourceRange.levelCount = imageCI.mipLevels;
	
	VK_CHECK(vkCreateImageView(m_Device, &viewCI, nullptr, &newImage.imageView));
	return newImage;
}

AllocatedImage TextureLoader::createTexture(void* data, Application* engine, VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped) {
	size_t dataSize = size.depth * size.width * size.height; // assuming we only have 1 channel it's okay. if we have more channels multiply it by the amount of channels
	if(format == VK_FORMAT_R8G8B8A8_UNORM) dataSize *= 4;
	
	uint32_t mipLevels = 1;
	if(mipmapped) mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(size.width, size.height)))) + 1;

	AllocatedBuffer uploadBuffer = engine->createBuffer(dataSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);
	
	memcpy(uploadBuffer.info.pMappedData, data, dataSize);
	
	AllocatedImage newTexture = createImage(size, format, usage | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, mipmapped);
	
	if(mipmapped) {
		engine->immediateSubmit([&](VkCommandBuffer cmd) {
			VkImageSubresourceRange subresourceRange = {};
			
			subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			subresourceRange.baseMipLevel = 0;
			subresourceRange.levelCount = mipLevels;
			subresourceRange.baseArrayLayer = 0;
			subresourceRange.layerCount = 1;
			
			vkutil::transition_image(cmd, newTexture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, subresourceRange);
			
			VkBufferImageCopy copyRegion = {};
			copyRegion.bufferOffset = 0;
			copyRegion.bufferRowLength = 0;
			copyRegion.bufferImageHeight = 0;
			copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			copyRegion.imageSubresource.mipLevel = 0;
			copyRegion.imageSubresource.baseArrayLayer = 0;
			copyRegion.imageSubresource.layerCount = 1;
			copyRegion.imageExtent = size;
			
			vkCmdCopyBufferToImage(cmd, uploadBuffer.buffer, newTexture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
			
			int32_t mipWidth  = static_cast<int32_t>(size.width );
			int32_t mipHeight = static_cast<int32_t>(size.height);
			
			for(uint32_t i = 1; i < mipLevels; i++) {
				VkImageSubresourceRange subresourceRangeSource = {};
				
				subresourceRangeSource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				subresourceRangeSource.baseMipLevel = i - 1;
				subresourceRangeSource.levelCount = 1;
				subresourceRangeSource.baseArrayLayer = 0;
				subresourceRangeSource.layerCount = 1;
				
				vkutil::transition_image(cmd, newTexture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, subresourceRangeSource);
				
				VkImageBlit blit = {};
				blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				blit.srcSubresource.mipLevel = i - 1;
				blit.srcSubresource.baseArrayLayer = 0;
				blit.srcSubresource.layerCount = 1;
				blit.srcOffsets[0] = {0, 0, 0};
				blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
				
				mipWidth = mipWidth * 0.5;
				mipHeight = mipHeight * 0.5;
				
				if(mipWidth  == 0) mipWidth  = 1;
				if(mipHeight == 0) mipHeight = 1;
				
				blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
				blit.dstSubresource.mipLevel = i;
				blit.dstSubresource.baseArrayLayer = 0;
				blit.dstSubresource.layerCount = 1;
				blit.dstOffsets[0] = {0, 0, 0};
				blit.dstOffsets[1] = {mipWidth, mipHeight, 1};
				
				vkCmdBlitImage(cmd, newTexture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, newTexture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
				
				vkutil::transition_image(cmd, newTexture.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, subresourceRangeSource);
			}
			
			VkImageSubresourceRange subresourceRangeFinal = {};
			
			subresourceRangeFinal.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			subresourceRangeFinal.baseMipLevel = mipLevels - 1;
			subresourceRangeFinal.levelCount = 1;
			subresourceRangeFinal.baseArrayLayer = 0;
			subresourceRangeFinal.layerCount = 1;
			
			vkutil::transition_image(cmd, newTexture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, subresourceRangeFinal);
		});
	} else {
		engine->immediateSubmit([&](VkCommandBuffer cmd) {
			vkutil::transition_image(cmd, newTexture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			
			VkBufferImageCopy copyRegion = {};
			copyRegion.bufferOffset = 0;
			copyRegion.bufferRowLength = 0;
			copyRegion.bufferImageHeight = 0;
			
			copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
			copyRegion.imageSubresource.mipLevel = 0;
			copyRegion.imageSubresource.baseArrayLayer = 0;
			copyRegion.imageSubresource.layerCount = 1;
			copyRegion.imageExtent = size;
			
			vkCmdCopyBufferToImage(cmd, uploadBuffer.buffer, newTexture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);
			
			vkutil::transition_image(cmd, newTexture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		});
	}
	
	engine->destroyBuffer(uploadBuffer);
	
	return newTexture;
}	

AllocatedImage TextureLoader::load(std::string path, Application* engine) {
	int width, height, channels;
	std::string filepath = util::getpath(path);
	stbi_uc* pixels = stbi_load(filepath.c_str(), &width, &height, &channels, 4);
	
	if(!pixels) throw std::runtime_error(std::string("failed to load texture at") + util::getpath(path));
	
	VkExtent3D size = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
	
	VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
	if(channels == 3) format = VK_FORMAT_R8G8B8A8_UNORM; // fucking truecolor isnt supported EVEN THOUGH ITS EXISTED FOR 35 YEARS!!! there goes 33% of vram
	if(channels == 1) { // todo: this fucking sucks make it better
		format = VK_FORMAT_R8_UNORM;
		stbi_image_free(pixels);
		pixels = stbi_load(filepath.c_str(), &width, &height, &channels, 1);
	}
	
	AllocatedImage newTexture = createTexture(pixels, engine, size, format, VK_IMAGE_USAGE_SAMPLED_BIT, true);
	
	stbi_image_free(pixels);
	
	return newTexture;
}

void TextureLoader::destroyImage(const AllocatedImage& image) {
	vkDestroyImageView(m_Device, image.imageView, nullptr);
	vmaDestroyImage(m_Allocator, image.image, image.allocation);
}