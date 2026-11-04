#include "trees.hpp"
#include "gbuffer.hpp"
#include "application.hpp"
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <random>
#include <functional>

// ------------------------------------------------------------------ procedural meshes

namespace {
	enum Material { BarkConifer = 0, BarkBroadleaf = 1, LeavesConifer = 2,LeavesBroadleaf = 3 };

	struct MeshBuilder {
		std::vector<TreeVertex>& vertices;
		std::vector<uint32_t>& indices;
		std::mt19937 rng;
		// the leaf atlases hold two branch photos side by side (tools/make_leaf_atlas.py): 0 = left, 1 = right
		int leafTile = 0;
		// the tree type's colouring (treecommon.glsl, treeTint): 1 spruce, 2 pine, 3 oak, 4 autumn bush, 5 birch, 6 bush
		float tint = 0.0f;
		// how much of the sky a bark vertex sees through the crown around it (1 = open), set by each species: the
		// trunk and the branches inside a crown are as hidden as the leaves at its core, not lit like a bare pole
		std::function<float(glm::vec3)> crownShade = [](glm::vec3) { return 1.0f; };

		float random(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }

		uint32_t vertex(glm::vec3 p, float wind, glm::vec3 n, Material m, glm::vec2 uv, float ao) {
			if(m == LeavesConifer || m == LeavesBroadleaf) uv.x = (uv.x + static_cast<float>(leafTile)) * 0.5f;
			vertices.push_back({ glm::vec4(p, wind), glm::vec4(glm::normalize(n), static_cast<float>(m)), glm::vec4(uv, ao, tint) });
			return static_cast<uint32_t>(vertices.size() - 1);
		}

		// a card from `start` along `along` (full length), `side` is half the width. normals bent towards `normal`
		void card(glm::vec3 start, glm::vec3 along, glm::vec3 side, glm::vec3 normal, Material m, float windStart, float windEnd, float aoStart, float aoEnd) {
			uint32_t a = vertex(start - side, windStart, normal, m, {0.0f, 1.0f}, aoStart);
			uint32_t b = vertex(start + side, windStart, normal, m, {0.0f, 0.0f}, aoStart);
			uint32_t c = vertex(start + along + side, windEnd, normal, m, {1.0f, 0.0f}, aoEnd);
			uint32_t d = vertex(start + along - side, windEnd, normal, m, {1.0f, 1.0f}, aoEnd);
			indices.insert(indices.end(), { a, b, c, a, c, d });
		}

		// a centred card for leaf clusters (uv 0..1 over the whole cluster texture)
		void cluster(glm::vec3 center, glm::vec3 right, glm::vec3 up, glm::vec3 normal, Material m, float wind, float ao) {
			uint32_t a = vertex(center - right - up, wind, normal, m, {0.0f, 1.0f}, ao);
			uint32_t b = vertex(center + right - up, wind, normal, m, {1.0f, 1.0f}, ao);
			uint32_t c = vertex(center + right + up, wind, normal, m, {1.0f, 0.0f}, ao);
			uint32_t d = vertex(center - right + up, wind, normal, m, {0.0f, 0.0f}, ao);
			indices.insert(indices.end(), { a, b, c, a, c, d });
		}

		// tapered tube, bark uv wraps around once and repeats along the length
		void tube(glm::vec3 base, glm::vec3 top, float r0, float r1, int sides, int segments, Material m, float wind0, float wind1) {
			glm::vec3 axis = glm::normalize(top - base);
			glm::vec3 tangent = glm::normalize(glm::cross(axis, std::abs(axis.y) < 0.99f ? glm::vec3(0, 1, 0) : glm::vec3(1, 0, 0)));
			glm::vec3 bitangent = glm::cross(axis, tangent);
			float length = glm::length(top - base);
			uint32_t first = static_cast<uint32_t>(vertices.size());
			for(int s = 0; s <= segments; s++) {
				float t = static_cast<float>(s) / segments;
				glm::vec3 center = glm::mix(base, top, t);
				float r = glm::mix(r0, r1, t);
				for(int i = 0; i <= sides; i++) {
					float a = glm::two_pi<float>() * i / sides;
					glm::vec3 n = tangent * std::cos(a) + bitangent * std::sin(a);
					glm::vec3 p = center + n * r;
					vertex(p, glm::mix(wind0, wind1, t), n, m, { static_cast<float>(i) / sides, t * length / (r0 * 6.0f) }, glm::mix(0.6f, 1.0f, t) * crownShade(p));
				}
			}
			for(int s = 0; s < segments; s++) {
				for(int i = 0; i < sides; i++) {
					uint32_t a = first + s * (sides + 1) + i, b = a + 1, c = a + sides + 1, d = c + 1;
					indices.insert(indices.end(), { a, c, b, b, c, d });
				}
			}
		}

		glm::vec3 randomDirection() {
			float z = random(-1.0f, 1.0f), a = random(0.0f, glm::two_pi<float>());
			float r = std::sqrt(1.0f - z * z);
			return { r * std::cos(a), z, r * std::sin(a) };
		}

		// ------------------------------------------------------------------ species

