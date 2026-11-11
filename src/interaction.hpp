#ifndef INTERACTION_HPP
#define INTERACTION_HPP

#include <vk_types.h>
#include <deque>

class Application;

// where the player's feet were over the last few seconds (interaction.glsl): the grass and the low leaves of
// trees bend away from it and spring back after. a small uniform buffer, written with vkCmdUpdateBuffer at the start
// of every frame so the frames in flight never see it change under them
class PlayerTrail {
public:
	static constexpr int POINTS = 16; // INTERACTION_POINTS
	struct Data {
		glm::vec4 points[POINTS]; // xyz feet, terrain space, w = age (s)
		glm::vec4 info;			  // x = count, y = body height, z = the weather's wind (0 calm .. 1 storm)
	};

	void init(Application* engine);
	void destroy();
	// feet / offset in continuous world units (the camera's space), active = touching the ground
	void update(glm::vec3 feetWorld, bool active, float bodyHeight, float dt);
	// records the upload (+ barriers), outside of any rendering, before the foliage / trees draw
	void record(VkCommandBuffer cmd, glm::vec2 offsetWorld);
	VkBuffer buffer() const { return m_Buffer.buffer; }

	// how fast you're moving through the vegetation (world units / s), the audio's rustle
	float speed() const { return m_Speed; }
	float m_Wind = 0.3f; // the weather's wind (weather.hpp), how hard the grass, bushes and trees sway
	float m_WindTravel = 0.0f; // how far the gusts have blown, world units (windfield.glsl)

private:
	Application* m_Engine = nullptr;
	AllocatedBuffer m_Buffer{};
	struct Point { glm::vec3 world; float age; };
	std::deque<Point> m_Points; // newest first
	glm::vec3 m_Current{ 0.0f };
	bool m_Active = false;
	float m_BodyHeight = 0.28f;
	float m_Speed = 0.0f;
	glm::vec3 m_LastFeet{ 0.0f };
	bool m_HaveLast = false;
};

#endif
