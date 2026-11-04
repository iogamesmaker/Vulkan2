#ifndef TEXTURES_HPP
#define TEXTURES_HPP
// based on vkguide.dev's article on textures
#include "vk_types.h"

class Application;

// what a texture is used for, decides the block compression format
enum class TextureKind {
	Albedo, // BC1, colour without alpha
	Atlas,	// BC3, colour + alpha for cutouts (grass, leaves, flowers)
	Normal, // BC5, xy only, z is reconstructed in the shader
	Height	// BC4, single channel
};

class TextureLoader {
public:
	//TextureLoader();
	//~TextureLoader();
	
	void pass(VkDevice device, VmaAllocator allocator);
	
	AllocatedImage createImage(VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped = false);
	AllocatedImage createTexture(void* data, Application* engine, VkExtent3D size, VkFormat format, VkImageUsageFlags usage, bool mipmapped = false);
	AllocatedImage load(std::string path, Application* engine);
	// block compressed with mips, cached in a cache folder next to the source after the first run
	AllocatedImage loadCompressed(std::string path, Application* engine, TextureKind kind);
		
	void destroyImage(const AllocatedImage& image);
	
private:
	
	VkDevice m_Device;
	VmaAllocator m_Allocator;
};

#endif