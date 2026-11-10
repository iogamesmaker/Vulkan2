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

namespace {
	// proprotation.glsl's, exactly: the two tilt angles packed in one number, then yaw * tilt (about z) * roll (about x)
	glm::vec2 unpackTilt(float packed) {
		float qx = std::floor(packed / 2048.0f);
		return glm::vec2(qx, packed - qx * 2048.0f) / 500.0f - 2.0f;
	}
	glm::mat3 propRotation(float yaw, float packedTilt) {
		glm::vec2 a = unpackTilt(packedTilt);
		float ct = std::cos(a.x), st = std::sin(a.x), cr = std::cos(a.y), sr = std::sin(a.y), cy = std::cos(yaw), sy = std::sin(yaw);
		glm::mat3 yawM(cy, 0.0f, -sy, 0.0f, 1.0f, 0.0f, sy, 0.0f, cy);
		glm::mat3 tiltM(ct, st, 0.0f, -st, ct, 0.0f, 0.0f, 0.0f, 1.0f);
		glm::mat3 rollM(1.0f, 0.0f, 0.0f, 0.0f, cr, sr, 0.0f, -sr, cr);
		return yawM * tiltM * rollM;
	}
	// the baked top surface at a point of the mesh's footprint (mesh space), < 0 off it
	float shapeTop(const PropShape& s, float x, float z) {
		glm::vec2 p = (glm::vec2(x, z) - s.lo) / glm::max(s.hi - s.lo, glm::vec2(1e-6f)) * float(SHAPE_GRID);
		if(p.x < 0.0f || p.y < 0.0f || p.x >= SHAPE_GRID || p.y >= SHAPE_GRID) return -1.0f;
		return s.top[int(p.y) * SHAPE_GRID + int(p.x)];
	}
}

// a rock / log / stump's surface over a world xz (its baked shape, turned and scaled like props.comp draws it). the
// mesh space point under xz depends on the height when it's tilted: found from the base height, then once more
// from the surface found
float Collision::propTop(int p, glm::vec2 xz) const {
	glm::vec4 a = m_Data.props[p * 2], b = m_Data.props[p * 2 + 1];
	if(b.w < 0.5f) return -1e9f;
	int mesh = int(b.z + 0.5f);
	if(mesh < 0 || mesh >= int(m_Shapes.size()) || !m_Shapes[mesh].valid) return -1e9f;
	const PropShape& shape = m_Shapes[mesh];
	glm::vec3 base(a);
	float scale = std::max(b.x, 1e-4f);
	glm::mat3 rotation = propRotation(a.w, b.y), inverse = glm::transpose(rotation);
	float y = base.y, top = -1e9f;
	for(int iteration = 0; iteration < 2; iteration++) {
		glm::vec3 local = inverse * (glm::vec3(xz.x, y, xz.y) - base) / scale;
		float h = shapeTop(shape, local.x, local.z);
		if(h < 0.0f) return -1e9f;
		top = y = (base + rotation * glm::vec3(local.x, h, local.z) * scale).y;
	}
	return top;
}

float Collision::resolve(glm::vec2& xz, float feet, float radius, float stepHeight) const {
	const float footRadius = radius * 0.3f; // what you stand on: the middle of your feet, not their whole disk
	const float level = feet + stepHeight;	// anything above this is a wall
	// the body's footprint: its middle and two rings, what of it is inside a prop above the step level is blocked
	glm::vec2 ring[16];
	for(int i = 0; i < 8; i++) {
		float angle = float(i) * 0.785398f;
		ring[i] = glm::vec2(std::cos(angle), std::sin(angle)) * radius;
		ring[i + 8] = ring[i] * 0.55f;
	}

	for(int iteration = 0; iteration < 3; iteration++) {
		for(const glm::vec4& tree : m_Data.trees) {
			if(tree.w < 0.5f) continue;
			glm::vec2 away = xz - glm::vec2(tree.x, tree.y);
			float minDistance = tree.z + radius, d = glm::length(away);
			if(d < minDistance) xz += (d > 1e-5f ? away / d : glm::vec2(1.0f, 0.0f)) * (minDistance - d);
		}
		for(int p = 0; p < PROBE_PROPS; p++) {
			glm::vec4 a = m_Data.props[p * 2], b = m_Data.props[p * 2 + 1];
			if(b.w < 0.5f) continue;
			glm::vec2 centre(a.x, a.z);
			if(glm::length(xz - centre) > b.x * 0.75f + radius) continue; // (the meshes reach ~0.55, logs 0.5 from their middle)
			auto blockedAt = [&](glm::vec2 at) {
				glm::vec2 direction(0.0f);
				bool blocked = false;
				if(propTop(p, at) > level) { blocked = true; direction += at - centre; }
				for(const glm::vec2& r : ring)
					if(propTop(p, at + r) > level) { blocked = true; direction -= r; }
				return std::pair<bool, glm::vec2>(blocked, direction);
			};
			auto [blocked, direction] = blockedAt(xz);
			if(!blocked) continue;
			// out the way the free side is, in short steps until nothing of the footprint is inside any more
			float len = glm::length(direction);
			glm::vec2 out = len > 1e-5f ? direction / len : glm::normalize(xz - centre + glm::vec2(1e-4f, 0.0f));
			for(int step = 1; step <= 16; step++) {
				glm::vec2 candidate = xz + out * (radius * 0.15f * float(step));
				if(!blockedAt(candidate).first) { xz = candidate; break; }
			}
		}
	}

	// what's under your feet: the terrain, or the top of a rock / log / stump you can step onto
	float ground = groundAt(xz);
	glm::vec2 feetPoints[5] = { xz, xz + glm::vec2(footRadius, 0.0f), xz - glm::vec2(footRadius, 0.0f), xz + glm::vec2(0.0f, footRadius), xz - glm::vec2(0.0f, footRadius) };
	for(int p = 0; p < PROBE_PROPS; p++) {
		if(m_Data.props[p * 2 + 1].w < 0.5f) continue;
		for(const glm::vec2& f : feetPoints) {
			float top = propTop(p, f);
			if(top <= level + 1e-4f) ground = glm::max(ground, top);
		}
	}
	return ground;
}

void Collision::dumpNearestProp(glm::vec2 from) const {
	int best = -1;
	float bestDistance = 1e9f;
	for(int p = 0; p < PROBE_PROPS; p++) {
		if(m_Data.props[p * 2 + 1].w < 0.5f) continue;
		float d = glm::length(glm::vec2(m_Data.props[p * 2].x, m_Data.props[p * 2].z) - from);
		if(d < bestDistance) { bestDistance = d; best = p; }
	}
	if(best < 0) { fmt::print("colldump: no props\n"); return; }
	glm::vec4 a = m_Data.props[best * 2], b = m_Data.props[best * 2 + 1];
	fmt::print("colldump: mesh {} scale {:.3f} at {:.1f} {:.1f} base y {:.3f}:", int(b.z + 0.5f), b.x, a.x, a.z, a.y);
	for(int i = -10; i <= 10; i++) {
		glm::vec2 at = glm::vec2(a.x, a.z) + glm::vec2(float(i) * b.x * 0.07f, 0.0f);
		float top = propTop(best, at);
		fmt::print(" {:.3f}", top > -1e8f ? top - a.y : -1.0f);
	}
	fmt::print("\n");
}
