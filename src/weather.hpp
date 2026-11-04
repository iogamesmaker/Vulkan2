#pragma once
// the weather: a slow state machine over the climate under the camera (worldgen.hpp). it picks what fits where you
// are (rain and thunderstorms in the temperate and tropical lands, snow in the cold ones, sandstorms in the deserts,
// clear and cloudy everywhere) and blends everything it drives over a minute or so: the cloud layer, the sky's
// greyness, the wind, what falls (precipitation.cpp), the haze it brings (volumetric fog), lightning. the ground
// keeps a memory of it: it stays wet a while after the rain, snow settles and melts away slowly
#include <glm/glm.hpp>
#include <random>

enum class WeatherType : int { Clear, Cloudy, Overcast, Rain, Thunderstorm, Snow, Sandstorm, Count };

struct WeatherState {
	float coverage = 0.4f;	// the cloud layer (clouds.glsl): how much of the sky, how thick
	float density = 1.0f;
	float overcast = 0.0f;	// how grey and dim the sky's light is (atmosphere.glsl's overcastSky)
	float rain = 0.0f;		// what's falling, 0..1
	float snow = 0.0f;
	float sand = 0.0f;
	float wind = 0.3f;		// 0 calm .. 1 storm: cloud drift, slanted rain, sand
	float haze = 0.0f;		// extra volumetric fog the precipitation brings
	float wetness = 0.0f;	// on the ground: builds up in the rain, dries over minutes
	float snowCover = 0.0f; // builds up while it snows, melts slowly (faster where it's warm)
	float flash = 0.0f;		// lightning, this frame (multiples of the sun's illuminance)
};

class Weather {
public:
	// climate = worldgen::climateAt under the camera: r temperature, g moisture, b ruggedness
	void update(float dt, glm::vec3 climate);
	void drawImGui(); // the atmosphere window's weather controls
	const WeatherState& state() const { return m_State; }
	WeatherType type() const { return m_Type; }
	static const char* name(WeatherType type);
	// straight to a weather, fully set in (the captures): no minute of blending in
	void forceType(WeatherType type) { m_Type = type; float w = m_State.wetness, s = m_State.snowCover; m_State = target(type); m_State.wetness = (m_State.rain > 0.0f) ? 1.0f : w; m_State.snowCover = (m_State.snow > 0.0f) ? 1.0f : s; }

	bool m_Auto = true;			 // picks the weather by itself, from the climate
	float m_Intensity = 1.0f;	 // how hard it rains / snows / blows
	float m_Minutes = 4.0f;		 // auto: how long a weather lasts on average (real minutes)
	float m_WindScale = 1.0f;

private:
	WeatherType pick(glm::vec3 climate);
	bool fits(WeatherType type, glm::vec3 climate) const;
	WeatherState target(WeatherType type) const;
	const char* climateName(glm::vec3 climate) const;

	WeatherType m_Type = WeatherType::Clear;
	WeatherState m_State;
	glm::vec3 m_Climate{ 0.5f };
	float m_Timer = 0.0f;		// auto: seconds until the next change
	float m_StrikeIn = 3.0f;	// lightning: seconds until the next strike
	float m_StrikeAge = 1e9f;	// seconds since the last one
	float m_StrikeSeed = 0.0f;
	std::mt19937 m_Random{ 1337u };
};