		// spruce: a trunk with whorls of branches, longest at the bottom, each a photo of a whole spruce branch (side
		// twigs and all) sagging a little, twice: flat and rolled around the branch so it has volume from the side.
		// an inner ring of shorter branches half a step around fills the crown
		void conifer(bool lod) {
			leafTile = 0;
			// the branches start just above the ground and close over the trunk all the way up, the top opens a little
			crownShade = [](glm::vec3 p) { return glm::mix(1.0f, glm::mix(0.3f, 0.6f, glm::clamp(p.y, 0.0f, 1.0f)), glm::smoothstep(0.05f, 0.15f, p.y)); };
			tube({0, 0, 0}, {0, 0.97f, 0}, 0.022f, 0.003f, lod ? 4 : 7, lod ? 1 : 3, BarkConifer, 0.0f, 0.4f);
			int whorls = lod ? 10 : 16;
			for(int w = 0; w < whorls; w++) {
				float t = static_cast<float>(w) / (whorls - 1);
				float h = 0.11f + 0.8f * std::pow(t, 0.9f) + random(-0.012f, 0.012f);
				float length = (0.38f * std::pow(1.0f - (h - 0.1f) / 0.9f, 0.85f) + 0.06f) * random(0.88f, 1.12f) * (lod ? 1.1f : 1.0f);
				int branches = lod ? 5 : 6;
				float offset = random(0.0f, glm::two_pi<float>());
				for(int ring = 0; ring < 2; ring++) {
					if(lod && ring == 1) break; // the far version keeps the outer silhouette only
					float ringLength = length * (ring == 0 ? 1.0f : 0.6f);
					float ringOffset = offset + (ring == 0 ? 0.0f : glm::pi<float>() / branches);
					for(int b = 0; b < branches; b++) {
						float angle = ringOffset + glm::two_pi<float>() * b / branches + random(-0.25f, 0.25f);
						glm::vec3 out(std::cos(angle), 0.0f, std::sin(angle));
						glm::vec3 start(0.0f, h + (ring == 1 ? 0.02f : 0.0f), 0.0f);
						float ao = glm::mix(0.45f, 1.0f, h) * (ring == 1 ? 0.75f : 1.0f); // the inside is darker
						glm::vec3 along = glm::normalize(out + glm::vec3(0.0f, -random(0.15f, 0.35f), 0.0f)) * ringLength;
						glm::vec3 side = glm::normalize(glm::cross(along, glm::vec3(0, 1, 0))) * ringLength * 0.42f;
						glm::vec3 normal = glm::normalize(out * 0.6f + glm::vec3(0, 0.8f, 0));
						card(start, along, side, normal, LeavesConifer, h * 0.5f, h * 0.6f + 0.4f, ao * 0.6f, ao);
						glm::vec3 rolled = glm::normalize(glm::cross(along, side)) * glm::length(side) * 0.8f;
						card(start, along, side * 0.4f + rolled, normal, LeavesConifer, h * 0.5f, h * 0.6f + 0.4f, ao * 0.6f, ao);
					}
				}
			}
			// the tip
			for(int i = 0; i < 2; i++) {
				glm::vec3 side = glm::vec3(i == 0 ? 0.05f : 0.0f, 0.0f, i == 0 ? 0.0f : 0.05f);
				card({0.0f, 0.82f, 0.0f}, {0.0f, 0.18f, 0.0f}, side, {side.x, 0.5f, side.z}, LeavesConifer, 0.9f, 1.0f, 0.9f, 1.0f);
			}
		}

		// pine: a tall bare trunk, the crown only in the top third, branches curving up with the pine branch photo on
		// them, a few dead stubs lower down
		void pine(bool lod) {
			leafTile = 1;
			crownShade = [](glm::vec3 p) { return glm::mix(1.0f, 0.45f, glm::smoothstep(0.55f, 0.7f, p.y)); }; // the crown is the top third
			glm::vec3 lean(random(-0.03f, 0.03f), 0.0f, random(-0.03f, 0.03f));
			glm::vec3 mid = glm::vec3(0.0f, 0.62f, 0.0f) + lean, top = glm::vec3(0.0f, 0.98f, 0.0f) + lean * 1.5f;
			tube({0, 0, 0}, mid, 0.026f, 0.016f, lod ? 4 : 7, lod ? 1 : 2, BarkConifer, 0.0f, 0.3f);
			tube(mid, top, 0.016f, 0.004f, lod ? 3 : 6, 1, BarkConifer, 0.3f, 0.6f);
			if(!lod) for(int i = 0; i < 6; i++) { // dead stubs
				float h = random(0.25f, 0.55f), a = random(0.0f, glm::two_pi<float>());
				glm::vec3 base = glm::mix(glm::vec3(0.0f), mid, h / 0.62f);
				tube(base, base + glm::vec3(std::cos(a), random(-0.1f, 0.2f), std::sin(a)) * random(0.04f, 0.08f), 0.004f, 0.001f, 3, 1, BarkConifer, 0.0f, 0.1f);
			}
			int whorls = lod ? 5 : 8;
			for(int w = 0; w < whorls; w++) {
				float k = static_cast<float>(w) / (whorls - 1);
				float h = 0.6f + 0.34f * k + random(-0.02f, 0.02f);
				glm::vec3 centre = glm::mix(mid, top, (h - 0.62f) / 0.36f);
				int branches = lod ? 3 : 5;
				float offset = random(0.0f, glm::two_pi<float>());
				for(int b = 0; b < branches; b++) {
					float angle = offset + glm::two_pi<float>() * b / branches + random(-0.35f, 0.35f);
					glm::vec3 out(std::cos(angle), 0.0f, std::sin(angle));
					float length = glm::mix(0.26f, 0.12f, k) * random(0.8f, 1.2f);
					glm::vec3 along = glm::normalize(out + glm::vec3(0.0f, random(0.25f, 0.6f), 0.0f)) * length;
					if(!lod) tube(centre, centre + along * 0.8f, 0.007f, 0.002f, 3, 1, BarkConifer, h * 0.5f, h * 0.7f);
					glm::vec3 side = glm::normalize(glm::cross(along, glm::vec3(0, 1, 0))) * length * 0.55f;
					glm::vec3 normal = glm::normalize(out * 0.5f + glm::vec3(0, 0.9f, 0));
					float ao = glm::mix(0.7f, 1.0f, k);
					card(centre, along * 1.15f, side, normal, LeavesConifer, h * 0.5f, h * 0.7f + 0.3f, ao * 0.7f, ao);
					glm::vec3 rolled = glm::normalize(glm::cross(along, side)) * glm::length(side) * 0.8f;
					card(centre, along * 1.15f, side * 0.4f + rolled, normal, LeavesConifer, h * 0.5f, h * 0.7f + 0.3f, ao * 0.7f, ao);
				}
			}
		}

