// loader
#include "stb_image.h"
#include <iostream>
#include "loader.hpp"

#include "application.hpp"
#include "vk_initializers.h"
#include "vk_types.h"
#include <glm/gtx/quaternion.hpp>

#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>

std::optional<std::vector<std::shared_ptr<MeshAsset>>> loadGltfMeshes(Application* engine, std::filesystem::path filePath) {
	std::string pathString = util::getpath(filePath.string());
	std::filesystem::path systemPath = pathString;
	std::cout << "loading GLTF: " << pathString << std::endl;
	
	auto data = fastgltf::GltfDataBuffer::FromPath(systemPath);
	if(data.error() != fastgltf::Error::None) {
		std::cout << "failed to load GLTF data" << std::endl;
		return {};
	}

	constexpr auto gltfOptions = /* fastgltf::Options::LoadGLBBuffers | */fastgltf::Options::LoadExternalBuffers;
	fastgltf::Asset gltf;
	fastgltf::Parser parser {};
	
	auto load = parser.loadGltf(data.get(), systemPath.parent_path(), gltfOptions);
    if (load) {
        gltf = std::move(load.get());
    } else {
        fmt::print("failed to load gltf: {} \n", fastgltf::to_underlying(load.error()));
        return {};
    }
	
	std::vector<std::shared_ptr<MeshAsset>> meshes;
	
	std::vector<uint32_t> indices;
	std::vector<Vertex> vertices;
	
	for(fastgltf::Mesh& mesh : gltf.meshes) {
		MeshAsset newmesh;
		newmesh.name = mesh.name;
		
		indices.clear();
		vertices.clear();
		
		for(auto&& p : mesh.primitives) {
			GeoSurface newSurface;
			newSurface.startIndex = (uint32_t)indices.size();
			newSurface.count = (uint32_t)gltf.accessors[p.indicesAccessor.value()].count;
			
			size_t initialVtx = vertices.size();
			
			{
				fastgltf::Accessor& indexaccessor = gltf.accessors[p.indicesAccessor.value()]; // ?!?!?!?
				indices.reserve(indices.size() + indexaccessor.count);
				
				fastgltf::iterateAccessor<std::uint32_t>(gltf, indexaccessor,
					[&](std::uint32_t idx) {
						indices.push_back(idx + initialVtx);
					});
			}
			
			{
				fastgltf::Accessor& posAccessor = gltf.accessors[p.findAttribute("POSITION")->accessorIndex];
				vertices.resize(vertices.size() + posAccessor.count);
				
				fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, posAccessor,
					[&](glm::vec3 v, size_t index) {
						Vertex newVtx;
						newVtx.position = v;
						newVtx.normal = { 1, 0, 0 };
						newVtx.color = glm::vec4{1.f};
						newVtx.uv_x = 0;
						newVtx.uv_y = 0;
						vertices[initialVtx + index] = newVtx;
					});
			}
			
			auto normals = p.findAttribute("NORMAL");
			if (normals != p.attributes.end()) {
				fastgltf::iterateAccessorWithIndex<glm::vec3>(gltf, gltf.accessors[(*normals).accessorIndex],
					[&](glm::vec3 v, size_t index) {
						vertices[initialVtx + index].normal = v;
					});
			}
			
			auto uv = p.findAttribute("TEXCOORD_0");
			if (uv != p.attributes.end()) {
				fastgltf::iterateAccessorWithIndex<glm::vec2>(gltf, gltf.accessors[(*uv).accessorIndex],
					[&](glm::vec2 v, size_t index) {
						vertices[initialVtx + index].uv_x = v.x;
						vertices[initialVtx + index].uv_y = v.y;
					});
			}
			
			auto colors = p.findAttribute("COLOR_0");
			if (colors != p.attributes.end()) {
				fastgltf::iterateAccessorWithIndex<glm::vec4>(gltf, gltf.accessors[(*normals).accessorIndex],
					[&](glm::vec4 v, size_t index) {
						vertices[initialVtx + index].color = v;
					});
			}
			
			newmesh.surfaces.push_back(newSurface);
		}
		
		constexpr bool OverrideColors = true;
		if(OverrideColors) {
			for(Vertex& vtx : vertices) {
				vtx.color = glm::vec4(vtx.normal, 1.f);
			}
		}
		newmesh.meshBuffers = engine->uploadMesh(indices, vertices);
		meshes.emplace_back(std::make_shared<MeshAsset>(std::move(newmesh)));
	}
	return meshes;
}