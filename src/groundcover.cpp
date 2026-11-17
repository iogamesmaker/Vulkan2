#include "groundcover.hpp"
#include "application.hpp"
#include "gbuffer.hpp"
#include "meshload.hpp"
#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <chrono>
#include <cstring>
#include <fstream>
#include <fastgltf/glm_element_traits.hpp>
#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <glm/gtc/quaternion.hpp>
#include <meshoptimizer.h>

namespace {
	// in GC_* order (groundcovercommon.glsl). opaque: no cutout map, the plain albedo. the newer downloads have options:
	// minTriangles skips the bits too small to be a plant of their own (single blades), maxVariants takes that many of
	// the rest (spread from the lightest to the densest, the mesh budget is MAX_MESHES), decimate simplifies a
	// variant over MAX_TRIANGLES down to it instead of leaving it out
	// decimateTo: the triangles a decimated variant is simplified to (0 = MAX_TRIANGLES; the dense bushes keep more)
	// cards: the model is loose leaf cards (thinCards below), its triangle budget and LODs are met by thinning them
	struct SpeciesFile { const char* name; bool opaque; uint32_t minTriangles = 0, maxVariants = 0; bool decimate = false; uint32_t decimateTo = 0; bool cards = false; };
	constexpr SpeciesFile SPECIES_FILES[] = {
		{ "fern_02", false }, { "dry_branches_medium_01", true }, { "shrub_02", false }, { "shrub_03", false }, { "nettle_plant", false },
		{ "periwinkle_plant", true }, { "shrub_sorrel_01", false }, { "weed_plant_02", false }, { "dandelion_01", false },
		{ "flower_gazania", false }, { "flower_ursinia", false }, { "flower_empodium", false }, { "flower_stinkkruid", false },
		{ "wild_rooibos_bush", false },
		{ "moss_01", false }, { "celandine_01", false },
		// "": an empty slot (nothing loads, groundcover.comp doesn't place it)
		// tools/import_packs.py's game ready packs (CC-BY, assets/plants/README.md), their own LODs (<plant>__LOD1 / 2).
		// TREE: the big ones the tree pipeline draws now (lilac bushes, saplings and trees, LOLIPOP's big bushes, the
		// single cacti: tools/import_trees.py's shrub packs, trees_place.comp places them where groundcover.comp picks
		// them, groundcoverspecies.glsl gcTreeKind). nothing loads here, the slot keeps the GC_* numbering
		{ "", false } /* TREE: lilac_bush */, { "", false } /* TREE: lolipop_bush_big */,
		{ "grass_medium_01", false },
		{ "", true }, // (root_cluster_01: one 225k triangle mesh, it never loaded)
		// Poly Haven (CC0), 2026-11: more grasses, the desert's succulents and dead branches
		{ "daisy", false, 0, 6 }, { "grass_medium_02", false, 0, 4 }, { "grass_bermuda_01", false, 100, 4 },
		{ "crystalline_iceplant", false, 0, 4 }, { "cheiridopsis_succulent", false, 0, 4 }, { "dead_quiver_branch_01", true, 0, 1 },
		// scanned mushrooms (Loic Norgeot), the packs' cacti, undergrowth, grass tufts, lilac saplings and trees, small bushes
		{ "mushrooms", true, 0, 8 },
		{ "", false } /* TREE: cactus */, { "cactus_clump", false },
		{ "dead_quiver_branch_02", true, 0, 1 }, { "undergrowth", false, 0, 6 }, { "grass_tufts", false, 0, 8 },
		{ "root_cluster_02", true, 0, 3 }, { "", false } /* TREE: lilac_sapling */, { "lolipop_bush_small", false, 0, 6 },
		{ "", false } /* TREE: lilac_tree */,
	};
	// species made from another's model with a look of their own (groundcovercommon.glsl GC_BRACKEN.., the colouring in
	// groundcover.frag): more kinds of plants from the models already here. in GC_* order after the files
	constexpr uint32_t VARIANT_OF[] = { 0 /* bracken: the fern */, 2 /* autumn shrub: shrub_02 */, 18 /* straw tussock */, 18 /* sedge */ };
	static_assert(std::size(SPECIES_FILES) == 36, "the GC_* numbering (groundcovercommon.glsl)");
	constexpr uint32_t MESH_CACHE_VERSION = 4; // bump when the mesh processing changes (assets/cache/groundcover.meshes)
	// debugging: every plant at its full original detail, no simplifying or thinning, the LODs copies of it (heavy)
	constexpr bool DEBUG_NO_SIMPLIFY = false;
	// a variant over this is left out, never decimated (models made for it, game ready, are; the rest broke when squeezed)
	constexpr size_t MAX_TRIANGLES = 15000;

