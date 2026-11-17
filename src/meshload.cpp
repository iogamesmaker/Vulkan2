#include "meshload.hpp"
#include <algorithm>
#include <fmt/core.h>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/tools.hpp>
#include <glm/gtc/quaternion.hpp>
#include <meshoptimizer.h>

namespace meshload {
	std::optional<fastgltf::Asset> loadGltf(const std::filesystem::path& path, const char* who) {
		auto data = fastgltf::GltfDataBuffer::FromPath(path);
		if(data.error() != fastgltf::Error::None) {
			fmt::print("{}: couldn't read {}\n", who, path.string());
			return std::nullopt;
		}
		fastgltf::Parser parser{};
		auto asset = parser.loadGltf(data.get(), path.parent_path(), fastgltf::Options::LoadExternalBuffers);
		if(!asset) {
			fmt::print("{}: couldn't load {}\n", who, path.string());
			return std::nullopt;
		}
		return std::move(asset.get());
	}

	glm::mat3 nodeRotationScale(const fastgltf::Node& node) {
		glm::mat3 m(1.0f);
		if(const auto* trs = std::get_if<fastgltf::TRS>(&node.transform)) {
			glm::quat q(trs->rotation[3], trs->rotation[0], trs->rotation[1], trs->rotation[2]); // w, x, y, z
			glm::mat3 scale(1.0f);
			for(int i = 0; i < 3; i++) scale[i][i] = trs->scale[i];
			m = glm::mat3_cast(q) * scale;
		} else if(const auto* mat = std::get_if<fastgltf::math::fmat4x4>(&node.transform)) {
			for(int c = 0; c < 3; c++)
				for(int r = 0; r < 3; r++) m[c][r] = (*mat)[c][r];
		}
		return m;
	}

	bool readPrimitive(const fastgltf::Asset& gltf, const fastgltf::Primitive& p, const glm::mat3& transform, Primitive& out) {
		out = {};
		auto position = p.findAttribute("POSITION"), normal = p.findAttribute("NORMAL"), uv = p.findAttribute("TEXCOORD_0");
		if(!p.indicesAccessor || position == p.attributes.end() || normal == p.attributes.end() || uv == p.attributes.end()) return false;
		const fastgltf::Accessor& positions = gltf.accessors[position->accessorIndex];
		out.positions.resize(positions.count);
		out.normals.resize(positions.count);
		out.uvs.resize(positions.count);
		fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, positions, [&](glm::vec3 v, size_t i) { out.positions[i] = transform * v; });
		fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, gltf.accessors[normal->accessorIndex], [&](glm::vec3 v, size_t i) { out.normals[i] = glm::normalize(transform * v); });
		fastgltf::iterateAccessorWithIndex<glm::vec2>(gltf, gltf.accessors[uv->accessorIndex], [&](glm::vec2 v, size_t i) { out.uvs[i] = v; });
		const fastgltf::Accessor& indices = gltf.accessors[*p.indicesAccessor];
		out.indices.reserve(indices.count);
		fastgltf::iterateAccessor<uint32_t>(gltf, indices, [&](uint32_t index) { out.indices.push_back(index); });
		return true;
	}

	size_t simplifyCareful(uint32_t* destination, const uint32_t* indices, size_t indexCount, const float* positions, size_t vertexCount,
						   size_t stride, size_t target, float error, float maxError, bool prune) {
		size_t count = meshopt_simplify(destination, indices, indexCount, positions, vertexCount, stride, target, error, 0, nullptr);
		while(count > target * 13 / 10 && error < maxError) {
			error = std::min(error * 2.0f, maxError);
			count = meshopt_simplify(destination, indices, indexCount, positions, vertexCount, stride, target, error, 0, nullptr);
		}
		// still over: allowed to collapse across the uv seams too (scans with fragmented uv charts are seams everywhere,
		// the careful simplifier won't touch them; still the error bounded one, not the sloppy)
		if(count > target * 13 / 10)
			count = meshopt_simplify(destination, indices, indexCount, positions, vertexCount, stride, target, maxError, meshopt_SimplifyPermissive, nullptr);
		if(prune && count > target * 13 / 10) {
			// (into a copy: pruning at a big error can drop nearly everything, then the unpruned one stays)
			std::vector<uint32_t> pruned(indexCount);
			size_t prunedCount = meshopt_simplify(pruned.data(), indices, indexCount, positions, vertexCount, stride, target, maxError * 0.5f, meshopt_SimplifyPrune, nullptr);
			if(prunedCount >= target / 2) { std::copy(pruned.begin(), pruned.begin() + prunedCount, destination); count = prunedCount; }
		}
		return count;
	}
}