		// a branching skeleton: a slightly leaning trunk, primary branches spiralling up out of it, secondary branches
		// off those, and leafy branch cards growing outwards along the secondaries (each card is a photo of a whole
		// leafy branch). the crown gets the clumpy silhouette of a real tree. card normals point away from the crown's
		// centre so it shades as one volume, a few darker cards fill the core. birch: slender, taller, a narrow airy
		// crown with drooping twigs, white bark (treeTint)
		void broadleaf(bool lod, bool birch) {
			leafTile = 0;
			glm::vec3 lean(random(-0.04f, 0.04f), 0.0f, random(-0.04f, 0.04f));
			float trunkHeight = birch ? 0.6f : 0.46f;
			glm::vec3 t1 = glm::vec3(0.0f, trunkHeight * 0.55f, 0.0f) + lean * 0.5f, t2 = glm::vec3(0.0f, trunkHeight, 0.0f) + lean;
			float r0 = birch ? 0.022f : 0.038f;
			tube({0, 0, 0}, t1, r0, r0 * 0.8f, lod ? 4 : 8, 1, BarkBroadleaf, 0.0f, 0.1f);
			tube(t1, t2, r0 * 0.8f, r0 * 0.55f, lod ? 4 : 8, 1, BarkBroadleaf, 0.1f, 0.25f);
			if(birch) tube(t2, t2 + glm::vec3(lean.x, 0.3f, lean.z), r0 * 0.55f, 0.003f, lod ? 3 : 5, 1, BarkBroadleaf, 0.25f, 0.6f); // the leader
			glm::vec3 crown = t2 + glm::vec3(random(-0.03f, 0.03f), birch ? 0.14f : 0.2f, random(-0.03f, 0.03f));
			glm::vec3 radii = birch ? glm::vec3(0.25f, 0.3f, 0.25f) * random(0.85f, 1.15f) : glm::vec3(0.37f * random(0.85f, 1.15f), 0.28f * random(0.85f, 1.15f), 0.37f * random(0.85f, 1.15f));

			auto keepInCrown = [&](glm::vec3 p) {
				glm::vec3 q = (p - crown) / radii;
				float l = glm::length(q);
				return l > 1.0f ? crown + q / l * radii : p;
			};
			// like the leaf cards: darker towards the crown's core, open outside it (a little shade just under it)
			crownShade = [crown, radii](glm::vec3 p) {
				float r = glm::length((p - crown) / radii);
				return glm::mix(0.35f, 1.0f, glm::smoothstep(0.3f, 1.25f, r));
			};
			auto leafCard = [&](glm::vec3 centre, float size, float shade) {
				glm::vec3 q = (centre - crown) / radii;
				float r = glm::clamp(glm::length(q), 0.0f, 1.0f);
				glm::vec3 radial = glm::length(q) > 1e-4f ? glm::normalize(q) : glm::vec3(0, 1, 0);
				// the branch photo grows outwards (its base towards the crown's inside), birch twigs hang down
				glm::vec3 grow = birch ? glm::normalize(radial * 0.6f + glm::vec3(0.0f, -0.7f, 0.0f) + randomDirection() * 0.3f)
									   : glm::normalize(radial * 0.8f + glm::vec3(0.0f, 0.25f, 0.0f) + randomDirection() * 0.35f);
				glm::vec3 right = glm::normalize(glm::cross(grow, randomDirection()));
				float ao = glm::mix(0.45f, 1.0f, r) * glm::mix(0.75f, 1.0f, radial.y * 0.5f + 0.5f) * shade;
				cluster(centre + grow * size * 0.6f, right * size, grow * size, radial, LeavesBroadleaf, 0.55f + 0.45f * r, ao);
			};

			int primaries = birch ? (lod ? 5 : 8) : (lod ? 4 : 6);
			float spiral = random(0.0f, glm::two_pi<float>());
			for(int b = 0; b < primaries; b++) {
				float k = (b + random(0.0f, 0.8f)) / primaries;
				glm::vec3 base = birch ? glm::mix(t2, t2 + glm::vec3(0.0f, 0.26f, 0.0f), k) : glm::mix(t1, t2, 0.35f + 0.65f * k);
				float angle = spiral + b * 2.39996f + random(-0.3f, 0.3f); // golden angle: no two branches on one side
				glm::vec3 dir = glm::normalize(glm::vec3(std::cos(angle), birch ? random(0.4f, 0.9f) : random(0.55f, 1.15f), std::sin(angle)));
				float length = birch ? random(0.16f, 0.24f) : random(0.24f, 0.34f);
				glm::vec3 mid = keepInCrown(base + dir * length * 0.55f + glm::vec3(0.0f, 0.03f, 0.0f));
				glm::vec3 end = keepInCrown(base + dir * length + glm::vec3(0.0f, birch ? -0.02f : 0.06f, 0.0f));
				float br = birch ? 0.006f : 0.014f;
				tube(base, mid, br, br * 0.65f, lod ? 3 : 5, 1, BarkBroadleaf, 0.25f, 0.45f);
				tube(mid, end, br * 0.65f, br * 0.3f, lod ? 3 : 4, 1, BarkBroadleaf, 0.45f, 0.7f);

				int secondaries = lod ? 2 : 3;
				for(int s = 0; s < secondaries; s++) {
					float along = random(0.35f, 1.0f);
					glm::vec3 from = along < 0.55f ? glm::mix(base, mid, along / 0.55f) : glm::mix(mid, end, (along - 0.55f) / 0.45f);
					glm::vec3 sdir = glm::normalize(dir + randomDirection() * 0.75f + glm::vec3(0.0f, birch ? -0.35f : 0.2f, 0.0f));
					glm::vec3 to = keepInCrown(from + sdir * random(0.1f, 0.17f));
					if(!lod) tube(from, to, 0.004f, 0.0015f, 3, 1, BarkBroadleaf, 0.6f, 0.85f);
					for(int c = 0; c < (lod ? 2 : 3); c++)
						leafCard(glm::mix(from, to, random(0.3f, 1.0f)) + randomDirection() * 0.03f, (birch ? random(0.11f, 0.15f) : random(0.15f, 0.21f)) * (lod ? 1.45f : 1.0f), 1.0f);
				}
				leafCard(end, (birch ? 0.14f : random(0.17f, 0.22f)) * (lod ? 1.45f : 1.0f), 1.0f);
			}
			// the core: a few darker cards so the crown isn't see-through
			for(int i = 0; i < (lod ? 4 : (birch ? 5 : 9)); i++) {
				glm::vec3 centre = crown + randomDirection() * radii * std::sqrt(random(0.0f, 0.45f));
				leafCard(centre, random(0.16f, 0.22f) * (lod ? 1.4f : 1.0f), 0.8f);
			}
		}

		// no trunk, a squashed ball of leaf clusters sitting on the ground
		void bush(bool lod) {
			glm::vec3 center(0.0f, 0.42f, 0.0f);
			glm::vec3 radii(0.5f * random(0.8f, 1.2f), 0.42f, 0.5f * random(0.8f, 1.2f));
			int clusters = lod ? 10 : 28;
			for(int i = 0; i < clusters; i++) {
				glm::vec3 dir = randomDirection();
				if(dir.y < -0.3f) dir.y = -dir.y; // nothing underground
				float r = std::sqrt(random(0.35f, 1.0f));
				glm::vec3 c = center + dir * radii * r;
				// the branch photo grows outwards from the middle of the bush
				glm::vec3 grow = glm::normalize(dir + glm::vec3(0.0f, 0.4f, 0.0f) + randomDirection() * 0.3f);
				glm::vec3 right = glm::normalize(glm::cross(grow, randomDirection()));
				float size = random(0.32f, 0.42f) * (lod ? 1.45f : 1.0f);
				float ao = glm::mix(0.4f, 1.0f, c.y / 0.85f);
				cluster(c, right * size, grow * size, dir, LeavesBroadleaf, 0.4f + 0.4f * r, ao);
			}
		}
	};
}

