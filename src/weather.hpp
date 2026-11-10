#pragma once
// the weather: a slow state machine over the climate under the camera (worldgen.hpp) that picks what fits where you
// are (rain, showers and thunderstorms in the temperate and tropical lands, snow in the cold ones, sandstorms in the
// deserts, fog in the damp valleys, clear and cloudy everywhere), plus a weather FIELD over the land: a slow noise in
// the clouds' wind carried space (clouds.glsl weatherCells, mirrored here) whose cells become the showers and storm
// cells. the weather sets how unstable the air is (how strongly the cells show: towers, anvils, showers, lightning)
// and whether it rains everywhere under a deck or only under the cells. so the weather is local: it rains where the
// cloud above you rains, a shower drifts over and past, a front rolls in from upwind, lightning comes out of the
// storm cells. what you stand under drives what falls (precipitation.cpp), the sky's greyness, the haze (volumetric
// fog), the flash; the ground keeps a memory of it (wet after the rain, snow melting slowly)
#include <glm/glm.hpp>
#include <random>

enum class WeatherType : int { Clear, Cloudy, Overcast, Rain, Showers, Thunderstorm, Snow, Sandstorm, Fog, Count };

struct WeatherState {
	float coverage = 0.4f;	// the cloud layer (clouds.glsl): how much of the sky, how thick
	float density = 1.0f;
	float overcast = 0.0f;	// how grey and dim the sky's light is where you are (atmosphere.glsl's overcastSky)
	float rain = 0.0f;		// what's falling where you are, 0..1
	float snow = 0.0f;
	float sand = 0.0f;
	float wind = 0.3f;		// 0 calm .. 1 storm: cloud drift, slanted rain, sand, the trees and grass
	float haze = 0.0f;		// extra volumetric fog the precipitation brings
	float wetness = 0.0f;	// on the ground: builds up in the rain, dries over minutes
	float snowCover = 0.0f; // builds up while it snows, melts slowly (faster where it's warm)
	float flash = 0.0f;		// lightning: what reaches you, this frame (multiples of the sun's illuminance)
	// the weather field (ubo.weather3)
	float instability = 0.0f;	 // how strongly the field's cells show: towers, showers, storms, 0..1
	float rainDeck = 0.0f;		 // rain (snow) everywhere under a thick deck
	float rainCells = 0.0f;		 // rain (snow) under the cells
	float cloudBase = 2.2f;		 // km: low and grey when it's damp, high in fair weather
	float mist = 1.0f;			 // the valley mist, times the setting (fog)
	// lightning (ubo.weather4)
	glm::vec2 strikeKm{ 0.0f };	 // continuous world km (no wind)
	float strikeFlash = 0.0f;	 // at the bolt itself
	float strikeSeed = 0.0f;
	float cellsHere = 0.0f;		 // the field's core where you are (for the UI)
};

// the field (clouds.glsl's weatherCells / weatherCellCore, the same float math)
float weatherCellsAt(glm::vec2 worldKm);
float weatherCellCore(float cells);

class Weather {
public:
	// climate = worldgen::climateAt under the camera: r temperature, g moisture, b ruggedness. cameraKm = the camera,
	// continuous world km; windKm = how far the clouds have drifted (the field moves with them)
	void update(float dt, glm::vec3 climate, glm::vec2 cameraKm, glm::vec2 windKm);
	void drawImGui(); // the atmosphere window's weather controls
	const WeatherState& state() const { return m_State; }
	WeatherType type() const { return m_Type; }
	static const char* name(WeatherType type);
	static bool fromName(const char* name, WeatherType& type); // case insensitive ("thunderstorm")
	// straight to a weather, fully set in (the captures): no minute of blending in
	void forceType(WeatherType type);

	bool m_Auto = true;			 // picks the weather by itself, from the climate
	float m_Intensity = 1.0f;	 // how hard it rains / snows / blows
	float m_Minutes = 4.0f;		 // auto: how long a weather lasts on average (real minutes)
	float m_WindScale = 1.0f;
	// automatic: every new weather rolls its own intensity and wind (times the sliders above), so no two rains are alike
	float m_RolledIntensity = 1.0f, m_RolledWind = 1.0f;
	// the captures: a strike held at its brightest, optionally at a fixed offset from the camera (km)
	bool m_HoldStrike = false, m_PlaceStrike = false;
	glm::vec2 m_PlacedStrike{ 0.0f };

private:
	WeatherType pick(glm::vec3 climate);
	bool fits(WeatherType type, glm::vec3 climate) const;
	WeatherState target(WeatherType type) const;
	const char* climateName(glm::vec3 climate) const;
	void localWeather(float dt, glm::vec3 climate, glm::vec2 cameraKm, glm::vec2 windKm, bool snap);
	void lightning(float dt, glm::vec2 cameraKm, glm::vec2 windKm);

	WeatherType m_Type = WeatherType::Clear;
	WeatherState m_State;
	WeatherState m_Regional;	 // the weather's own values, eased (m_State = them + what's overhead)
	glm::vec3 m_Climate{ 0.5f };
	bool m_Snap = false;		 // forceType: no easing on the next update
	float m_Timer = 0.0f;		 // auto: seconds until the next change
	float m_StrikeIn = 3.0f;	 // lightning: seconds until the next strike
	float m_StrikeAge = 1e9f;	 // seconds since the last one
	float m_StrikeDistance = 10.0f; // km from the camera
	std::mt19937 m_Random{ 1337u };
};
