#ifndef ATMOSPHERE_HPP
#define ATMOSPHERE_HPP

#include <vk_types.h>
#include "weather.hpp"

class Application;

// must match the UBO in src/shaders/atmosphere.glsl (std140)
struct AtmosphereUBO {
	alignas(16)glm::vec3 rayleigh;
	float rayleighscale;
	alignas(16)glm::vec3 mie;
	float miescale;
	alignas(16)glm::vec3 absorbtion;
	float mieg;
	alignas(16)glm::vec3 groundAlbedo;
	float radiustop;

	float radiusbottom;
	float sunIlluminance;
	float moonIlluminance;
	float worldScale;	  // km per world unit

	float aerialDistance; // km covered by the aerial perspective LUT
	float baseAltitude;	  // km above the planet surface at world y = 0
	float sunAngularRadius;
	float moonAngularRadius;

	float fogStart;		  // world units
	float fogEnd;		  // world units, fully opaque from here on
	float fogDensity;
	float starBrightness;

	float terrainSize;	  // world units covered by the heightmap
	float textureRepeatNear; // world units per detail texture repeat up close
	float textureRepeatFar;	// and further away, has to divide 3 (the map offset step) too
	float terrainCache = 1.0f; // 1 = terrain.frag reads the far field cache (terraincache.comp), 0 = shades it all

	float pomDistance = 8.0f;	 // world units (~50m), parallax occlusion mapping fades out over the last 40%, 0 = off
	float pomDepth = 0.045f;	 // world units (~25cm) between the lowest and highest point of a height map
	float pomSteps = 16.0f;		 // most steps down the view ray (looking along the surface)
	float pomShadow = 1.0f;		 // strength of the parallax self shadowing, 0 = off

	float cloudCoverage = 0.45f; // 0 clear .. 1 overcast (clouds.glsl)
	float cloudAltitude = 2.2f;	 // km above the planet surface
	float cloudDensity = 1.0f;	 // how opaque the clouds are
	float fogDistance = 400.0f;	 // world units the volumetric fog's froxel grid reaches (volfog.glsl)

	// graphics settings (settings.hpp), 1 = on
	float terrainTriplanar = 1.0f; // cliffs textured from the side
	float terrainBumps = 1.0f;	   // procedural detail bumps in the normal up close
	float terrainMacro = 1.0f;	   // the far anti-tiling texture layer
	float cloudShadows = 1.0f;
	float clouds = 1.0f;			// the cloud layer at all (and its shadows)
	float cloudQuality = 1.0f;		// 0 = fewer noise octaves + light samples
	float aerialPerspective = 1.0f; // the haze over distance (aerial perspective LUT)
	float volumetricFog = 0.0f;		// the froxel fog (volfog_*.comp), composited by lighting.comp

	// the weather (weather.cpp), every frame
	glm::vec4 weather{ 0.0f };	// x = overcast, y = wetness, z = lightning flash, w = snow cover
	glm::vec4 weather2{ 0.0f }; // x = rain, y = snowfall, z = sand, w = wind (0 calm .. 1 storm)
};
static_assert(sizeof(AtmosphereUBO) == 224, "AtmosphereUBO doesn't match the std140 layout");

// per frame data for the sky view / aerial LUTs and the sky shader
struct AtmospherePushConstants {
	glm::mat4 invViewProj; // rotation only
	glm::vec3 camPos;	   // km, planet centred
	float starRotation;	   // hour angle
	glm::vec3 sunDir;
	float latitude;
	glm::vec2 cameraWorldKm; // the camera's continuous world position, for the clouds (sky.frag)
	glm::vec2 cloudWind;	 // km the clouds have drifted
	float frame = 0.0f;		 // counts up, the volumetric clouds' per frame jitter (TAA averages it)
	glm::vec3 pad{ 0.0f };
};
static_assert(sizeof(AtmospherePushConstants) <= 128, "push constants are only guaranteed to have 128 bytes");

// auto exposure (exposure.comp) + bloom + tonemapping (post.comp), they share it
struct PostPushConstants {
	float exposure;		 // compensation (2^EV) on top of the automatic exposure
	float deltaTime;	 // seconds since the last frame (the exposure adapts over time)
	float bloomStrength; // the mix of the bloom chain
	float pad;
};
static_assert(sizeof(PostPushConstants) <= 128, "push constants are only guaranteed to have 128 bytes");

// two shadow light directions along the sun's path with the current one in between, shadows crossfade
// between them so they move smoothly instead of re-rasterising (shimmering) every frame
struct ShadowLightKeys {
	long long keyA, keyB; // keyframe ids, B is the next one along the sun's path
	glm::vec3 dirA, dirB; // sun or moon, whichever is up at that keyframe
	float t;			  // 0 = all A, 1 = all B
	float strength;		  // fades shadows out near the horizon, hides the sun -> moon switch
};

class Atmosphere {
public:
	void setBloom(float strength) { m_BloomStrength = strength; }
	bool m_AerialPerspective = true; // graphics settings: off skips the per frame aerial LUT
	void init(VkDevice device, VmaAllocator allocator, Application* engine);
	void initSkyPipeline(Application* engine);
	void destroy();

	// advances the day/night cycle
	void tick(float deltatime);
	void setTimeOfDay(float hours, bool animate) { m_TimeOfDay = hours; m_AnimateTime = animate; computeSunDir(); }
	float getTimeOfDay() const { return m_TimeOfDay; }
	glm::vec3 getSunDir() const { return m_SunDir; }
	glm::vec3 sunDirAt(float timeOfDay) const;
	ShadowLightKeys getShadowLight(float stepDegrees) const;