	// foliage made of loose leaf cards (thousands of separate 2-6 triangle pieces): a triangle collapsing simplifier
	// mangles those (cards fold, stretch across each other, the thin twigs vanish), so these are thinned instead:
	// every connected piece is kept or dropped whole, the kept leaf cards grown about their centre so the plant stays
	// as full, the branches (the big pieces) always kept. new vertices for the grown cards are appended to verts.
	// indices / the result are local to verts. salt: which cards (each LOD its own pick)
	template<class V>
	std::vector<uint32_t> thinCards(std::vector<V>& verts, const uint32_t* idx, size_t indexCount, size_t targetTriangles, uint32_t salt) {
		size_t triangles = indexCount / 3;
		std::vector<uint32_t> out(idx, idx + indexCount);
		if(triangles <= targetTriangles) return out;
		// pieces: vertices welded by their exact position, union find over the triangles
		std::unordered_map<uint64_t, uint32_t> weld;
		std::vector<uint32_t> welded(verts.size());
		for(size_t i = 0; i < verts.size(); i++) {
			const float* p = &verts[i].positionSpecies.x;
			uint64_t h = 1469598103934665603ull;
			for(int k = 0; k < 3; k++) { uint32_t bits; std::memcpy(&bits, p + k, 4); h = (h ^ bits) * 1099511628211ull; }
			welded[i] = weld.emplace(h, static_cast<uint32_t>(i)).first->second;
		}
		std::vector<uint32_t> parent(verts.size());
		for(size_t i = 0; i < parent.size(); i++) parent[i] = static_cast<uint32_t>(i);
		auto find = [&](uint32_t x) { while(parent[x] != x) { parent[x] = parent[parent[x]]; x = parent[x]; } return x; };
		for(size_t t = 0; t < triangles; t++) {
			uint32_t a = find(welded[idx[t * 3]]), b = find(welded[idx[t * 3 + 1]]), c = find(welded[idx[t * 3 + 2]]);
			parent[b] = a; parent[find(c)] = a;
		}
		struct Piece { std::vector<uint32_t> tris; glm::vec3 lo{ 1e9f }, hi{ -1e9f }; };
		std::unordered_map<uint32_t, uint32_t> pieceOf;
		std::vector<Piece> pieces;
		glm::vec3 lo(1e9f), hi(-1e9f);
		for(size_t t = 0; t < triangles; t++) {
			uint32_t root = find(welded[idx[t * 3]]);
			auto [it, added] = pieceOf.emplace(root, static_cast<uint32_t>(pieces.size()));
			if(added) pieces.emplace_back();
			Piece& piece = pieces[it->second];
			piece.tris.push_back(static_cast<uint32_t>(t));
			for(int k = 0; k < 3; k++) {
				glm::vec3 p(verts[idx[t * 3 + k]].positionSpecies);
				piece.lo = glm::min(piece.lo, p); piece.hi = glm::max(piece.hi, p);
				lo = glm::min(lo, p); hi = glm::max(hi, p);
			}
		}
		// the branches: pieces over a fifth of the plant's size or of many triangles. the cards: the rest
		float size = glm::length(hi - lo);
		size_t structure = 0, cards = 0;
		std::vector<bool> isCard(pieces.size());
		for(size_t i = 0; i < pieces.size(); i++) {
			isCard[i] = glm::length(pieces[i].hi - pieces[i].lo) < size * 0.2f && pieces[i].tris.size() <= 64;
			(isCard[i] ? cards : structure) += pieces[i].tris.size();
		}
		// what fraction of the cards fits the budget (the branches thinned too when they alone are over half of it)
		float keepCards = cards ? glm::clamp((static_cast<float>(targetTriangles) - std::min<float>(structure, targetTriangles * 0.5f)) / cards, 0.03f, 1.0f) : 1.0f;
		float keepStructure = structure > targetTriangles / 2 ? glm::clamp(targetTriangles * 0.5f / structure, 0.05f, 1.0f) : 1.0f;
		float grow = std::min(1.0f / std::sqrt(keepCards), 2.2f);
		out.clear();
		for(size_t i = 0; i < pieces.size(); i++) {
			uint32_t h = static_cast<uint32_t>(i) * 2654435761u ^ (salt * 2246822519u);
			h ^= h >> 15; h *= 2246822519u; h ^= h >> 13; h *= 3266489917u; h ^= h >> 16;
			float roll = (h >> 8) * (1.0f / 16777216.0f);
			// (the biggest branches always: the plant's frame)
			bool keep = isCard[i] ? roll < keepCards : (roll < keepStructure || glm::length(pieces[i].hi - pieces[i].lo) > size * 0.35f);
			if(!keep) continue;
			if(!isCard[i] || grow <= 1.001f) {
				for(uint32_t t : pieces[i].tris) out.insert(out.end(), idx + t * 3, idx + t * 3 + 3);
				continue;
			}
			glm::vec3 centre = (pieces[i].lo + pieces[i].hi) * 0.5f;
			std::unordered_map<uint32_t, uint32_t> copy;
			for(uint32_t t : pieces[i].tris)
				for(int k = 0; k < 3; k++) {
					uint32_t v = idx[t * 3 + k];
					auto [it, added] = copy.emplace(v, static_cast<uint32_t>(verts.size()));
					if(added) {
						V grown = verts[v];
						glm::vec3 p = centre + (glm::vec3(grown.positionSpecies) - centre) * grow;
						grown.positionSpecies = glm::vec4(p, grown.positionSpecies.w);
						verts.push_back(grown);
					}
					out.push_back(it->second);
				}
		}
		return out;
	}

}

