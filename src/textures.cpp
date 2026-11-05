#include "textures.hpp"
#include "application.hpp"
#include <vk_initializers.h>
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_DXT_IMPLEMENTATION
#include "stb_dxt.h"
#include <filesystem>
#include <algorithm>
#include <fstream>

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
	if(format == VK_FORMAT_R16_UNORM) dataSize *= 2; // the cirrus noise lattice
	
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

// ------------------------------------------------------------------ block compressed textures
// a 2K RGBA8 texture with mips is ~22MB, as BC1 it's ~2.8MB. the compression (stb_dxt) runs once, the result is
// cached in assets/cache so later startups just read it back

namespace {
	struct MipLevel {
		uint32_t width, height;
		std::vector<uint8_t> data; // compressed blocks
	};

	constexpr uint32_t CACHE_MAGIC = 0x31544342; // "BCT1"
	constexpr uint32_t CACHE_VERSION = 1;
	// the cutout atlases (BC3) are on 2: their mips keep the full size coverage (preserveCoverage), rebuilt once
	uint32_t cacheVersion(uint32_t format) { return format == VK_FORMAT_BC3_UNORM_BLOCK ? 2u : CACHE_VERSION; }

	float alphaCoverage(const std::vector<uint8_t>& rgba, float scale) {
		size_t covered = 0, count = rgba.size() / 4;
		for(size_t i = 0; i < count; i++) covered += rgba[i * 4 + 3] * scale >= 127.5f;
		return count ? static_cast<float>(covered) / static_cast<float>(count) : 0.0f;
	}

	// alpha tested cutouts (leaves, grass, flowers) thin out in the mips: averaged alpha drops under the 0.5 test
	// where leaves and gaps mix, so a crown 100m away was bare twigs. scales this mip's alpha until the same share of
	// it passes the test as in the full size image (Castano, "Computing alpha mipmaps"). the chain itself is
	// downsampled from the unscaled mips
	std::vector<uint8_t> preserveCoverage(const std::vector<uint8_t>& rgba, float target) {
		float lo = 0.0f, hi = 8.0f;
		for(int i = 0; i < 16; i++) {
			float mid = 0.5f * (lo + hi);
			if(alphaCoverage(rgba, mid) < target) lo = mid;
			else hi = mid;
		}
		std::vector<uint8_t> out = rgba;
		for(size_t i = 3; i < out.size(); i += 4) out[i] = static_cast<uint8_t>(std::min(out[i] * hi + 0.5f, 255.0f));
		return out;
	}

	VkFormat formatFor(TextureKind kind) {
		switch(kind) {
			case TextureKind::Albedo: return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
			case TextureKind::Atlas:  return VK_FORMAT_BC3_UNORM_BLOCK;
			case TextureKind::Normal: return VK_FORMAT_BC5_UNORM_BLOCK;
			case TextureKind::Height: return VK_FORMAT_BC4_UNORM_BLOCK;
		}
		return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
	}

	uint32_t blockBytes(TextureKind kind) {
		return (kind == TextureKind::Albedo || kind == TextureKind::Height) ? 8 : 16;
	}

	// 2x2 box filter, normals get renormalised so they don't shrink towards flat in the small mips
	std::vector<uint8_t> downsample(const std::vector<uint8_t>& src, uint32_t width, uint32_t height, uint32_t channels, bool normals) {
		uint32_t w = std::max(width / 2, 1u), h = std::max(height / 2, 1u);
		std::vector<uint8_t> dst(w * h * channels);
		for(uint32_t y = 0; y < h; y++) {
			for(uint32_t x = 0; x < w; x++) {
				float sum[4] = {};
				for(uint32_t dy = 0; dy < 2; dy++) for(uint32_t dx = 0; dx < 2; dx++) {
					uint32_t sx = std::min(x * 2 + dx, width - 1), sy = std::min(y * 2 + dy, height - 1);
					for(uint32_t c = 0; c < channels; c++) sum[c] += src[(sy * width + sx) * channels + c];
				}
				for(uint32_t c = 0; c < channels; c++) sum[c] *= 0.25f;
				if(normals) {
					glm::vec3 n = glm::vec3(sum[0], sum[1], sum[2]) / 127.5f - 1.0f;
					n = glm::length(n) > 1e-4f ? glm::normalize(n) : glm::vec3(0, 0, 1);
					sum[0] = (n.x * 0.5f + 0.5f) * 255.0f; sum[1] = (n.y * 0.5f + 0.5f) * 255.0f; sum[2] = (n.z * 0.5f + 0.5f) * 255.0f;
				}
				for(uint32_t c = 0; c < channels; c++) dst[(y * w + x) * channels + c] = static_cast<uint8_t>(std::clamp(sum[c] + 0.5f, 0.0f, 255.0f));
			}
		}
		return dst;
	}

