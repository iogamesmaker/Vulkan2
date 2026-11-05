#ifndef DRAWSTATS_HPP
#define DRAWSTATS_HPP

#include <vk_types.h>
#include <vector>

// what the GPU driven passes (grass, trees, props, ground cover) actually drew, read back from their indirect draw
// buffers for the "GPU draw counts" window. every system lists the buffers it fills (copied into a host visible
// buffer at the end of a frame) and turns their bytes into rows

struct DrawStat {
	const char* name;
	uint64_t instances = 0;
	uint64_t triangles = 0;
};

struct DrawStatBuffer {
	VkBuffer buffer;
	VkDeviceSize bytes;
};

// sums of indirect commands [first, first + count) into a row
inline DrawStat sumIndexed(const char* name, const uint8_t* data, uint32_t first, uint32_t count) {
	DrawStat s{ name };
	const auto* commands = reinterpret_cast<const VkDrawIndexedIndirectCommand*>(data);
	for(uint32_t i = first; i < first + count; i++) {
		s.instances += commands[i].instanceCount;
		s.triangles += static_cast<uint64_t>(commands[i].instanceCount) * (commands[i].indexCount / 3);
	}
	return s;
}

inline DrawStat sumNonIndexed(const char* name, const uint8_t* data, uint32_t first, uint32_t count) {
	DrawStat s{ name };
	const auto* commands = reinterpret_cast<const VkDrawIndirectCommand*>(data);
	for(uint32_t i = first; i < first + count; i++) {
		s.instances += commands[i].instanceCount;
		s.triangles += static_cast<uint64_t>(commands[i].instanceCount) * (commands[i].vertexCount / 3);
	}
	return s;
}

#endif
