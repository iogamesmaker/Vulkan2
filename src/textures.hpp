#ifndef TEXTURES_HPP
#define TEXTURES_HPP
// based on vkguide.dev's article on textures
#include "vk_types.h"

class Application;

class TextureLoader {
public:
	//TextureLoader();
	//~TextureLoader();
	
	void pass(VkDevice device, VmaAllocator allocator);
	
	AllocatedImage createImage(VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped = false);
	AllocatedImage createTexture(void* data, Application* engine, VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped = false);
	AllocatedImage load(std::string path, Application* engine);
		
	void destroyImage(const AllocatedImage& image);
	
private:
	
	VkDevice m_Device;
	VmaAllocator m_Allocator;
};

#endif