	MipLevel compress(const std::vector<uint8_t>& pixels, uint32_t width, uint32_t height, uint32_t channels, TextureKind kind) {
		MipLevel level{ width, height, {} };
		uint32_t bx = (width + 3) / 4, by = (height + 3) / 4;
		level.data.resize(bx * by * blockBytes(kind));

		uint8_t block[16 * 4];
		for(uint32_t y = 0; y < by; y++) {
			for(uint32_t x = 0; x < bx; x++) {
				// gather the 4x4 block, clamping at the edges of tiny mips
				for(uint32_t py = 0; py < 4; py++) for(uint32_t px = 0; px < 4; px++) {
					uint32_t sx = std::min(x * 4 + px, width - 1), sy = std::min(y * 4 + py, height - 1);
					const uint8_t* p = &pixels[(sy * width + sx) * channels];
					uint8_t* b = &block[(py * 4 + px) * 4];
					b[0] = p[0];
					b[1] = channels > 1 ? p[1] : 0;
					b[2] = channels > 2 ? p[2] : 0;
					b[3] = channels > 3 ? p[3] : 255;
				}
				uint8_t* out = &level.data[(y * bx + x) * blockBytes(kind)];
				switch(kind) {
					case TextureKind::Albedo: stb_compress_dxt_block(out, block, 0, STB_DXT_HIGHQUAL); break;
					case TextureKind::Atlas:  stb_compress_dxt_block(out, block, 1, STB_DXT_HIGHQUAL); break;
					case TextureKind::Normal: {
						uint8_t rg[32];
						for(int i = 0; i < 16; i++) { rg[i * 2] = block[i * 4]; rg[i * 2 + 1] = block[i * 4 + 1]; }
						stb_compress_bc5_block(out, rg);
						break;
					}
					case TextureKind::Height: {
						uint8_t r[16];
						for(int i = 0; i < 16; i++) r[i] = block[i * 4];
						stb_compress_bc4_block(out, r);
						break;
					}
				}
			}
		}
		return level;
	}

	std::string cachePathFor(const std::string& sourcePath) {
		std::filesystem::path source(sourcePath);
		return (source.parent_path() / "cache" / (source.filename().string() + ".bc")).string();
	}

	// the cache stores the source's size + timestamp, a changed source gets recompressed
	bool readCache(const std::string& path, uint64_t sourceSize, int64_t sourceTime, uint32_t format, std::vector<MipLevel>& levels) {
		std::ifstream file(path, std::ios::binary);
		if(!file) return false;
		uint32_t header[4];
		uint64_t size; int64_t time;
		file.read(reinterpret_cast<char*>(header), sizeof(header));
		file.read(reinterpret_cast<char*>(&size), sizeof(size));
		file.read(reinterpret_cast<char*>(&time), sizeof(time));
		if(!file || header[0] != CACHE_MAGIC || header[1] != cacheVersion(format) || header[2] != format || size != sourceSize || time != sourceTime) return false;

		levels.resize(header[3]);
		for(auto& level : levels) {
			uint32_t dims[3];
			file.read(reinterpret_cast<char*>(dims), sizeof(dims));
			level.width = dims[0]; level.height = dims[1];
			level.data.resize(dims[2]);
			file.read(reinterpret_cast<char*>(level.data.data()), dims[2]);
		}
		return static_cast<bool>(file);
	}