void GroundCover::init(Application* engine, TextureLoader& textures, const AllocatedImage& heightmap, const AllocatedImage& climateMap,
					   VkSampler heightmapSampler, VkSampler textureSampler, VkFormat shadowFormat, VkBuffer interactionBuffer) {
	m_Device = engine->m_Device;
	m_Allocator = engine->getAllocator();
	m_TextureLoader = &textures;
	static_assert(std::size(SPECIES_FILES) == FILES && FILES + std::size(VARIANT_OF) == SPECIES);

	// ---------------------------------------------------------------- the models
	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;
	Species species[SPECIES]{};
	const float toWorld = 1.0f / 6.0f; // metres -> world units (Camera::metersPerUnit)
	// a texture pair (albedo, normal) of a model: textures/<stem>_diff_alpha_1k.png (the cutout) or _diff_1k.jpg
	auto loadTexturePair = [&](uint32_t s, const std::string& stem) {
		std::string tex = std::string("assets\\plants\\") + SPECIES_FILES[s].name + "\\textures\\" + stem;
		bool cutout = !SPECIES_FILES[s].opaque && std::filesystem::exists(util::getpath(tex + "_diff_alpha_1k.png"));
		m_Textures.push_back(textures.loadCompressed(tex + (cutout ? "_diff_alpha_1k.png" : "_diff_1k.jpg"), engine, cutout ? TextureKind::Atlas : TextureKind::Albedo));
		m_Textures.push_back(textures.loadCompressed(tex + "_nor_gl_1k.jpg", engine, TextureKind::Normal));
	};
	// the processed meshes (simplified, their LODs, reordered for the GPU) are cached in assets/cache/groundcover.meshes:
	// the downloads are up to ~360k triangles a variant, simplifying them took seconds on every launch. the key is
	// the model files' sizes and times, the list above and MESH_CACHE_VERSION (bump it when the processing changes)
	std::vector<std::pair<uint32_t, std::string>> stems; // the texture pairs in load order: (file, stem)
	uint64_t cacheKey = 1469598103934665603ull;
	{
		auto mix = [&](const void* data, size_t size) {
			for(size_t i = 0; i < size; i++) cacheKey = (cacheKey ^ static_cast<const uint8_t*>(data)[i]) * 1099511628211ull;
		};
		uint32_t numbers[] = { MESH_CACHE_VERSION, FILES, SPECIES, MAX_MESHES, MAX_TEXTURE_PAIRS, static_cast<uint32_t>(MAX_TRIANGLES), static_cast<uint32_t>(sizeof(Vertex)), static_cast<uint32_t>(sizeof(Mesh)), DEBUG_NO_SIMPLIFY ? 1u : 0u };
		mix(numbers, sizeof(numbers));
		for(const SpeciesFile& f : SPECIES_FILES) {
			mix(f.name, std::strlen(f.name) + 1);
			if(!f.name[0]) continue;
			uint32_t options[] = { f.opaque, f.minTriangles, f.maxVariants, f.decimate, f.decimateTo };
			mix(options, sizeof(options));
			std::error_code ec;
			for(const auto& entry : std::filesystem::directory_iterator(util::getpath(std::string("assets\\plants\\") + f.name), ec)) {
				std::string ext = entry.path().extension().string();
				if(ext != ".gltf" && ext != ".bin") continue;
				uint64_t stamp[2] = { static_cast<uint64_t>(entry.file_size(ec)), static_cast<uint64_t>(entry.last_write_time(ec).time_since_epoch().count()) };
				mix(entry.path().filename().string().data(), entry.path().filename().string().size());
				mix(stamp, sizeof(stamp));
			}
		}
	}
	std::filesystem::path cachePath = util::getpath("assets\\cache\\groundcover.meshes");
	bool cached = false;
	auto started = std::chrono::steady_clock::now();
	if(std::ifstream in{ cachePath, std::ios::binary }) {
		uint64_t key = 0;
		uint32_t counts[4] = {}; // vertices, indices, meshes, texture pairs
		if(in.read(reinterpret_cast<char*>(&key), sizeof(key)) && key == cacheKey && in.read(reinterpret_cast<char*>(counts), sizeof(counts))) {
			vertices.resize(counts[0]);
			indices.resize(counts[1]);
			m_Meshes.resize(counts[2]);
			in.read(reinterpret_cast<char*>(vertices.data()), vertices.size() * sizeof(Vertex));
			in.read(reinterpret_cast<char*>(indices.data()), indices.size() * sizeof(uint32_t));
			in.read(reinterpret_cast<char*>(m_Meshes.data()), m_Meshes.size() * sizeof(Mesh));
			in.read(reinterpret_cast<char*>(species), sizeof(species));
			for(uint32_t i = 0; i < counts[3] && in; i++) {
				uint32_t file = 0, length = 0;
				in.read(reinterpret_cast<char*>(&file), sizeof(file));
				in.read(reinterpret_cast<char*>(&length), sizeof(length));
				std::string stem(length, '\0');
				in.read(stem.data(), length);
				stems.emplace_back(file, stem);
			}
			cached = static_cast<bool>(in) && counts[3] == stems.size();
			if(!cached) { vertices.clear(); indices.clear(); m_Meshes.clear(); stems.clear(); }
		}
	}
	if(cached) {
		for(const auto& [s, stem] : stems) loadTexturePair(s, stem);
	} else {
		for(uint32_t s = 0; s < FILES; s++) {
			if(!SPECIES_FILES[s].name[0]) { species[s].meshes = glm::uvec4(static_cast<uint32_t>(m_Meshes.size()), 0, 0, 0); continue; } // (an empty slot)
			std::string base = std::string("assets\\plants\\") + SPECIES_FILES[s].name + "\\";
			species[s].meshes = glm::uvec4(static_cast<uint32_t>(m_Meshes.size()), 0, 0, 0);
			// textures: a pair (albedo, normal) per image the model's materials use (most use one, a sapling's twigs and
			// bark two), loaded when a primitive first needs it. the albedo's cutout version (its alpha merged in,
			// <stem>_diff_alpha_1k.png) where there is one, the plain <stem>_diff_1k.jpg otherwise. every vertex carries
			// its pair's index (uv.z)
			std::unordered_map<std::string, uint32_t> pairOf;
			auto texturePair = [&](const std::string& stem) -> uint32_t {
				if(auto it = pairOf.find(stem); it != pairOf.end()) return it->second;
				uint32_t pair = static_cast<uint32_t>(m_Textures.size() / 2);
				if(pair >= MAX_TEXTURE_PAIRS) { fmt::print("ground cover: too many textures, {} shares the first\n", stem); return pairOf[stem] = 0; }
				loadTexturePair(s, stem);
				stems.emplace_back(s, stem);
				return pairOf[stem] = pair;
			};

			std::filesystem::path path = util::getpath(base + SPECIES_FILES[s].name + "_1k.gltf");
			std::optional<fastgltf::Asset> asset = meshload::loadGltf(path, "ground cover");
			if(!asset) continue;
			fastgltf::Asset& gltf = *asset;
			const SpeciesFile& file = SPECIES_FILES[s];
			// which variants (nodes) to take: big enough, then at most maxVariants spread over them by density
			std::vector<std::pair<size_t, size_t>> candidates; // triangles, node
			// (tools/import_packs.py's packs: <plant>__LOD0 is the variant, its __LOD1 / __LOD2 its own hand made LODs)
			auto lodOf = [&](size_t n) {
				std::string name(gltf.nodes[n].name);
				size_t at = name.rfind("__LOD");
				return at == std::string::npos ? -1 : std::atoi(name.c_str() + at + 5);
			};
			for(size_t n = 0; n < gltf.nodes.size(); n++) {
				if(!gltf.nodes[n].meshIndex || lodOf(n) > 0) continue;
				size_t triangles = 0;
				for(const fastgltf::Primitive& p : gltf.meshes[*gltf.nodes[n].meshIndex].primitives)
					if(p.indicesAccessor) triangles += gltf.accessors[*p.indicesAccessor].count / 3;
				if(triangles >= file.minTriangles) candidates.push_back({ triangles, n });
			}
			std::sort(candidates.begin(), candidates.end());
			std::vector<bool> chosen(gltf.nodes.size(), file.maxVariants == 0);
			if(file.maxVariants > 0 && !candidates.empty()) {
				size_t count = std::min<size_t>(file.maxVariants, candidates.size());
				for(size_t i = 0; i < count; i++) chosen[candidates[count == 1 ? 0 : i * (candidates.size() - 1) / (count - 1)].second] = true;
			} else for(const auto& c : candidates) chosen[c.second] = true;
			for(size_t n = 0; n < gltf.nodes.size(); n++) {
				const fastgltf::Node& node = gltf.nodes[n];
				if(!node.meshIndex || !chosen[n] || m_Meshes.size() >= MAX_MESHES) continue;
				// every primitive of the node into one mesh (some plants are split into twigs / leaves / stems, all on
				// the same texture)
				const fastgltf::Mesh& gltfMesh = gltf.meshes[*node.meshIndex];
				size_t triangles = 0;
				bool usable = true;
				for(const fastgltf::Primitive& p : gltfMesh.primitives) {
					usable &= p.indicesAccessor && p.findAttribute("POSITION") != p.attributes.end() && p.findAttribute("NORMAL") != p.attributes.end()
							  && p.findAttribute("TEXCOORD_0") != p.attributes.end();
					if(p.indicesAccessor) triangles += gltf.accessors[*p.indicesAccessor].count / 3;
				}
				if(!usable || (triangles > MAX_TRIANGLES && !file.decimate && !DEBUG_NO_SIMPLIFY)) continue;

				size_t first = vertices.size();
				Mesh mesh;
				mesh.firstIndex = static_cast<uint32_t>(indices.size());
				mesh.vertexOffset = static_cast<int32_t>(first);
				auto appendNode = [&](const fastgltf::Node& node) {
				glm::mat3 transform = meshload::nodeRotationScale(node);
				meshload::Primitive prim;
				for(const fastgltf::Primitive& p : gltf.meshes[*node.meshIndex].primitives) {
					// the image its material's colour comes from: textures/<stem>_diff_1k.jpg (the file's name without one)
					std::string stem = SPECIES_FILES[s].name;
					if(p.materialIndex && gltf.materials[*p.materialIndex].pbrData.baseColorTexture) {
						const fastgltf::Texture& t = gltf.textures[gltf.materials[*p.materialIndex].pbrData.baseColorTexture->textureIndex];
						if(t.imageIndex)
							if(const auto* uri = std::get_if<fastgltf::sources::URI>(&gltf.images[*t.imageIndex].data)) {
								std::string file = std::filesystem::path(std::string(uri->uri.path())).filename().string();
								if(size_t at = file.find("_diff"); at != std::string::npos) stem = file.substr(0, at);
							}
					}
					if(!meshload::readPrimitive(gltf, p, transform, prim)) continue;
					float pair = static_cast<float>(texturePair(stem));
					size_t base = vertices.size();
					vertices.resize(base + prim.positions.size());
					for(size_t i = 0; i < prim.positions.size(); i++) {
						vertices[base + i].positionSpecies = glm::vec4(prim.positions[i] * toWorld, static_cast<float>(s));
						vertices[base + i].normalHeight = glm::vec4(prim.normals[i], 0.0f);
						vertices[base + i].uv = glm::vec4(prim.uvs[i], pair, 0.0f);
					}
					uint32_t offset = static_cast<uint32_t>(base - first); // indices are relative to the mesh's vertexOffset
					for(uint32_t index : prim.indices) indices.push_back(index + offset);
				}
				};
				appendNode(node);
				mesh.indexCount = static_cast<uint32_t>(indices.size()) - mesh.firstIndex;
				// the pack's own LODs where it has them: LOD1 for the simplified slot, LOD2 for the far one and the shadows
				// (cards = 2: the LOD generation below leaves them alone)
				if(lodOf(n) == 0) {
					std::string base(node.name);
					base.resize(base.rfind("__LOD"));
					uint32_t found[3][2] = {}; // LOD1, LOD2
					for(int lod = 1; lod <= 2; lod++)
						for(size_t k = 0; k < gltf.nodes.size(); k++)
							if(gltf.nodes[k].meshIndex && std::string(gltf.nodes[k].name) == base + "__LOD" + std::to_string(lod)) {
								found[lod][0] = static_cast<uint32_t>(indices.size());
								appendNode(gltf.nodes[k]);
								found[lod][1] = static_cast<uint32_t>(indices.size()) - found[lod][0];
								break;
							}
					if(found[1][1]) {
						if(!found[2][1]) { found[2][0] = found[1][0]; found[2][1] = found[1][1]; }
						mesh.firstIndex1 = found[1][0]; mesh.indexCount1 = found[1][1];
						mesh.firstIndex2 = found[2][0]; mesh.indexCount2 = found[2][1];
						mesh.firstIndexS = found[2][0]; mesh.indexCountS = found[2][1];
						mesh.cards = 2u;
					}
				}
				mesh.vertexCount = static_cast<uint32_t>(vertices.size() - first);
				size_t cap = file.decimateTo ? file.decimateTo : MAX_TRIANGLES;
				if(!DEBUG_NO_SIMPLIFY && mesh.indexCount / 3 > cap) {
					// too dense (file.decimate): simplified towards the cap, carefully (it may stay a bit over)
					size_t target = cap * 3;
					std::vector<uint32_t> lod0(mesh.indexCount);
					if(file.cards) {
						// (this mesh's vertices are the end of the buffer: the grown cards append right after them)
						std::vector<Vertex> local(vertices.begin() + first, vertices.end());
						lod0 = thinCards(local, indices.data() + mesh.firstIndex, mesh.indexCount, cap, 0u);
						vertices.resize(first);
						vertices.insert(vertices.end(), local.begin(), local.end());
						mesh.vertexCount = static_cast<uint32_t>(local.size());
					} else
						lod0.resize(meshload::simplifyCareful(lod0.data(), indices.data() + mesh.firstIndex, mesh.indexCount, &vertices[first].positionSpecies.x,
													mesh.vertexCount, sizeof(Vertex), target, 0.05f, 0.15f, false));
					indices.resize(mesh.firstIndex);
					// and only the vertices it still uses (the dense downloads kept hundreds of thousands unused: ~150MB)
					std::vector<uint32_t> remap(mesh.vertexCount);
					size_t used = meshopt_optimizeVertexFetchRemap(remap.data(), lod0.data(), lod0.size(), mesh.vertexCount);
					std::vector<Vertex> compact(used);
					meshopt_remapVertexBuffer(compact.data(), &vertices[first], mesh.vertexCount, sizeof(Vertex), remap.data());
					meshopt_remapIndexBuffer(lod0.data(), lod0.data(), lod0.size(), remap.data());
					vertices.resize(first);
					vertices.insert(vertices.end(), compact.begin(), compact.end());
					mesh.vertexCount = static_cast<uint32_t>(used);
					indices.insert(indices.end(), lod0.begin(), lod0.end());
					mesh.indexCount = static_cast<uint32_t>(lod0.size());
				}
				// standing on its lowest point, the height up the plant for the wind and the AO
				float bottom = 1e9f, top = -1e9f, radius = 0.0f;
				for(size_t i = first; i < vertices.size(); i++) {
					bottom = std::min(bottom, vertices[i].positionSpecies.y);
					top = std::max(top, vertices[i].positionSpecies.y);
				}
				float ground = std::max(bottom, -0.05f * toWorld); // a few roots below 0 are fine, the variants stand at 0
				for(size_t i = first; i < vertices.size(); i++) {
					Vertex& v = vertices[i];
					v.normalHeight.w = glm::clamp((v.positionSpecies.y - ground) / std::max(top - ground, 1e-5f), 0.0f, 1.0f);
					radius = std::max(radius, glm::length(glm::vec3(v.positionSpecies)));
				}

				if(mesh.cards != 2u) mesh.cards = file.cards ? 1u : 0u;
				m_Meshes.push_back(mesh);
				species[s].meshes.y++;
				species[s].size.x = std::max(species[s].size.x, radius);
			}
			// lightest first: groundcover.comp lets the dense upper half reach less far
			std::sort(m_Meshes.begin() + species[s].meshes.x, m_Meshes.end(), [](const Mesh& a, const Mesh& b) { return a.indexCount < b.indexCount; });

		}
		for(uint32_t v = 0; v < SPECIES - FILES; v++) species[FILES + v] = species[VARIANT_OF[v]]; // the same meshes

		// the simplified LOD of every plant: ~30% of the triangles within a small error (it stops early where leaves would
		// go missing), for the plants that are small on screen (groundcover.comp) and for the shadows. drawn at full
		// detail out to their reach, ~190k triangles over thousands of plants were a good part of a meadow's frame
		for(Mesh& mesh : m_Meshes) {
			if(mesh.cards == 2u) continue; // (authored LODs, set when it was loaded)
			if(DEBUG_NO_SIMPLIFY) {
				// (copies of the full index list: the GPU reordering below remaps each list once)
				uint32_t* firsts[3] = { &mesh.firstIndex1, &mesh.firstIndex2, &mesh.firstIndexS };
				uint32_t* counts[3] = { &mesh.indexCount1, &mesh.indexCount2, &mesh.indexCountS };
				std::vector<uint32_t> full(indices.begin() + mesh.firstIndex, indices.begin() + mesh.firstIndex + mesh.indexCount);
				for(int l = 0; l < 3; l++) {
					*firsts[l] = static_cast<uint32_t>(indices.size());
					*counts[l] = mesh.indexCount;
					indices.insert(indices.end(), full.begin(), full.end());
				}
				continue;
			}
			if(mesh.cards) {
				// thinned like LOD0 (thinCards), each LOD its own pick of cards; the grown copies need vertices of their
				// own, so the mesh moves to the end of the buffer with them (the old range is compacted away below)
				std::vector<Vertex> local(vertices.begin() + mesh.vertexOffset, vertices.begin() + mesh.vertexOffset + mesh.vertexCount);
				const uint32_t* lod0 = indices.data() + mesh.firstIndex;
				size_t tris = mesh.indexCount / 3;
				std::vector<uint32_t> lists[3] = { thinCards(local, lod0, mesh.indexCount, tris * 3 / 10, 1u), thinCards(local, lod0, mesh.indexCount, tris * 8 / 100, 2u),
												   thinCards(local, lod0, mesh.indexCount, tris * 12 / 100, 3u) };
				uint32_t* firsts[3] = { &mesh.firstIndex1, &mesh.firstIndex2, &mesh.firstIndexS };
				uint32_t* counts[3] = { &mesh.indexCount1, &mesh.indexCount2, &mesh.indexCountS };
				for(int l = 0; l < 3; l++) {
					*firsts[l] = static_cast<uint32_t>(indices.size());
					*counts[l] = static_cast<uint32_t>(lists[l].size());
					indices.insert(indices.end(), lists[l].begin(), lists[l].end());
				}
				mesh.vertexOffset = static_cast<int32_t>(vertices.size());
				mesh.vertexCount = static_cast<uint32_t>(local.size());
				vertices.insert(vertices.end(), local.begin(), local.end());
				continue;
			}
			std::vector<uint32_t> simplified(mesh.indexCount);
			size_t target = std::max<size_t>(mesh.indexCount * 3 / 10 / 3 * 3, 3);
			float error = 0.0f;
			simplified.resize(meshopt_simplify(simplified.data(), indices.data() + mesh.firstIndex, mesh.indexCount, &vertices[mesh.vertexOffset].positionSpecies.x,
											   mesh.vertexCount, sizeof(Vertex), target, 0.02f, 0, &error));
			if(simplified.empty()) simplified.assign(indices.begin() + mesh.firstIndex, indices.begin() + mesh.firstIndex + mesh.indexCount);
			mesh.firstIndex1 = static_cast<uint32_t>(indices.size());
			mesh.indexCount1 = static_cast<uint32_t>(simplified.size());
			indices.insert(indices.end(), simplified.begin(), simplified.end());
			// LOD 2: ~8% for the plants a few pixels across (a higher error and its tiny loose pieces dropped if it must)
			std::vector<uint32_t> far(mesh.indexCount);
			size_t farTarget = std::max<size_t>(mesh.indexCount * 8 / 100 / 3 * 3, 3);
			far.resize(meshload::simplifyCareful(far.data(), indices.data() + mesh.firstIndex, mesh.indexCount, &vertices[mesh.vertexOffset].positionSpecies.x,
									   mesh.vertexCount, sizeof(Vertex), farTarget, 0.2f, 0.5f, true));
			if(far.empty()) far.assign(simplified.begin(), simplified.end());
			mesh.firstIndex2 = static_cast<uint32_t>(indices.size());
			mesh.indexCount2 = static_cast<uint32_t>(far.size());
			indices.insert(indices.end(), far.begin(), far.end());
			// the shadow casters' LOD, ~12%: the simplified one (30%) made the casters as many triangles as the camera's
			// plants. a shadow is a dark blob a few pixels from the plant, its leaves' outline isn't in it
			std::vector<uint32_t> shadow(mesh.indexCount);
			size_t shadowTarget = std::max<size_t>(mesh.indexCount * 12 / 100 / 3 * 3, 3);
			shadow.resize(meshload::simplifyCareful(shadow.data(), indices.data() + mesh.firstIndex, mesh.indexCount, &vertices[mesh.vertexOffset].positionSpecies.x,
										  mesh.vertexCount, sizeof(Vertex), shadowTarget, 0.1f, 0.4f, true));
			if(shadow.empty()) shadow.assign(simplified.begin(), simplified.end());
			mesh.firstIndexS = static_cast<uint32_t>(indices.size());
			mesh.indexCountS = static_cast<uint32_t>(shadow.size());
			indices.insert(indices.end(), shadow.begin(), shadow.end());
		}
		// for the GPU: each index list in the order its post transform cache likes (and then, within that, the least
		// overdraw), the vertices in the order the full LOD first uses them; the LODs index the same vertices
		for(Mesh& mesh : m_Meshes) {
			const float* positions = &vertices[mesh.vertexOffset].positionSpecies.x;
			uint32_t lists[4][2] = { { mesh.firstIndex, mesh.indexCount }, { mesh.firstIndex1, mesh.indexCount1 }, { mesh.firstIndex2, mesh.indexCount2 }, { mesh.firstIndexS, mesh.indexCountS } };
			for(auto& list : lists) {
				uint32_t* p = indices.data() + list[0];
				meshopt_optimizeVertexCache(p, p, list[1], mesh.vertexCount);
				meshopt_optimizeOverdraw(p, p, list[1], positions, mesh.vertexCount, sizeof(Vertex), 1.05f);
			}
			// (over all four lists in a row: the full LOD's first, so its vertices come first; the thinned LODs of the
			// card plants use grown copies the full one doesn't)
			std::vector<uint32_t> all;
			for(auto& list : lists) all.insert(all.end(), indices.begin() + list[0], indices.begin() + list[0] + list[1]);
			std::vector<uint32_t> remap(mesh.vertexCount);
			size_t used = meshopt_optimizeVertexFetchRemap(remap.data(), all.data(), all.size(), mesh.vertexCount);
			std::vector<Vertex> ordered(vertices.begin() + mesh.vertexOffset, vertices.begin() + mesh.vertexOffset + mesh.vertexCount);
			meshopt_remapVertexBuffer(ordered.data(), &vertices[mesh.vertexOffset], mesh.vertexCount, sizeof(Vertex), remap.data());
			std::copy(ordered.begin(), ordered.begin() + used, vertices.begin() + mesh.vertexOffset);
			mesh.vertexCount = static_cast<uint32_t>(used);
			for(auto& list : lists) meshopt_remapIndexBuffer(indices.data() + list[0], indices.data() + list[0], list[1], remap.data());
		}
		{
			// the buffer compacted: every mesh's used vertices back to back (the card plants left their old ranges behind)
			std::vector<Vertex> packed;
			for(Mesh& mesh : m_Meshes) {
				int32_t offset = static_cast<int32_t>(packed.size());
				packed.insert(packed.end(), vertices.begin() + mesh.vertexOffset, vertices.begin() + mesh.vertexOffset + mesh.vertexCount);
				mesh.vertexOffset = offset;
			}
			vertices.swap(packed);
		}
		if(std::ofstream out{ cachePath, std::ios::binary }) {
			uint32_t counts[4] = { static_cast<uint32_t>(vertices.size()), static_cast<uint32_t>(indices.size()), static_cast<uint32_t>(m_Meshes.size()), static_cast<uint32_t>(stems.size()) };
			out.write(reinterpret_cast<const char*>(&cacheKey), sizeof(cacheKey));
			out.write(reinterpret_cast<const char*>(counts), sizeof(counts));
			out.write(reinterpret_cast<const char*>(vertices.data()), vertices.size() * sizeof(Vertex));
			out.write(reinterpret_cast<const char*>(indices.data()), indices.size() * sizeof(uint32_t));
			out.write(reinterpret_cast<const char*>(m_Meshes.data()), m_Meshes.size() * sizeof(Mesh));
			out.write(reinterpret_cast<const char*>(species), sizeof(species));
			for(const auto& [s, stem] : stems) {
				uint32_t length = static_cast<uint32_t>(stem.size());
				out.write(reinterpret_cast<const char*>(&s), sizeof(s));
				out.write(reinterpret_cast<const char*>(&length), sizeof(length));
				out.write(stem.data(), length);
			}
		}
	}
	fmt::print("ground cover: meshes {} in {:.0f} ms\n", cached ? "from the cache" : "processed (and cached)",
			   std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count());
	if(vertices.empty()) vertices.resize(1), indices.resize(3, 0); // nothing found: keep the buffers valid

	size_t vertexBytes = vertices.size() * sizeof(Vertex), indexBytes = indices.size() * sizeof(uint32_t);
	m_VertexBuffer = engine->createBuffer(vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	m_IndexBuffer = engine->createBuffer(indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	m_SpeciesBuffer = engine->createBuffer(sizeof(species), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	AllocatedBuffer staging = engine->createBuffer(vertexBytes + indexBytes + sizeof(species), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	memcpy(staging.info.pMappedData, vertices.data(), vertexBytes);
	memcpy(static_cast<uint8_t*>(staging.info.pMappedData) + vertexBytes, indices.data(), indexBytes);
	memcpy(static_cast<uint8_t*>(staging.info.pMappedData) + vertexBytes + indexBytes, species, sizeof(species));
	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		VkBufferCopy vertexCopy{ 0, 0, vertexBytes };
		VkBufferCopy indexCopy{ vertexBytes, 0, indexBytes };
		VkBufferCopy speciesCopy{ vertexBytes + indexBytes, 0, sizeof(species) };
		vkCmdCopyBuffer(cmd, staging.buffer, m_VertexBuffer.buffer, 1, &vertexCopy);
		vkCmdCopyBuffer(cmd, staging.buffer, m_IndexBuffer.buffer, 1, &indexCopy);
		vkCmdCopyBuffer(cmd, staging.buffer, m_SpeciesBuffer.buffer, 1, &speciesCopy);
	});
	engine->destroyBuffer(staging);
	size_t lod1 = 0, lod2 = 0;
	size_t lod0 = 0, lodS = 0;
	for(const Mesh& mesh : m_Meshes) lod0 += mesh.indexCount / 3, lod1 += mesh.indexCount1 / 3, lod2 += mesh.indexCount2 / 3, lodS += mesh.indexCountS / 3;
	fmt::print("ground cover: {} meshes, {} vertices, {} triangles ({} simplified, {} far, {} shadow)\n", m_Meshes.size(), vertices.size(), lod0, lod1, lod2, lodS);

	// bins: one per mesh for the camera, then one per mesh for the shadow casters
	m_Instances = engine->createBuffer(BINS * CAPACITY * sizeof(glm::vec4) * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	m_Commands = engine->createBuffer(BINS * sizeof(VkDrawIndexedIndirectCommand), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	m_CommandTemplate.assign(BINS, VkDrawIndexedIndirectCommand{});
	for(uint32_t bin = 0; bin < BINS; bin++) {
		uint32_t mesh = bin % MAX_MESHES;
		m_CommandTemplate[bin].firstInstance = bin * CAPACITY;
		if(mesh >= m_Meshes.size()) continue;
		uint32_t block = bin / MAX_MESHES; // 0 the camera's lod 0, 1 the shadow casters and 2 the camera's lod 1 (simplified), 3 lod 2
		const Mesh& m = m_Meshes[mesh];
		m_CommandTemplate[bin].indexCount = block == 0 ? m.indexCount : (block == 1 ? m.indexCountS : (block == 2 ? m.indexCount1 : m.indexCount2));
		m_CommandTemplate[bin].firstIndex = block == 0 ? m.firstIndex : (block == 1 ? m.firstIndexS : (block == 2 ? m.firstIndex1 : m.firstIndex2));
		m_CommandTemplate[bin].vertexOffset = m_Meshes[mesh].vertexOffset;
	}

	// ---------------------------------------------------------------- descriptors
	DescriptorLayoutBuilder computeBuilder;
	computeBuilder.add_binding(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // heightmap
	computeBuilder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER); // climate map
	computeBuilder.add_binding(2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);		  // instances
	computeBuilder.add_binding(3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);		  // draw commands
	computeBuilder.add_binding(4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);		  // species
	m_ComputeLayout = computeBuilder.build(m_Device, VK_SHADER_STAGE_COMPUTE_BIT);
	m_ComputeSet = engine->g_DescriptorAllocator.allocate(m_Device, m_ComputeLayout);

	DescriptorLayoutBuilder renderBuilder;
	renderBuilder.add_binding(0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);							   // instances
	renderBuilder.add_binding(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_TEXTURE_PAIRS * 2); // albedo, normal per texture pair
	renderBuilder.add_binding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER); // the player's trail + the weather's wind (interaction.glsl)
	m_RenderLayout = renderBuilder.build(m_Device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	m_RenderSet = engine->g_DescriptorAllocator.allocate(m_Device, m_RenderLayout);

	VkDescriptorImageInfo mapInfos[2] = {
		{ heightmapSampler, heightmap.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
		{ heightmapSampler, climateMap.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
	};
	VkDescriptorBufferInfo instanceInfo{ m_Instances.buffer, 0, VK_WHOLE_SIZE };
	VkDescriptorBufferInfo commandInfo{ m_Commands.buffer, 0, VK_WHOLE_SIZE };
	VkDescriptorBufferInfo speciesInfo{ m_SpeciesBuffer.buffer, 0, VK_WHOLE_SIZE };
	VkDescriptorImageInfo textureInfos[MAX_TEXTURE_PAIRS * 2];
	fmt::print("ground cover: {} texture pairs\n", m_Textures.size() / 2);
	for(uint32_t i = 0; i < MAX_TEXTURE_PAIRS * 2; i++) // (the unused slots point at the first pair)
		textureInfos[i] = { textureSampler, m_Textures[i < m_Textures.size() ? i : i % 2].imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkDescriptorBufferInfo interactionInfo{ interactionBuffer, 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet writes[8] = {};
	for(auto& write : writes) { write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; write.descriptorCount = 1; write.dstSet = m_ComputeSet; write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; }
	writes[0].dstBinding = 0; writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[0].pImageInfo = &mapInfos[0];
	writes[1].dstBinding = 1; writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; writes[1].pImageInfo = &mapInfos[1];
	writes[2].dstBinding = 2; writes[2].pBufferInfo = &instanceInfo;
	writes[3].dstBinding = 3; writes[3].pBufferInfo = &commandInfo;
	writes[4].dstBinding = 4; writes[4].pBufferInfo = &speciesInfo;
	writes[5].dstSet = m_RenderSet; writes[5].dstBinding = 0; writes[5].pBufferInfo = &instanceInfo;
	writes[6].dstSet = m_RenderSet; writes[6].dstBinding = 1; writes[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	writes[6].descriptorCount = MAX_TEXTURE_PAIRS * 2; writes[6].pImageInfo = textureInfos;
	writes[7].dstSet = m_RenderSet; writes[7].dstBinding = 2; writes[7].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; writes[7].pBufferInfo = &interactionInfo;
	vkUpdateDescriptorSets(m_Device, 8, writes, 0, nullptr);

	// ---------------------------------------------------------------- pipelines
	VkPushConstantRange computeRange{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FoliagePushConstants) };
	VkPipelineLayoutCreateInfo computeLayoutCI = vkinit::pipeline_layout_create_info();
	computeLayoutCI.setLayoutCount = 1;
	computeLayoutCI.pSetLayouts = &m_ComputeLayout;
	computeLayoutCI.pushConstantRangeCount = 1;
	computeLayoutCI.pPushConstantRanges = &computeRange;
	VkPipelineLayout scatterLayout;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &computeLayoutCI, nullptr, &scatterLayout));
	m_Scatter = engine->loadComputeShader("src/shaders/bin/groundcover.comp.spv", "ground cover scatter", scatterLayout);

	VkPushConstantRange renderRange{ VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FoliagePushConstants) };
	VkPipelineLayoutCreateInfo renderLayoutCI = vkinit::pipeline_layout_create_info();
	renderLayoutCI.setLayoutCount = 1;
	renderLayoutCI.pSetLayouts = &m_RenderLayout;
	renderLayoutCI.pushConstantRangeCount = 1;
	renderLayoutCI.pPushConstantRanges = &renderRange;
	VK_CHECK(vkCreatePipelineLayout(m_Device, &renderLayoutCI, nullptr, &m_RenderPipelineLayout));

	VkVertexInputBindingDescription binding{ 0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX };
	std::vector<VkVertexInputAttributeDescription> attributes = {
		{ 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, positionSpecies) },
		{ 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, normalHeight) },
		{ 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, uv) },
	};
	CompiledShader vertexShader = engine->loadShader("src/shaders/bin/groundcover.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
	CompiledShader fragmentShader = engine->loadShader("src/shaders/bin/groundcover.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
	CompiledShader shadowShader = engine->loadShader("src/shaders/bin/groundcover_shadow.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);

	PipelineBuilder builder;
	builder._pipelineLayout = m_RenderPipelineLayout;
	builder.set_shaders(vertexShader.shader, fragmentShader.shader);
	builder.set_vertex_input({ binding }, attributes);
	builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
	builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE); // two sided leaves
	builder.set_multisampling_none();
	builder.disable_blending();
	builder.enable_depthtest(true, VK_COMPARE_OP_GREATER); // reversed z
	setGBufferTargets(builder, true);
	builder.set_depth_format(engine->m_DepthBuffer.imageFormat);
	m_RenderPipeline = builder.build_pipeline(m_Device);

	PipelineBuilder shadowBuilder;
	shadowBuilder._pipelineLayout = m_RenderPipelineLayout;
	shadowBuilder.set_shaders(vertexShader.shader, shadowShader.shader);
	shadowBuilder.set_vertex_input({ binding }, attributes);
	shadowBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	shadowBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
	shadowBuilder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE);
	shadowBuilder._rasterizer.depthBiasEnable = VK_TRUE;
	shadowBuilder._rasterizer.depthBiasConstantFactor = 2.0f;
	shadowBuilder._rasterizer.depthBiasSlopeFactor = 2.5f;
	shadowBuilder.set_multisampling_none();
	shadowBuilder.disable_blending();
	shadowBuilder.enable_depthtest(true, VK_COMPARE_OP_LESS); // shadow maps are regular z
	shadowBuilder.set_depth_format(shadowFormat);
	m_ShadowPipeline = shadowBuilder.build_pipeline(m_Device);

	vkDestroyShaderModule(m_Device, vertexShader.shader, nullptr);
	vkDestroyShaderModule(m_Device, fragmentShader.shader, nullptr);
	vkDestroyShaderModule(m_Device, shadowShader.shader, nullptr);
}

void GroundCover::destroy() {
	vkDestroyPipeline(m_Device, m_RenderPipeline, nullptr);
	vkDestroyPipeline(m_Device, m_ShadowPipeline, nullptr);
	vkDestroyPipelineLayout(m_Device, m_RenderPipelineLayout, nullptr);
	vkDestroyPipelineLayout(m_Device, m_Scatter.layout, nullptr);
	vkDestroyDescriptorSetLayout(m_Device, m_ComputeLayout, nullptr);
	vkDestroyDescriptorSetLayout(m_Device, m_RenderLayout, nullptr);
	for(AllocatedBuffer* buffer : { &m_VertexBuffer, &m_IndexBuffer, &m_Instances, &m_Commands, &m_SpeciesBuffer })
		vmaDestroyBuffer(m_Allocator, buffer->buffer, buffer->allocation);
	for(const AllocatedImage& image : m_Textures) m_TextureLoader->destroyImage(image);
	m_Textures.clear();
}

// ---------------------------------------------------------------- per frame

void GroundCover::update(VkCommandBuffer cmd, const FoliagePushConstants& pc) {
	m_PushConstants = pc;
	m_PushConstants.extra = m_Density; // groundcover.comp: pc.interaction
	if(!m_Enabled || m_Meshes.empty()) return;
	const VkPipelineStageFlags2 drawStages = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
	const VkAccessFlags2 drawReads = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;

	vkutil::memoryBarrier(cmd, drawStages, drawReads, VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
	vkCmdUpdateBuffer(cmd, m_Commands.buffer, 0, m_CommandTemplate.size() * sizeof(VkDrawIndexedIndirectCommand), m_CommandTemplate.data());
	vkutil::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Scatter.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Scatter.layout, 0, 1, &m_ComputeSet, 0, nullptr);
	vkCmdPushConstants(cmd, m_Scatter.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FoliagePushConstants), &m_PushConstants);
	vkCmdDispatch(cmd, (THREADS + 63) / 64, 1, 1);

	vkutil::memoryBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, drawStages, drawReads);
}

void GroundCover::draw(VkCommandBuffer cmd) {
	if(!m_Enabled || m_Meshes.empty()) return;
	vkutil::bindGraphics(cmd, m_RenderPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_RenderPipelineLayout, 0, 1, &m_RenderSet, 0, nullptr);
	vkCmdPushConstants(cmd, m_RenderPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FoliagePushConstants), &m_PushConstants);
	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &m_VertexBuffer.buffer, &offset);
	vkCmdBindIndexBuffer(cmd, m_IndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
	vkCmdDrawIndexedIndirect(cmd, m_Commands.buffer, 0, static_cast<uint32_t>(m_Meshes.size()), sizeof(VkDrawIndexedIndirectCommand));
	vkCmdDrawIndexedIndirect(cmd, m_Commands.buffer, MAX_MESHES * 2 * sizeof(VkDrawIndexedIndirectCommand), static_cast<uint32_t>(m_Meshes.size()), sizeof(VkDrawIndexedIndirectCommand)); // lod 1
	vkCmdDrawIndexedIndirect(cmd, m_Commands.buffer, MAX_MESHES * 3 * sizeof(VkDrawIndexedIndirectCommand), static_cast<uint32_t>(m_Meshes.size()), sizeof(VkDrawIndexedIndirectCommand)); // lod 2
}

void GroundCover::drawShadow(VkCommandBuffer cmd, const glm::mat4& lightViewProj) {
	if(!m_Enabled || m_Meshes.empty()) return;
	FoliagePushConstants pc = m_PushConstants;
	pc.viewproj = lightViewProj;
	vkutil::bindGraphics(cmd, m_ShadowPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_RenderPipelineLayout, 0, 1, &m_RenderSet, 0, nullptr);
	vkCmdPushConstants(cmd, m_RenderPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FoliagePushConstants), &pc);
	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &m_VertexBuffer.buffer, &offset);
	vkCmdBindIndexBuffer(cmd, m_IndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
	vkCmdDrawIndexedIndirect(cmd, m_Commands.buffer, MAX_MESHES * sizeof(VkDrawIndexedIndirectCommand), static_cast<uint32_t>(m_Meshes.size()), sizeof(VkDrawIndexedIndirectCommand));
}