	// the post pass reads the depth buffer and writes the draw image, call again after they get recreated
	void setRenderTargets(const AllocatedImage& drawImage);
	void setMoonTexture(VkImageView view, VkSampler sampler); // equirectangular colour map, owned by the application
	void setCirrusNoise(VkImageView view, VkSampler sampler); // the cirrus' noise lattice (linear, repeating), owned by the application
	void setVolumetricFog(VkImageView view, VkSampler sampler); // the integrated froxel fog (GENERAL), owned by the application
	void setWeather(const WeatherState& weather); // every frame (weather.hpp)
	// tells the fog where the terrain ends
	void setTerrainSize(float worldSize, float renderDistance, float textureRepeatNear, float textureRepeatFar);

	// records the per frame LUTs (sky view + aerial perspective), call outside of rendering
	// cameraWorld: the camera's continuous world position (terrain space + map offset), for the clouds
	void update(VkCommandBuffer cmd, const glm::mat4& view, const glm::mat4& projection, glm::vec3 campos, glm::vec2 cameraWorld);
	glm::vec2 getCloudWind() const { return m_CloudWind; }
	void setBloom(VkImageView view, VkSampler sampler); // the bloom chain post.comp adds (Bloom), after every resize
	void draw(VkCommandBuffer cmd);
	// draw image in GENERAL, depth buffer in SHADER_READ_ONLY_OPTIMAL
	void postProcess(VkCommandBuffer cmd, VkExtent2D extent);
	// metered luminance / current exposure (auto exposure, read back for the capture mode's printout)
	glm::vec2 getExposureState() const;

	// imgui controls, rebuilds the static LUTs when the physical parameters change
	void drawImGui();

	void updateUBO(const AtmosphereUBO& newUBO);
	void buildStaticLUTs(); // transmittance + multiscattering

	VkDescriptorSetLayout getRenderDescriptorLayout() const { return m_SkyDescriptorLayout; }
	VkDescriptorSet getRenderDescriptor() const { return m_SkyDescriptor; }

	AtmosphereUBO m_AtmosphereData;
	AllocatedImage transmittance;
	AllocatedImage multiscattering;
	AllocatedImage skyview;
	AllocatedImage aerial;
	AllocatedImage aerialSun; // the part of aerial's light that's the sun / moon's single scattering: lighting.comp shadows it
private:
	void setDefaults();
	void computeSunDir();

	VkDevice m_Device;
	VmaAllocator m_Allocator;
	Application* m_Engine;

	AtmospherePushConstants m_PushConstants{};
	PostPushConstants m_PostPushConstants{};

	// day/night cycle
	float m_TimeOfDay = 8.0f;		 // hours
	float m_DayLength = 600.0f;		 // real seconds per in-game day
	bool m_AnimateTime = true;
	float m_Latitude = 40.0f;		 // degrees
	float m_Declination = 10.0f;	 // degrees, the season (-23.44 winter .. 23.44 summer)
	float m_SunAzimuthOffset = 0.0f; // degrees, rotates the sun path around the world
	glm::vec3 m_SunDir{0.0f, 1.0f, 0.0f};
	float m_HourAngle = 0.0f;

	// exposure follows the sun so nights aren't pitch black
	float m_ExposureEV = 0.0f; // compensation on top of the automatic exposure (exposure.comp)
	float m_Deltatime = 0.016f; // for the exposure adaptation


	float m_CloudWindSpeed = 0.012f; // km per second
	float m_BloomStrength = 0.04f;	 // how much of the bloom chain gets mixed in (post.comp)
	glm::vec2 m_CloudWind{0.0f};	 // accumulated drift (clouds.glsl)
	

	AllocatedBuffer m_AtmosphereUBO; // stores VMA data + VkBuffer

	VkDescriptorSetLayout m_UBODescriptorLayout;
	VkDescriptorSet m_UBODescriptor;

	VkDescriptorSetLayout m_SkyviewDescriptorLayout;
	VkDescriptorSet m_SkyviewDescriptor;

	VkDescriptorSetLayout m_TransmittanceDescriptorLayout;
	VkDescriptorSet m_TransmittanceDescriptor;

	VkDescriptorSetLayout m_MultiscatteringDescriptorLayout;
	VkDescriptorSet m_MultiscatteringDescriptor;

	VkDescriptorSetLayout m_AerialDescriptorLayout;
	VkDescriptorSet m_AerialDescriptor;

	VkDescriptorSetLayout m_PostDescriptorLayout;
	VkDescriptorSet m_PostDescriptor;

	VkPipeline m_SkyPipeline;
	VkPipelineLayout m_SkyLayout;

	// all LUTs + UBO for graphics shaders, used by the sky (set 0) and the terrain (set 1)
	VkDescriptorSetLayout m_SkyDescriptorLayout;
	VkDescriptorSet m_SkyDescriptor;

	// pipelines are destroyed by the application's deletion queue, layouts by destroy()
	ComputeEffect m_TransmittanceEffect;
	ComputeEffect m_SkyviewEffect;
	ComputeEffect m_MultiscatteringEffect;
	ComputeEffect m_AerialEffect;
	ComputeEffect m_PostEffect;
	ComputeEffect m_ExposureEffect; // same layout as the post pass
	AllocatedBuffer m_ExposureBuffer; // host visible so the capture mode can print it

	/*
	struct ComputeEffect {
		std::string name;

		VkPipeline pipeline;
		VkPipelineLayout layout;
	};
	*/

};

#endif
