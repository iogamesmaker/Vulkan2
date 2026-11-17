#pragma once
// what the model loaders (GroundCover, Props) share: reading a glTF, its nodes and primitives, and the careful
// simplifier their generated LODs come from
#include <cstdint>
#include <filesystem>
#include <optional>
#include <vector>
#include <glm/glm.hpp>
#include <fastgltf/core.hpp>

namespace meshload {
	// a .gltf with its external buffers; nullopt (and a line printed, "<who>: couldn't ...") when it can't be read
	std::optional<fastgltf::Asset> loadGltf(const std::filesystem::path& path, const char* who);

	// a node's rotation / scale only: the packs and sets lay their models out side by side, each one goes to its origin
	glm::mat3 nodeRotationScale(const fastgltf::Node& node);

	// one primitive's attributes, transformed by a node's rotation / scale (normals renormalised)
	struct Primitive {
		std::vector<glm::vec3> positions, normals;
		std::vector<glm::vec2> uvs;
		std::vector<uint32_t> indices; // relative to positions
	};
	// false (out left empty): the primitive lacks indices, positions, normals or uvs
	bool readPrimitive(const fastgltf::Asset& gltf, const fastgltf::Primitive& p, const glm::mat3& transform, Primitive& out);

	// meshopt_simplify only (never the sloppy one: it welds the mesh on a grid and mangles it): within error, and while
	// that stays over budget (+30%) the error allowed doubles up to maxError, then it may collapse across the uv seams
	// too. prune: then also drop the small loose pieces under maxError (only for what's small on screen: far plants,
	// shadows). returns the index count written to destination (room for indexCount)
	size_t simplifyCareful(uint32_t* destination, const uint32_t* indices, size_t indexCount, const float* positions, size_t vertexCount,
						   size_t stride, size_t target, float error, float maxError, bool prune);
}