void Trees::buildMeshes(std::vector<TreeVertex>& vertices, std::vector<uint32_t>& indices) {
	for(uint32_t species = 0; species < SPECIES; species++) {
		for(uint32_t variant = 0; variant < VARIANTS; variant++) {
			for(uint32_t lod = 0; lod < 2; lod++) {
				// same seed for both LODs so they're the same tree
				// the third variant of each species is a different tree: pines among the spruces, birches among the
				// broadleaves, an autumn coloured bush
				MeshBuilder builder{ vertices, indices, std::mt19937(1234 + species * 97 + variant * 13) };
				uint32_t first = static_cast<uint32_t>(indices.size());
				bool other = variant == VARIANTS - 1;
				if(species == 0) {
					builder.tint = other ? 2.0f : 1.0f;
					if(other) builder.pine(lod == 1);
					else builder.conifer(lod == 1);
				} else if(species == 1) {
					builder.tint = other ? 5.0f : 3.0f;
					builder.broadleaf(lod == 1, other);
				} else {
					builder.tint = other ? 4.0f : 6.0f;
					builder.leafTile = other ? 1 : 0;
					builder.bush(lod == 1);
				}
				m_Meshes[species * VARIANTS + variant][lod] = { first, static_cast<uint32_t>(indices.size()) - first };
			}
		}
	}
}

uint32_t Trees::binOffset(uint32_t bin) {
	if(bin < 9) return bin * CAPACITY_LOD0;
	if(bin < 18) return 9 * CAPACITY_LOD0 + (bin - 9) * CAPACITY_LOD1;
	if(bin < 27) return 9 * (CAPACITY_LOD0 + CAPACITY_LOD1) + (bin - 18) * CAPACITY_SHADOW;
	return 9 * (CAPACITY_LOD0 + CAPACITY_LOD1 + CAPACITY_SHADOW); // bin 27: the impostor pool, shared by all meshes
}

// ------------------------------------------------------------------ vulkan

namespace {
	VkDescriptorSetLayout storageLayout(VkDevice device, std::initializer_list<VkDescriptorType> types, VkShaderStageFlags stages) {
		DescriptorLayoutBuilder builder;
		uint32_t binding = 0;
		for(VkDescriptorType type : types) builder.add_binding(binding++, type);
		return builder.build(device, stages);
	}

