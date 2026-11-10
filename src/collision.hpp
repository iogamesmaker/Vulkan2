#pragma once
#include <vk_types.h>

// walking collisions, resolved on the CPU every frame against a snapshot of the surroundings that probe.comp
// gathers on the GPU (a terrain patch, tree trunks, rock / log shapes, all in continuous world space = the
// camera's xz). the snapshot is a couple of frames old, but the world doesn't move, so only the patch's extent
// matters (it's a few metres around where you were)
//
// rocks and logs: the part of a shape above your feet + the step height is a wall you get pushed out of, the
// surface under your feet (a small disk) is ground. so low rocks you walk over, big ones you climb wherever the
// slope is gentle enough to step up (step height / (radius - foot radius) ~ 60 degrees), jump onto anything

// a prop mesh's real shape for walking on it: the height of its top surface over its footprint, in the mesh's own
// normalised space (Props::loadMesh), baked when it loads. what's above your step level is a wall
constexpr int SHAPE_GRID = 32;
struct PropShape {
	glm::vec2 lo{ 0.0f }, hi{ 0.0f }; // the footprint, mesh x / z
	float top[SHAPE_GRID * SHAPE_GRID]{}; // mesh y, < 0 where there's nothing
	bool valid = false;
};

// must match probe.comp
constexpr int PROBE_GRID = 32;
constexpr int PROBE_TREES = 25;
constexpr int PROBE_PROPS = 49;
struct ProbeData {
	glm::vec4 header; // xy = world xz of grid sample (0, 0), z = grid spacing, w = 1 once written
	glm::vec4 trees[PROBE_TREES];
	glm::vec4 props[PROBE_PROPS * 2];
	float terrain[PROBE_GRID * PROBE_GRID];
};
static_assert(sizeof(ProbeData) % 16 == 0, "std430 array stride");

class Collision {
public:
	void ingest(const ProbeData& data) { m_Data = data; m_Valid = data.header.w > 0.5f; }
	void setShapes(const std::vector<PropShape>& shapes) { m_Shapes = shapes; }
	// VULKAN2_COLLDUMP: the ground (terrain or prop top) along a line through the nearest prop, printed
	void dumpNearestProp(glm::vec2 from) const;
	bool valid() const { return m_Valid; }

	// the terrain under a world xz point
	float groundAt(glm::vec2 xz) const;
	// pushes xz out of trunks and the parts of rocks / logs you can't step onto, returns the ground under you
	float resolve(glm::vec2& xz, float feet, float radius, float stepHeight) const;

private:
	float sample(const float* grid, glm::vec2 xz) const;
	float propTop(int p, glm::vec2 xz) const; // a prop's surface over a world xz, -1e9 where it has none
	std::vector<PropShape> m_Shapes;
	ProbeData m_Data{};
	bool m_Valid = false;
};