	void writeCache(const std::string& path, uint64_t sourceSize, int64_t sourceTime, uint32_t format, const std::vector<MipLevel>& levels) {
		std::filesystem::create_directories(std::filesystem::path(path).parent_path());
		std::ofstream file(path, std::ios::binary);
		uint32_t header[4] = { CACHE_MAGIC, cacheVersion(format), format, static_cast<uint32_t>(levels.size()) };
		file.write(reinterpret_cast<const char*>(header), sizeof(header));
		file.write(reinterpret_cast<const char*>(&sourceSize), sizeof(sourceSize));
		file.write(reinterpret_cast<const char*>(&sourceTime), sizeof(sourceTime));
		for(const auto& level : levels) {
			uint32_t dims[3] = { level.width, level.height, static_cast<uint32_t>(level.data.size()) };
			file.write(reinterpret_cast<const char*>(dims), sizeof(dims));
			file.write(reinterpret_cast<const char*>(level.data.data()), level.data.size());
		}
	}
}

AllocatedImage TextureLoader::loadCompressed(std::string path, Application* engine, TextureKind kind) {
	std::string filepath = util::getpath(path);
	VkFormat format = formatFor(kind);

	uint64_t sourceSize = std::filesystem::file_size(filepath);
	int64_t sourceTime = std::filesystem::last_write_time(filepath).time_since_epoch().count();
	std::string cachePath = cachePathFor(filepath);

	std::vector<MipLevel> levels;
	if(!readCache(cachePath, sourceSize, sourceTime, format, levels)) {
		uint32_t channels = kind == TextureKind::Height ? 1 : 4;
		int width, height, fileChannels;
		stbi_uc* pixels = stbi_load(filepath.c_str(), &width, &height, &fileChannels, channels);
		if(!pixels) throw std::runtime_error(std::string("failed to load texture at ") + filepath);

		std::vector<uint8_t> mip(pixels, pixels + static_cast<size_t>(width) * height * channels);
		stbi_image_free(pixels);

		uint32_t w = width, h = height;
		levels.clear();
		float coverage = kind == TextureKind::Atlas ? alphaCoverage(mip, 1.0f) : 0.0f;
		while(true) {
			bool scaled = kind == TextureKind::Atlas && !levels.empty() && coverage > 0.0f;
			levels.push_back(compress(scaled ? preserveCoverage(mip, coverage) : mip, w, h, channels, kind));
			if(w == 1 && h == 1) break;
			mip = downsample(mip, w, h, channels, kind == TextureKind::Normal);
			w = std::max(w / 2, 1u); h = std::max(h / 2, 1u);
		}
		writeCache(cachePath, sourceSize, sourceTime, format, levels);
		fmt::print("compressed {} ({} mips)\n", path, levels.size());
	}

	// upload every mip in one go
	size_t total = 0;
	for(const auto& level : levels) total += level.data.size();
	AllocatedBuffer upload = engine->createBuffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_TO_GPU);

	std::vector<VkBufferImageCopy> regions;
	size_t offset = 0;
	for(uint32_t i = 0; i < levels.size(); i++) {
		memcpy(static_cast<uint8_t*>(upload.info.pMappedData) + offset, levels[i].data.data(), levels[i].data.size());
		VkBufferImageCopy region = {};
		region.bufferOffset = offset;
		region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.imageSubresource.mipLevel = i;
		region.imageSubresource.layerCount = 1;
		region.imageExtent = { levels[i].width, levels[i].height, 1 };
		regions.push_back(region);
		offset += levels[i].data.size();
	}

	VkExtent3D size = { levels[0].width, levels[0].height, 1 };
	AllocatedImage texture = createImage(size, format, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, true);

	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		VkImageSubresourceRange range = vkinit::image_subresource_range(VK_IMAGE_ASPECT_COLOR_BIT);
		vkutil::transition_image(cmd, texture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, range);
		vkCmdCopyBufferToImage(cmd, upload.buffer, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, static_cast<uint32_t>(regions.size()), regions.data());
		vkutil::transition_image(cmd, texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, range);
	});

	engine->destroyBuffer(upload);
	return texture;
}