	void writeSet(VkDevice device, VkDescriptorSet set, uint32_t binding, VkDescriptorType type, const VkDescriptorImageInfo* image, const VkDescriptorBufferInfo* buffer, uint32_t count = 1) {
		VkWriteDescriptorSet write{ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
		write.dstSet = set;
		write.dstBinding = binding;
		write.descriptorCount = count;
		write.descriptorType = type;
		write.pImageInfo = image;
		write.pBufferInfo = buffer;
		vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
	}

	VkPipelineLayout pipelineLayout(VkDevice device, std::initializer_list<VkDescriptorSetLayout> sets, VkShaderStageFlags pushStages) {
		std::vector<VkDescriptorSetLayout> layouts(sets);
		VkPushConstantRange range{ pushStages, 0, sizeof(FoliagePushConstants) };
		VkPipelineLayoutCreateInfo layoutCI = vkinit::pipeline_layout_create_info();
		layoutCI.setLayoutCount = static_cast<uint32_t>(layouts.size());
		layoutCI.pSetLayouts = layouts.data();
		layoutCI.pushConstantRangeCount = 1;
		layoutCI.pPushConstantRanges = &range;
		VkPipelineLayout layout;
		VK_CHECK(vkCreatePipelineLayout(device, &layoutCI, nullptr, &layout));
		return layout;
	}

	AllocatedImage createAtlas(Application* engine, VkFormat format, VkImageUsageFlags usage, uint32_t width, uint32_t height, VkImageAspectFlags aspect) {
		AllocatedImage image;
		image.imageFormat = format;
		image.imageExtent = { width, height, 1 };
		VkImageCreateInfo imageCI = vkinit::image_create_info(format, usage, image.imageExtent);
		VmaAllocationCreateInfo allocCI = {};
		allocCI.usage = VMA_MEMORY_USAGE_GPU_ONLY;
		allocCI.requiredFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
		VK_CHECK(vmaCreateImage(engine->getAllocator(), &imageCI, &allocCI, &image.image, &image.allocation, nullptr));
		VkImageViewCreateInfo viewCI = vkinit::imageview_create_info(format, image.image, aspect);
		VK_CHECK(vkCreateImageView(engine->m_Device, &viewCI, nullptr, &image.imageView));
		return image;
	}
}

void Trees::init(Application* engine, const AllocatedImage& heightmap, const AllocatedImage& pathMap, VkSampler heightmapSampler,
				 const AllocatedImage* textures, VkSampler textureSampler,
				 VkDescriptorSetLayout atmosphereLayout, VkDescriptorSetLayout shadowLayout, VkFormat shadowFormat, VkBuffer interactionBuffer) {
	m_Device = engine->m_Device;
	m_Allocator = engine->getAllocator();

	// meshes
	std::vector<TreeVertex> vertices;
	std::vector<uint32_t> indices;
	buildMeshes(vertices, indices);

	size_t vertexBytes = vertices.size() * sizeof(TreeVertex), indexBytes = indices.size() * sizeof(uint32_t);
	m_VertexBuffer = engine->createBuffer(vertexBytes, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	m_IndexBuffer = engine->createBuffer(indexBytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	AllocatedBuffer staging = engine->createBuffer(vertexBytes + indexBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VMA_MEMORY_USAGE_CPU_ONLY);
	memcpy(staging.info.pMappedData, vertices.data(), vertexBytes);
	memcpy(static_cast<uint8_t*>(staging.info.pMappedData) + vertexBytes, indices.data(), indexBytes);
	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		VkBufferCopy vertexCopy{ 0, 0, vertexBytes };
		VkBufferCopy indexCopy{ vertexBytes, 0, indexBytes };
		vkCmdCopyBuffer(cmd, staging.buffer, m_VertexBuffer.buffer, 1, &vertexCopy);
		vkCmdCopyBuffer(cmd, staging.buffer, m_IndexBuffer.buffer, 1, &indexCopy);
	});
	engine->destroyBuffer(staging);
	fmt::print("trees: {} vertices, {} triangles\n", vertices.size(), indices.size() / 3);

	// buffers: placed trees (persistent), their count, the per frame bins + their draw commands
	VkBufferUsageFlags storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	m_Placed = engine->createBuffer(MAX_PLACED * sizeof(glm::vec4) * 2, storage, VMA_MEMORY_USAGE_GPU_ONLY);
	m_PlacedCount = engine->createBuffer(sizeof(uint32_t), storage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	m_OcclusionBuffer = engine->createBuffer(sizeof(OcclusionData), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	m_Instances = engine->createBuffer((MAX_INSTANCES + 1) * sizeof(glm::vec4) * 2, storage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY); // + the impostor bake
	m_Commands = engine->createBuffer(BINS * sizeof(VkDrawIndexedIndirectCommand), storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	m_ImpostorCommands = engine->createBuffer(sizeof(VkDrawIndirectCommand), storage | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);

	m_CommandTemplate.resize(BINS);
	for(uint32_t bin = 0; bin < BINS; bin++) {
		uint32_t mesh = bin % MESHES;
		uint32_t lod = bin < 9 ? 0 : 1; // shadow casters use LOD1
		m_CommandTemplate[bin] = { m_Meshes[mesh][lod].indexCount, 0, m_Meshes[mesh][lod].firstIndex, 0, binOffset(bin) };
	}
	m_ImpostorTemplate = { { 6, 0, 0, binOffset(27) } }; // one draw, every mesh's impostors

	// descriptor sets
	const VkDescriptorType SAMPLER = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, SSBO = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	m_PlaceLayout = storageLayout(m_Device, { SAMPLER, SSBO, SSBO, SAMPLER }, VK_SHADER_STAGE_COMPUTE_BIT);
	m_ComputeLayout = storageLayout(m_Device, { SSBO, SSBO, SSBO, SSBO, SSBO, SAMPLER, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER }, VK_SHADER_STAGE_COMPUTE_BIT); // + depth pyramid, occlusion
	DescriptorLayoutBuilder renderBuilder;
	renderBuilder.add_binding(0, SSBO);
	renderBuilder.add_binding(1, SAMPLER, TREE_TEXTURES);
	renderBuilder.add_binding(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER); // the player's trail (interaction.glsl)
	m_RenderLayout = renderBuilder.build(m_Device, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);
	m_ImpostorLayout = storageLayout(m_Device, { SSBO, SAMPLER, SAMPLER }, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT);

	m_PlaceSet = engine->g_DescriptorAllocator.allocate(m_Device, m_PlaceLayout);
	m_ComputeSet = engine->g_DescriptorAllocator.allocate(m_Device, m_ComputeLayout);
	m_RenderSet = engine->g_DescriptorAllocator.allocate(m_Device, m_RenderLayout);
	m_ImpostorSet = engine->g_DescriptorAllocator.allocate(m_Device, m_ImpostorLayout);

	setHeightmap(heightmap, pathMap, heightmapSampler); // place set bindings 0 and 3
	VkDescriptorBufferInfo placedInfo{ m_Placed.buffer, 0, VK_WHOLE_SIZE }, countInfo{ m_PlacedCount.buffer, 0, VK_WHOLE_SIZE };
	VkDescriptorBufferInfo instanceInfo{ m_Instances.buffer, 0, VK_WHOLE_SIZE }, commandInfo{ m_Commands.buffer, 0, VK_WHOLE_SIZE };
	VkDescriptorBufferInfo impostorCommandInfo{ m_ImpostorCommands.buffer, 0, VK_WHOLE_SIZE };
	writeSet(m_Device, m_PlaceSet, 1, SSBO, nullptr, &placedInfo);
	writeSet(m_Device, m_PlaceSet, 2, SSBO, nullptr, &countInfo);
	writeSet(m_Device, m_ComputeSet, 0, SSBO, nullptr, &instanceInfo);
	writeSet(m_Device, m_ComputeSet, 1, SSBO, nullptr, &commandInfo);
	writeSet(m_Device, m_ComputeSet, 2, SSBO, nullptr, &placedInfo);
	writeSet(m_Device, m_ComputeSet, 3, SSBO, nullptr, &countInfo);
	writeSet(m_Device, m_ComputeSet, 4, SSBO, nullptr, &impostorCommandInfo);
	VkDescriptorBufferInfo occlusionInfo{ m_OcclusionBuffer.buffer, 0, VK_WHOLE_SIZE };
	writeSet(m_Device, m_ComputeSet, 6, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &occlusionInfo);
	VkDescriptorImageInfo textureInfos[TREE_TEXTURES];
	for(uint32_t i = 0; i < TREE_TEXTURES; i++) textureInfos[i] = { textureSampler, textures[i].imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	writeSet(m_Device, m_RenderSet, 0, SSBO, nullptr, &instanceInfo);
	writeSet(m_Device, m_RenderSet, 1, SAMPLER, textureInfos, nullptr, TREE_TEXTURES);
	VkDescriptorBufferInfo interactionInfo{ interactionBuffer, 0, VK_WHOLE_SIZE };
	writeSet(m_Device, m_RenderSet, 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &interactionInfo);

	// pipelines
	m_Place = engine->loadComputeShader("src/shaders/bin/trees_place.comp.spv", "tree placement", pipelineLayout(m_Device, { m_PlaceLayout }, VK_SHADER_STAGE_COMPUTE_BIT));
	m_Scatter = engine->loadComputeShader("src/shaders/bin/trees.comp.spv", "tree culling", pipelineLayout(m_Device, { m_ComputeLayout }, VK_SHADER_STAGE_COMPUTE_BIT));
	const VkShaderStageFlags graphicsStages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
	m_RenderPipelineLayout = pipelineLayout(m_Device, { m_RenderLayout, atmosphereLayout, shadowLayout }, graphicsStages);
	m_ShadowPipelineLayout = pipelineLayout(m_Device, { m_RenderLayout }, graphicsStages);
	m_ImpostorPipelineLayout = pipelineLayout(m_Device, { m_ImpostorLayout, atmosphereLayout, shadowLayout }, graphicsStages);

	VkVertexInputBindingDescription binding{ 0, sizeof(TreeVertex), VK_VERTEX_INPUT_RATE_VERTEX };
	std::vector<VkVertexInputAttributeDescription> attributes = {
		{ 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TreeVertex, positionWind) },
		{ 1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TreeVertex, normalMaterial) },
		{ 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(TreeVertex, uvAO) },
	};

	CompiledShader vertexShader = engine->loadShader("src/shaders/bin/trees.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
	CompiledShader fragmentShader = engine->loadShader("src/shaders/bin/trees.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
	CompiledShader shadowShader = engine->loadShader("src/shaders/bin/trees_shadow.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
	CompiledShader prepassShader = engine->loadShader("src/shaders/bin/trees_prepass.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
	CompiledShader bakeShader = engine->loadShader("src/shaders/bin/trees_bake.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);
	CompiledShader impostorVertex = engine->loadShader("src/shaders/bin/impostor.vert.spv", VK_SHADER_STAGE_VERTEX_BIT);
	CompiledShader impostorFragment = engine->loadShader("src/shaders/bin/impostor.frag.spv", VK_SHADER_STAGE_FRAGMENT_BIT);

	auto meshPipeline = [&](VkPipelineLayout layout, VkShaderModule fragment) {
		PipelineBuilder builder;
		builder._pipelineLayout = layout;
		builder.set_shaders(vertexShader.shader, fragment);
		builder.set_vertex_input({ binding }, attributes);
		builder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
		builder.set_polygon_mode(VK_POLYGON_MODE_FILL);
		builder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE); // leaf cards are two sided
		builder.set_multisampling_none();
		builder.disable_blending();
		return builder;
	};

	// depth prepass (alpha test only, no colour), then the shading only where its depth matches exactly
	PipelineBuilder prepassBuilder = meshPipeline(m_ShadowPipelineLayout, prepassShader.shader);
	prepassBuilder.enable_depthtest(true, VK_COMPARE_OP_GREATER); // reversed z
	setGBufferTargets(prepassBuilder, false);
	prepassBuilder._colorBlendAttachment.colorWriteMask = 0;
	prepassBuilder.set_depth_format(engine->m_DepthBuffer.imageFormat);
	m_PrepassPipeline = prepassBuilder.build_pipeline(m_Device);

	PipelineBuilder renderBuilderPipeline = meshPipeline(m_RenderPipelineLayout, fragmentShader.shader);
	renderBuilderPipeline.enable_depthtest(false, VK_COMPARE_OP_EQUAL); // after the prepass
	setGBufferTargets(renderBuilderPipeline, true); // deferred, lit in lighting.comp
	renderBuilderPipeline.set_depth_format(engine->m_DepthBuffer.imageFormat);
	m_RenderPipeline = renderBuilderPipeline.build_pipeline(m_Device);

	PipelineBuilder shadowBuilder = meshPipeline(m_ShadowPipelineLayout, shadowShader.shader);
	shadowBuilder._rasterizer.depthBiasEnable = VK_TRUE;
	shadowBuilder._rasterizer.depthBiasConstantFactor = 2.0f;
	shadowBuilder._rasterizer.depthBiasSlopeFactor = 2.5f;
	shadowBuilder.enable_depthtest(true, VK_COMPARE_OP_LESS); // shadow maps are regular z
	shadowBuilder.set_depth_format(shadowFormat);
	m_ShadowPipeline = shadowBuilder.build_pipeline(m_Device);

	PipelineBuilder impostorBuilder;
	impostorBuilder._pipelineLayout = m_ImpostorPipelineLayout;
	impostorBuilder.set_shaders(impostorVertex.shader, impostorFragment.shader);
	impostorBuilder.set_input_topology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
	impostorBuilder.set_polygon_mode(VK_POLYGON_MODE_FILL);
	impostorBuilder.set_cull_mode(VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE);
	impostorBuilder.set_multisampling_none();
	impostorBuilder.disable_blending();
	impostorBuilder.enable_depthtest(true, VK_COMPARE_OP_GREATER);
	setGBufferTargets(impostorBuilder, true);
	impostorBuilder.set_depth_format(engine->m_DepthBuffer.imageFormat);
	m_ImpostorPipeline = impostorBuilder.build_pipeline(m_Device);

	bakeImpostors(engine, meshPipeline, bakeShader.shader);

	VkDescriptorImageInfo albedoInfo{ textureSampler, m_ImpostorAlbedo.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkDescriptorImageInfo normalInfo{ textureSampler, m_ImpostorNormal.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	writeSet(m_Device, m_ImpostorSet, 0, SSBO, nullptr, &instanceInfo);
	writeSet(m_Device, m_ImpostorSet, 1, SAMPLER, &albedoInfo, nullptr);
	writeSet(m_Device, m_ImpostorSet, 2, SAMPLER, &normalInfo, nullptr);

	for(CompiledShader* shader : { &vertexShader, &fragmentShader, &shadowShader, &bakeShader, &impostorVertex, &impostorFragment })
		vkDestroyShaderModule(m_Device, shader->shader, nullptr);
}

void Trees::setHeightmap(const AllocatedImage& heightmap, const AllocatedImage& pathMap, VkSampler heightmapSampler) {
	VkDescriptorImageInfo heightmapInfo{ heightmapSampler, heightmap.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkDescriptorImageInfo pathMapInfo{ heightmapSampler, pathMap.imageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	writeSet(m_Device, m_PlaceSet, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &heightmapInfo, nullptr);
	writeSet(m_Device, m_PlaceSet, 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &pathMapInfo, nullptr);
}

// renders every mesh from 3 elevations (0, 35 and 70 degrees) into an atlas: one column per mesh, one row per
// elevation. colour + coverage and normals (in the bake view's space) + AO
void Trees::bakeImpostors(Application* engine, const std::function<PipelineBuilder(VkPipelineLayout, VkShaderModule)>& meshPipeline, VkShaderModule bakeShader) {
	const uint32_t cell = IMPOSTOR_CELL, width = cell * MESHES, height = cell * IMPOSTOR_ELEVATIONS;
	VkExtent2D size = { width, height };
	m_ImpostorAlbedo = createAtlas(engine, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, width, height, VK_IMAGE_ASPECT_COLOR_BIT);
	m_ImpostorNormal = createAtlas(engine, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, width, height, VK_IMAGE_ASPECT_COLOR_BIT);
	AllocatedImage depth = createAtlas(engine, VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, width, height, VK_IMAGE_ASPECT_DEPTH_BIT);

	PipelineBuilder builder = meshPipeline(m_ShadowPipelineLayout, bakeShader); // set 0 only, like the shadows
	VkFormat formats[2] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM };
	builder._renderInfo.colorAttachmentCount = 2;
	builder._renderInfo.pColorAttachmentFormats = formats;
	builder.enable_depthtest(true, VK_COMPARE_OP_LESS);
	builder.set_depth_format(VK_FORMAT_D32_SFLOAT);
	// two attachments, so two blend states (the builder only has one)
	VkPipelineColorBlendAttachmentState blends[2] = { builder._colorBlendAttachment, builder._colorBlendAttachment };
	VkPipeline bakePipeline;
	{
		// build_pipeline uses one blend attachment, so build this one by hand from the builder's state
		VkPipelineViewportStateCreateInfo viewportState{ .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
		viewportState.viewportCount = 1;
		viewportState.scissorCount = 1;
		VkPipelineColorBlendStateCreateInfo colorBlending{ .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
		colorBlending.attachmentCount = 2;
		colorBlending.pAttachments = blends;
		VkDynamicState dynamics[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
		VkPipelineDynamicStateCreateInfo dynamicInfo{ .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
		dynamicInfo.dynamicStateCount = 2;
		dynamicInfo.pDynamicStates = dynamics;
		VkGraphicsPipelineCreateInfo pipelineInfo{ .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
		pipelineInfo.pNext = &builder._renderInfo;
		pipelineInfo.stageCount = static_cast<uint32_t>(builder._shaderStages.size());
		pipelineInfo.pStages = builder._shaderStages.data();
		builder._vertexInputInfo.vertexBindingDescriptionCount = static_cast<uint32_t>(builder._vertexBindings.size());
		builder._vertexInputInfo.pVertexBindingDescriptions = builder._vertexBindings.data();
		builder._vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(builder._vertexAttributes.size());
		builder._vertexInputInfo.pVertexAttributeDescriptions = builder._vertexAttributes.data();
		pipelineInfo.pVertexInputState = &builder._vertexInputInfo;
		pipelineInfo.pInputAssemblyState = &builder._inputAssembly;
		pipelineInfo.pViewportState = &viewportState;
		pipelineInfo.pRasterizationState = &builder._rasterizer;
		pipelineInfo.pMultisampleState = &builder._multisampling;
		pipelineInfo.pColorBlendState = &colorBlending;
		pipelineInfo.pDepthStencilState = &builder._depthStencil;
		pipelineInfo.pDynamicState = &dynamicInfo;
		pipelineInfo.layout = m_ShadowPipelineLayout;
		VK_CHECK(vkCreateGraphicsPipelines(m_Device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &bakePipeline));
	}

	engine->immediateSubmit([&](VkCommandBuffer cmd) {
		// the tree to bake: at the origin, height 1, no rotation (the slot after the bins)
		glm::vec4 instance[2] = { glm::vec4(0.0f, 0.0f, 0.0f, 1.0f), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f) };
		vkCmdUpdateBuffer(cmd, m_Instances.buffer, MAX_INSTANCES * sizeof(glm::vec4) * 2, sizeof(instance), instance);
		VkMemoryBarrier2 barrier{ .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
		barrier.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
		barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		barrier.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
		barrier.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
		VkDependencyInfo dependency{ .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
		dependency.memoryBarrierCount = 1;
		dependency.pMemoryBarriers = &barrier;
		vkCmdPipelineBarrier2(cmd, &dependency);

		vkutil::transition_image(cmd, m_ImpostorAlbedo.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		vkutil::transition_image(cmd, m_ImpostorNormal.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
		vkutil::transition_image(cmd, depth.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);

		VkClearValue clear{}; // transparent black, coverage 0
		VkClearValue depthClear{}; depthClear.depthStencil.depth = 1.0f;
		VkRenderingAttachmentInfo colorAttachments[2] = {
			vkinit::attachment_info(m_ImpostorAlbedo.imageView, &clear, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
			vkinit::attachment_info(m_ImpostorNormal.imageView, &clear, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL),
		};
		VkRenderingAttachmentInfo depthAttachment = vkinit::attachment_info(depth.imageView, &depthClear, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
		VkRenderingInfo renderInfo = vkinit::rendering_info(size, colorAttachments, &depthAttachment);
		renderInfo.colorAttachmentCount = 2;
		vkCmdBeginRendering(cmd, &renderInfo);

		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bakePipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ShadowPipelineLayout, 0, 1, &m_RenderSet, 0, nullptr);
		VkDeviceSize offset = 0;
		vkCmdBindVertexBuffers(cmd, 0, 1, &m_VertexBuffer.buffer, &offset);
		vkCmdBindIndexBuffer(cmd, m_IndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

		// orthographic views of the unit tree (height 0..1) around its centre, from the side up to 70 degrees
		glm::vec3 center(0.0f, 0.5f, 0.0f);
		for(uint32_t row = 0; row < IMPOSTOR_ELEVATIONS; row++) {
			float elevation = glm::radians(IMPOSTOR_ELEVATION_STEP * row);
			glm::vec3 toViewer(0.0f, std::sin(elevation), std::cos(elevation));
			FoliagePushConstants pc{};
			pc.viewproj = glm::ortho(-0.6f, 0.6f, -0.6f, 0.6f, 0.0f, 6.0f) * glm::lookAt(center + toViewer * 3.0f, center, glm::vec3(0.0f, 1.0f, 0.0f));
			pc.viewproj = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, -1.0f, 1.0f)) * pc.viewproj; // atlas rows go down, tree tops at the top
			pc.campos = center + toViewer * 1000.0f;
			pc.sundir = toViewer; // trees_bake.frag stores normals relative to this view
			vkCmdPushConstants(cmd, m_ShadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FoliagePushConstants), &pc);

			for(uint32_t mesh = 0; mesh < MESHES; mesh++) {
				VkViewport viewport = { static_cast<float>(mesh * cell), static_cast<float>(row * cell), static_cast<float>(cell), static_cast<float>(cell), 0.0f, 1.0f };
				VkRect2D scissor = { { static_cast<int32_t>(mesh * cell), static_cast<int32_t>(row * cell) }, { cell, cell } };
				vkCmdSetViewport(cmd, 0, 1, &viewport);
				vkCmdSetScissor(cmd, 0, 1, &scissor);
				vkCmdDrawIndexed(cmd, m_Meshes[mesh][0].indexCount, 1, m_Meshes[mesh][0].firstIndex, 0, MAX_INSTANCES);
			}
		}
		vkCmdEndRendering(cmd);

		vkutil::transition_image(cmd, m_ImpostorAlbedo.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		vkutil::transition_image(cmd, m_ImpostorNormal.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
	});

	vkDestroyPipeline(m_Device, bakePipeline, nullptr);
	vkDestroyImageView(m_Device, depth.imageView, nullptr);
	vmaDestroyImage(m_Allocator, depth.image, depth.allocation);
}

void Trees::destroy() {
	vkDestroyPipeline(m_Device, m_RenderPipeline, nullptr);
	vkDestroyPipeline(m_Device, m_ShadowPipeline, nullptr);
	vkDestroyPipeline(m_Device, m_PrepassPipeline, nullptr);
	vkDestroyPipeline(m_Device, m_ImpostorPipeline, nullptr);
	for(VkPipelineLayout layout : { m_RenderPipelineLayout, m_ShadowPipelineLayout, m_ImpostorPipelineLayout, m_Scatter.layout, m_Place.layout })
		vkDestroyPipelineLayout(m_Device, layout, nullptr);
	for(VkDescriptorSetLayout layout : { m_PlaceLayout, m_ComputeLayout, m_RenderLayout, m_ImpostorLayout })
		vkDestroyDescriptorSetLayout(m_Device, layout, nullptr);
	for(AllocatedBuffer* buffer : { &m_VertexBuffer, &m_IndexBuffer, &m_Instances, &m_Commands, &m_ImpostorCommands, &m_Placed, &m_PlacedCount, &m_OcclusionBuffer })
		vmaDestroyBuffer(m_Allocator, buffer->buffer, buffer->allocation);
	for(AllocatedImage* image : { &m_ImpostorAlbedo, &m_ImpostorNormal }) {
		vkDestroyImageView(m_Device, image->imageView, nullptr);
		vmaDestroyImage(m_Allocator, image->image, image->allocation);
	}
}

static void treeBarrier(VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
	VkMemoryBarrier2 barrier{ .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
	barrier.srcStageMask = srcStage;
	barrier.srcAccessMask = srcAccess;
	barrier.dstStageMask = dstStage;
	barrier.dstAccessMask = dstAccess;
	VkDependencyInfo dependency{ .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	dependency.memoryBarrierCount = 1;
	dependency.pMemoryBarriers = &barrier;
	vkCmdPipelineBarrier2(cmd, &dependency);
}

void Trees::setOcclusionSource(VkImageView pyramid, VkSampler sampler) {
	VkDescriptorImageInfo info{ sampler, pyramid, VK_IMAGE_LAYOUT_GENERAL };
	writeSet(m_Device, m_ComputeSet, 5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &info, nullptr);
}

void Trees::update(VkCommandBuffer cmd, const FoliagePushConstants& pc) {
	m_PushConstants = pc;
	m_PushConstants.extra = m_Density;
	if(!m_Enabled) return;

	const VkPipelineStageFlags2 drawStages = VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
	const VkAccessFlags2 drawReads = VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
	const VkAccessFlags2 computeAccess = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

	// last frame may still be drawing from these
	treeBarrier(cmd, drawStages | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, drawReads | VK_ACCESS_2_SHADER_STORAGE_READ_BIT,
				VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT | computeAccess);

	// re-place once the camera moved far enough (placement covers a much bigger area than what's needed)
	glm::vec2 camWorld = glm::vec2(pc.campos.x, pc.campos.z) + pc.offsetWorld;
	bool replace = m_ForcePlace || glm::distance(camWorld, m_LastPlaceCenter) > PLACE_STEP || m_Density != m_LastDensity;
	if(replace) {
		m_ForcePlace = false;
		m_LastPlaceCenter = camWorld;
		m_LastDensity = m_Density;
		uint32_t zero = 0;
		vkCmdUpdateBuffer(cmd, m_PlacedCount.buffer, 0, sizeof(zero), &zero);
	}

	vkCmdUpdateBuffer(cmd, m_Commands.buffer, 0, m_CommandTemplate.size() * sizeof(VkDrawIndexedIndirectCommand), m_CommandTemplate.data());
	vkCmdUpdateBuffer(cmd, m_ImpostorCommands.buffer, 0, m_ImpostorTemplate.size() * sizeof(VkDrawIndirectCommand), m_ImpostorTemplate.data());
	vkCmdUpdateBuffer(cmd, m_OcclusionBuffer.buffer, 0, sizeof(OcclusionData), &m_Occlusion);
treeBarrier(cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | drawStages, computeAccess | drawReads);

	if(replace) {
		vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Place.pipeline);
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Place.layout, 0, 1, &m_PlaceSet, 0, nullptr);
		vkCmdPushConstants(cmd, m_Place.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FoliagePushConstants), &m_PushConstants);
		vkCmdDispatch(cmd, (PLACE_GRID_SIDE * PLACE_GRID_SIDE + 63) / 64, 1, 1);
		treeBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
	}

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Scatter.pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_Scatter.layout, 0, 1, &m_ComputeSet, 0, nullptr);
	vkCmdPushConstants(cmd, m_Scatter.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(FoliagePushConstants), &m_PushConstants);
	vkCmdDispatch(cmd, (MAX_PLACED + 63) / 64, 1, 1); // threads past the placed count exit straight away

	treeBarrier(cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, drawStages, drawReads);
}

void Trees::draw(VkCommandBuffer cmd, VkDescriptorSet atmosphereSet, VkDescriptorSet shadowSet, const std::function<void()>& split) {
	if(!m_Enabled) {
		if(split) { split(); split(); } // the timers still need their boundaries
		return;
	}
	VkDeviceSize vertexOffset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &m_VertexBuffer.buffer, &vertexOffset);
	vkCmdBindIndexBuffer(cmd, m_IndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);

	// depth prepass: leaf cards overlap a lot, this way each pixel gets shaded once
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_PrepassPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ShadowPipelineLayout, 0, 1, &m_RenderSet, 0, nullptr);
	vkCmdPushConstants(cmd, m_ShadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FoliagePushConstants), &m_PushConstants);
	for(uint32_t bin = 0; bin < 18; bin++)
		vkCmdDrawIndexedIndirect(cmd, m_Commands.buffer, bin * sizeof(VkDrawIndexedIndirectCommand), 1, sizeof(VkDrawIndexedIndirectCommand));
	if(split) split();

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_RenderPipeline);
	VkDescriptorSet sets[] = { m_RenderSet, atmosphereSet, shadowSet };
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_RenderPipelineLayout, 0, 3, sets, 0, nullptr);
	vkCmdPushConstants(cmd, m_RenderPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FoliagePushConstants), &m_PushConstants);
	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &m_VertexBuffer.buffer, &offset);
	vkCmdBindIndexBuffer(cmd, m_IndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
	for(uint32_t bin = 0; bin < 18; bin++)
		vkCmdDrawIndexedIndirect(cmd, m_Commands.buffer, bin * sizeof(VkDrawIndexedIndirectCommand), 1, sizeof(VkDrawIndexedIndirectCommand));
	if(split) split();

	// far trees as impostors
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ImpostorPipeline);
	VkDescriptorSet impostorSets[] = { m_ImpostorSet, atmosphereSet, shadowSet };
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ImpostorPipelineLayout, 0, 3, impostorSets, 0, nullptr);
	vkCmdPushConstants(cmd, m_ImpostorPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FoliagePushConstants), &m_PushConstants);
	vkCmdDrawIndirect(cmd, m_ImpostorCommands.buffer, 0, 1, sizeof(VkDrawIndirectCommand));
}

void Trees::drawShadow(VkCommandBuffer cmd, const glm::mat4& lightViewProj) {
	if(!m_Enabled) return;
	FoliagePushConstants pc = m_PushConstants;
	pc.viewproj = lightViewProj;
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ShadowPipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_ShadowPipelineLayout, 0, 1, &m_RenderSet, 0, nullptr);
	vkCmdPushConstants(cmd, m_ShadowPipelineLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(FoliagePushConstants), &pc);
	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &m_VertexBuffer.buffer, &offset);
	vkCmdBindIndexBuffer(cmd, m_IndexBuffer.buffer, 0, VK_INDEX_TYPE_UINT32);
	for(uint32_t bin = 18; bin < BINS; bin++)
		vkCmdDrawIndexedIndirect(cmd, m_Commands.buffer, bin * sizeof(VkDrawIndexedIndirectCommand), 1, sizeof(VkDrawIndexedIndirectCommand));
}

void Trees::drawImGui() {
	ImGui::Checkbox("Trees", &m_Enabled);
	ImGui::SliderFloat("Tree density", &m_Density, 0.0f, 3.0f);
}
