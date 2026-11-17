#ifndef SNOWTRAIL_HPP
#define SNOWTRAIL_HPP

#include <vk_types.h>
#include <vector>

class Application;

// footprints in the deep snow (snow.glsl): every stride walked on the ground leaves a print, alternating feet, and
// the legs plough a shallow furrow between them. they're pressed into a world anchored texture around the player
// (SIZE texels of TEXEL_METRES, toroidal: a world texel always sits at its coordinates modulo SIZE) by
// snowstamp.comp, and stay until the window around the player moves on past them. the terrain mesh (terrain.tese)
// reads it wherever the snow is deep, nothing is pressed where there's no snow to press
class SnowTrail {
public:
	static constexpr int SIZE = 2048;					// texels per side (~82m)
	static constexpr double TEXEL_METRES = 0.04;
	static constexpr int SNAP = 256;					// the window re-centres in steps of this many texels

	// writes its info + texture into the terrain set (bindings 9, 10)
	void init(Application* engine, VkDescriptorSet terrainSet);
	void destroy();
	// feet in continuous world units (the camera's space), on = touching the ground and walking
	void update(glm::dvec3 feetWorld, bool on, double metresPerUnit);
	// the stamps since the last frame, the window's strips, the info terrain.tese / terrain.frag read. outside of any
	// rendering, before the terrain draws (its shadows included). cameraWorld: as terrain.tese's campos + the offset
	void record(VkCommandBuffer cmd, glm::dvec2 cameraWorld, double metresPerUnit, bool enabled);

	VkImageView view() const { return m_View; }

private:
	struct Stamp { glm::dvec2 a, b; double halfWidth, halfLength, pressed; }; // metres / world positions in texels
	struct StampPushConstants {
		glm::ivec2 boxMin, boxSize;
		glm::vec2 a, b;
		float halfWidth, halfLength, pressed;
		int mode;
	};
	struct Info { glm::vec4 camera, window; }; // snow.glsl's SnowInfo

	void dispatch(VkCommandBuffer cmd, const StampPushConstants& pc);
	static int wrap(long long t) { return static_cast<int>(((t % SIZE) + SIZE) % SIZE); }

	Application* m_Engine = nullptr;
	VkDevice m_Device = VK_NULL_HANDLE;
	AllocatedImage m_Image{};
	VkImageView m_View = VK_NULL_HANDLE;
	VkSampler m_Sampler = VK_NULL_HANDLE;
	AllocatedBuffer m_InfoBuffer{};
	VkDescriptorSetLayout m_StampLayout = VK_NULL_HANDLE;
	VkDescriptorSet m_StampSet = VK_NULL_HANDLE;
	ComputeEffect m_Effect{};

	std::vector<Stamp> m_Pending; // pressed since the last record, in world texels
	long long m_CentreX = 0, m_CentreY = 0; // the window's centre, world texels
	bool m_HaveCentre = false;
	glm::dvec2 m_LastFeet{ 0.0 };
	bool m_HaveLast = false;
	double m_Walked = 0.0; // metres since the last print
	bool m_LeftFoot = false;
};

#endif
