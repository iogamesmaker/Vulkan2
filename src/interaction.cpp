#include "interaction.hpp"
#include "application.hpp"

void PlayerTrail::init(Application* engine) {
	m_Engine = engine;
	m_Buffer = engine->createBuffer(sizeof(Data), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VMA_MEMORY_USAGE_GPU_ONLY);
	// nothing in it until the first frame's upload
	m_Engine->immediateSubmit([&](VkCommandBuffer cmd) { vkCmdFillBuffer(cmd, m_Buffer.buffer, 0, VK_WHOLE_SIZE, 0); });
}

void PlayerTrail::destroy() {
	if(m_Engine) m_Engine->destroyBuffer(m_Buffer);
}

void PlayerTrail::update(glm::vec3 feetWorld, bool active, float bodyHeight, float dt) {
	m_BodyHeight = bodyHeight;
	for(Point& p : m_Points) p.age += dt;
	while(!m_Points.empty() && m_Points.back().age > 3.5f) m_Points.pop_back(); // settled long ago

	// speed over the ground, smoothed (teleports / the first frame don't count)
	if(m_HaveLast && dt > 0.0f) {
		float moved = glm::length(glm::vec2(feetWorld.x - m_LastFeet.x, feetWorld.z - m_LastFeet.z)) / dt;
		if(moved > 50.0f) moved = 0.0f;
		m_Speed += ((active ? moved : 0.0f) - m_Speed) * std::min(1.0f, dt * 8.0f);
	}
	m_LastFeet = feetWorld;
	m_HaveLast = true;

	m_Active = active;
	m_Current = feetWorld;
	if(!active) return;
	// a new point every ~45cm walked: the trail behind you (the current position is always point 0, age 0)
	const float spacing = bodyHeight * 0.27f;
	if(m_Points.empty() || glm::distance(m_Points.front().world, feetWorld) > spacing) {
		m_Points.push_front({ feetWorld, 0.0f });
		while(m_Points.size() > POINTS - 1) m_Points.pop_back();
	}
}

void PlayerTrail::record(VkCommandBuffer cmd, glm::vec2 offsetWorld) {
	Data data{};
	int count = 0;
	glm::vec3 offset(offsetWorld.x, 0.0f, offsetWorld.y);
	if(m_Active) data.points[count++] = glm::vec4(m_Current - offset, 0.0f);
	for(const Point& p : m_Points) {
		if(count >= POINTS) break;
		// the newest trail point sits where you are anyway while you stand still, skip the duplicate
		if(m_Active && p.age < 0.05f && glm::distance(p.world, m_Current) < 1e-3f) continue;
		data.points[count++] = glm::vec4(p.world - offset, p.age);
	}
	data.info = glm::vec4(static_cast<float>(count), m_BodyHeight, 0.0f, 0.0f);

	// the last frame's vertex shaders are done with it before it changes, this frame's wait for the new data
	VkMemoryBarrier2 before{ .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
	before.srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
	before.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
	before.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	VkDependencyInfo dependency{ .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
	dependency.memoryBarrierCount = 1;
	dependency.pMemoryBarriers = &before;
	vkCmdPipelineBarrier2(cmd, &dependency);

	vkCmdUpdateBuffer(cmd, m_Buffer.buffer, 0, sizeof(Data), &data);

	VkMemoryBarrier2 after{ .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
	after.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
	after.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	after.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
	after.dstAccessMask = VK_ACCESS_2_UNIFORM_READ_BIT;
	dependency.pMemoryBarriers = &after;
	vkCmdPipelineBarrier2(cmd, &dependency);
}
