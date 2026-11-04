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
	bool valid() const { return m_Valid; }

	// the terrain under a world xz point
	float groundAt(glm::vec2 xz) const;
	// pushes xz out of trunks and the parts of rocks / logs you can't step onto, returns the ground under you
	float resolve(glm::vec2& xz, float feet, float radius, float stepHeight) const;

private:
	float sample(const float* grid, glm::vec2 xz) const;
	ProbeData m_Data{};
	bool m_Valid = false;
};
