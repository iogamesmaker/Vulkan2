#include "collision.hpp"

// bilinear in the patch (clamped to its edge if you somehow got outside it)
float Collision::sample(const float* grid, glm::vec2 xz) const {
	glm::vec2 p = glm::clamp((xz - glm::vec2(m_Data.header)) / m_Data.header.z, glm::vec2(0.0f), glm::vec2(PROBE_GRID - 1.001f));
	glm::ivec2 i = glm::ivec2(p);
	glm::vec2 f = p - glm::vec2(i);
	auto at = [&](int x, int y) {
		int index = (i.y + y) * PROBE_GRID + i.x + x;
		return grid[index];
	};
	return glm::mix(glm::mix(at(0, 0), at(1, 0), f.x), glm::mix(at(0, 1), at(1, 1), f.x), f.y);
}

float Collision::groundAt(glm::vec2 xz) const {
	return sample(m_Data.terrain, xz);
}

float Collision::resolve(glm::vec2& xz, float feet, float radius, float stepHeight) const {
	const float footRadius = radius * 0.3f; // what you stand on: the middle of your feet, not their whole disk
	const float level = feet + stepHeight;	// anything above this is a wall

	for(int iteration = 0; iteration < 3; iteration++) {
		for(const glm::vec4& tree : m_Data.trees) {
			if(tree.w < 0.5f) continue;
			glm::vec2 away = xz - glm::vec2(tree.x, tree.y);
			float minDistance = tree.z + radius, d = glm::length(away);
			if(d < minDistance) xz += (d > 1e-5f ? away / d : glm::vec2(1.0f, 0.0f)) * (minDistance - d);
		}
		for(int p = 0; p < PROBE_PROPS; p++) {
			glm::vec4 a = m_Data.props[p * 2], b = m_Data.props[p * 2 + 1];
			if(b.w > 0.5f && b.w < 1.5f) {
				// rock: the ellipsoid's horizontal cross section at the step level, grown by your radius
				if(level >= a.y + b.y) continue;
				float s = level <= a.y ? 1.0f : std::sqrt(1.0f - glm::pow((level - a.y) / b.y, 2.0f));
				glm::vec2 d = xz - glm::vec2(a.x, a.z);
				float cy = std::cos(a.w), sy = std::sin(a.w);
				glm::vec2 local(cy * d.x - sy * d.y, sy * d.x + cy * d.y); // into the rock's frame
				glm::vec2 grown = glm::vec2(b.x, b.z) * s + radius;
				float q = glm::dot(local / grown, local / grown);
				if(q >= 1.0f) continue;
				glm::vec2 move = local / glm::max(std::sqrt(q), 1e-3f) - local;
				xz += glm::vec2(cy * move.x + sy * move.y, -sy * move.x + cy * move.y); // back to world
			} else if(b.w > 1.5f) {
				// log: a capsule, its width at the step level along the closest point of its axis
				glm::vec2 ab = glm::vec2(b.x, b.z) - glm::vec2(a.x, a.z);
				float t = glm::clamp(glm::dot(xz - glm::vec2(a.x, a.z), ab) / glm::max(glm::dot(ab, ab), 1e-8f), 0.0f, 1.0f);
				glm::vec3 axis = glm::mix(glm::vec3(a), glm::vec3(b), t);
				float r = a.w;
				if(level >= axis.y + r) continue;
				float halfWidth = level <= axis.y ? r : std::sqrt(r * r - (level - axis.y) * (level - axis.y));
				glm::vec2 away = xz - glm::vec2(axis.x, axis.z);
				float d = glm::length(away);
				if(d < halfWidth + radius) xz += (d > 1e-5f ? away / d : glm::vec2(-ab.y, ab.x) / glm::max(glm::length(ab), 1e-5f)) * (halfWidth + radius - d);
			}
		}
	}

	// what's under your feet: the terrain, or the top of a rock or log you can step onto
	float ground = groundAt(xz);
	for(int p = 0; p < PROBE_PROPS; p++) {
		glm::vec4 a = m_Data.props[p * 2], b = m_Data.props[p * 2 + 1];
		float top = -1e9f;
		if(b.w > 0.5f && b.w < 1.5f) {
			glm::vec2 d = xz - glm::vec2(a.x, a.z);
			float cy = std::cos(a.w), sy = std::sin(a.w);
			glm::vec2 local(cy * d.x - sy * d.y, sy * d.x + cy * d.y);
			float l = glm::length(local);
			glm::vec2 foot = l > 1e-6f ? local - local / l * glm::min(l, footRadius) : local; // the foot point nearest the middle
			float q = glm::dot(foot / glm::vec2(b.x, b.z), foot / glm::vec2(b.x, b.z));
			if(q < 1.0f) top = a.y + b.y * std::sqrt(1.0f - q);
		} else if(b.w > 1.5f) {
			glm::vec2 ab = glm::vec2(b.x, b.z) - glm::vec2(a.x, a.z);
			float t = glm::clamp(glm::dot(xz - glm::vec2(a.x, a.z), ab) / glm::max(glm::dot(ab, ab), 1e-8f), 0.0f, 1.0f);
			glm::vec3 axis = glm::mix(glm::vec3(a), glm::vec3(b), t);
			float r = a.w;
			float d = glm::max(glm::length(xz - glm::vec2(axis.x, axis.z)) - footRadius, 0.0f);
			if(d < r) top = axis.y + std::sqrt(r * r - d * d);
		}
		if(top <= level + 1e-4f) ground = glm::max(ground, top);
	}
	return ground;
}
