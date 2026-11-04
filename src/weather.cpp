#include "weather.hpp"
#include <imgui.h>
#include <algorithm>
#include <cmath>

namespace {
// the biome lines of biometools.glsl's groundCover
bool cold(glm::vec3 c) { return c.x < 0.36f; }
bool desert(glm::vec3 c) { return c.x > 0.6f && c.y < 0.45f; }
float approach(float value, float target, float dt, float seconds) { return value + (target - value) * std::min(dt / seconds, 1.0f); }
}

const char* Weather::name(WeatherType type) {
	switch(type) {
	case WeatherType::Clear: return "Clear";
	case WeatherType::Cloudy: return "Cloudy";
	case WeatherType::Overcast: return "Overcast";
	case WeatherType::Rain: return "Rain";
	case WeatherType::Thunderstorm: return "Thunderstorm";
	case WeatherType::Snow: return "Snow";
	case WeatherType::Sandstorm: return "Sandstorm";
	default: return "?";
	}
}

const char* Weather::climateName(glm::vec3 c) const {
	if(c.x < 0.22f) return "frozen (tundra)";
	if(cold(c)) return "cold (taiga)";
	if(desert(c)) return "hot and dry (desert)";
	if(c.x > 0.6f) return "warm (tropics)";
	return "temperate";
}

// rain where it's too cold falls as snow, sand only blows where it's a desert, nothing falls from a desert sky
bool Weather::fits(WeatherType type, glm::vec3 c) const {
	switch(type) {
	case WeatherType::Rain:
	case WeatherType::Thunderstorm: return !cold(c) && !desert(c);
	case WeatherType::Snow: return cold(c);
	case WeatherType::Sandstorm: return desert(c);
	default: return true;
	}
}

// what comes next: a weighted pick of what fits the climate. wetter places rain more, deserts are mostly clear
WeatherType Weather::pick(glm::vec3 c) {
	float weights[static_cast<int>(WeatherType::Count)] = {};
	auto set = [&](WeatherType t, float w) { weights[static_cast<int>(t)] = w; };
	if(desert(c)) {
		set(WeatherType::Clear, 5.0f); set(WeatherType::Cloudy, 1.5f); set(WeatherType::Sandstorm, 2.5f);
	} else if(cold(c)) {
		set(WeatherType::Clear, 2.0f); set(WeatherType::Cloudy, 2.0f); set(WeatherType::Overcast, 1.5f); set(WeatherType::Snow, 3.0f);
	} else {
		float wet = glm::clamp(c.y, 0.0f, 1.0f);
		set(WeatherType::Clear, 3.5f - wet * 1.5f); set(WeatherType::Cloudy, 3.0f); set(WeatherType::Overcast, 1.5f);
		set(WeatherType::Rain, 1.0f + wet * 2.0f); set(WeatherType::Thunderstorm, 0.4f + wet * (c.x > 0.55f ? 1.5f : 0.6f));
	}
	weights[static_cast<int>(m_Type)] *= 0.3f; // usually something else
	float total = 0.0f;
	for(float w : weights) total += w;
	float r = std::uniform_real_distribution<float>(0.0f, total)(m_Random);
	for(int i = 0; i < static_cast<int>(WeatherType::Count); i++) {
		r -= weights[i];
		if(r <= 0.0f && weights[i] > 0.0f) return static_cast<WeatherType>(i);
	}
	return WeatherType::Clear;
}

// what each weather settles at (at intensity 1). the haze is kept moderate: moderate rain or snow still leaves a few
// km of visibility, only the sandstorm really closes in
WeatherState Weather::target(WeatherType type) const {
	WeatherState s;
	switch(type) {
	case WeatherType::Clear:		s.coverage = 0.25f; s.density = 0.8f; s.wind = 0.2f; break;
	case WeatherType::Cloudy:		s.coverage = 0.55f; s.density = 1.1f; s.overcast = 0.15f; s.wind = 0.35f; break;
	case WeatherType::Overcast:		s.coverage = 0.88f; s.density = 1.6f; s.overcast = 0.7f; s.wind = 0.45f; s.haze = 0.2f; break;
	case WeatherType::Rain:			s.coverage = 0.93f; s.density = 2.0f; s.overcast = 0.85f; s.rain = 0.7f; s.wind = 0.55f; s.haze = 0.8f; break;
	case WeatherType::Thunderstorm: s.coverage = 1.0f; s.density = 2.8f; s.overcast = 1.0f; s.rain = 1.0f; s.wind = 0.95f; s.haze = 1.0f; break;
	case WeatherType::Snow:			s.coverage = 0.92f; s.density = 1.7f; s.overcast = 0.8f; s.snow = 0.8f; s.wind = 0.35f; s.haze = 1.05f; break;
	case WeatherType::Sandstorm:	s.coverage = 0.35f; s.density = 0.9f; s.overcast = 0.45f; s.sand = 1.0f; s.wind = 1.0f; s.haze = 3.5f; break;
	default: break;
	}
	s.rain *= m_Intensity; s.snow *= m_Intensity; s.sand *= m_Intensity; s.haze *= m_Intensity;
	s.wind = std::min(s.wind * m_WindScale, 1.0f);
	return s;
}

void Weather::update(float dt, glm::vec3 climate) {
	dt = std::min(dt, 0.25f); // a hitch (loading) shouldn't jump the weather
	m_Climate = climate;
	if(m_Auto) {
		m_Timer -= dt;
		// travelled somewhere it can't happen (snow in the desert): change now, it blends over
		if(m_Timer <= 0.0f || !fits(m_Type, climate)) {
			m_Type = pick(climate);
			float minutes = std::uniform_real_distribution<float>(0.6f, 1.4f)(m_Random) * m_Minutes;
			m_Timer = minutes * 60.0f;
		}
	}

	// everything eases towards the weather's values: the sky over ~40s, what falls a little faster
	WeatherState goal = target(m_Type);
	WeatherState& s = m_State;
	s.coverage = approach(s.coverage, goal.coverage, dt, 40.0f);
	s.density = approach(s.density, goal.density, dt, 40.0f);
	s.overcast = approach(s.overcast, goal.overcast, dt, 35.0f);
	s.wind = approach(s.wind, goal.wind, dt, 25.0f);
	s.haze = approach(s.haze, goal.haze, dt, 30.0f);
	// it starts raining once the clouds are mostly in
	float ready = glm::smoothstep(0.6f, 0.9f, s.coverage);
	// what falls follows the climate you're in (automatic only, picked by hand it's what you asked for): walked or
	// teleported out of the snow into the desert, it stops within a few seconds instead of the usual ~20
	bool snowFits = !m_Auto || fits(WeatherType::Snow, climate);
	bool rainFits = !m_Auto || fits(WeatherType::Rain, climate);
	bool sandFits = !m_Auto || fits(WeatherType::Sandstorm, climate);
	s.rain = approach(s.rain, rainFits ? goal.rain * (goal.rain > 0.0f ? ready : 1.0f) : 0.0f, dt, rainFits ? 15.0f : 2.5f);
	s.snow = approach(s.snow, snowFits ? goal.snow * (goal.snow > 0.0f ? ready : 1.0f) : 0.0f, dt, snowFits ? 20.0f : 2.5f);
	s.sand = approach(s.sand, sandFits ? goal.sand : 0.0f, dt, sandFits ? 20.0f : 2.5f);

	// the ground: soaked within a minute of heavy rain, dries in a few minutes (slower under an overcast sky)
	if(s.rain > 0.05f) s.wetness = std::min(s.wetness + s.rain * dt / 45.0f, 1.0f);
	else s.wetness = std::max(s.wetness - dt / (150.0f * (1.0f + s.overcast * 2.0f)), 0.0f);
	// snow: settles over a minute or two, melts over several (fast where it's warm, the rain washes it away)
	if(s.snow > 0.05f) s.snowCover = std::min(s.snowCover + s.snow * dt / 70.0f, 1.0f);
	else s.snowCover = std::max(s.snowCover - dt * ((desert(climate) ? 1.0f / 5.0f : climate.x > 0.4f ? 1.0f / 60.0f : 1.0f / 400.0f) + s.rain / 40.0f), 0.0f);
	// (the lighting only lets it settle where the ground's own climate is cold enough, lighting.comp)

	// lightning: strikes every few seconds in a thunderstorm, each a few quick flickers over ~half a second
	m_StrikeAge += dt;
	if(m_Type == WeatherType::Thunderstorm && s.rain > 0.5f) {
		m_StrikeIn -= dt;
		if(m_StrikeIn <= 0.0f) {
			m_StrikeAge = 0.0f;
			m_StrikeSeed = std::uniform_real_distribution<float>(0.0f, 100.0f)(m_Random);
			m_StrikeIn = std::uniform_real_distribution<float>(3.0f, 14.0f)(m_Random) / std::max(m_Intensity, 0.3f);
		}
	}
	s.flash = 0.0f;
	if(m_StrikeAge < 0.7f) {
		// three pulses, the first the brightest, each decaying fast
		for(int i = 0; i < 3; i++) {
			float start = i * (0.08f + 0.05f * std::fmod(m_StrikeSeed * (i + 1), 1.0f));
			float t = m_StrikeAge - start;
			if(t >= 0.0f) s.flash += std::exp(-t * 18.0f) * (i == 0 ? 1.0f : 0.55f);
		}
		s.flash *= 0.6f + 0.4f * std::fmod(m_StrikeSeed, 1.0f);
	}
}

void Weather::drawImGui() {
	ImGui::SeparatorText("Weather");
	ImGui::Text("Now: %s   (%s here)", name(m_Type), climateName(m_Climate));
	ImGui::Checkbox("Automatic", &m_Auto);
	ImGui::SetItemTooltip("Picks the weather by itself from where you are: rain and storms in temperate and tropical land, snow where it's cold, sandstorms in deserts");
	int current = static_cast<int>(m_Type);
	const char* names[] = { "Clear", "Cloudy", "Overcast", "Rain", "Thunderstorm", "Snow", "Sandstorm" };
	// what's set by hand applies right away, no minute of blending: the sky, what falls, the wind, the ground too
	if(ImGui::Combo("Weather", &current, names, IM_ARRAYSIZE(names))) {
		m_Auto = false; // picked by hand: it stays
		forceType(static_cast<WeatherType>(current));
	}
	bool changed = ImGui::SliderFloat("Intensity", &m_Intensity, 0.2f, 2.0f, "%.1fx");
	changed |= ImGui::SliderFloat("Wind", &m_WindScale, 0.0f, 2.0f, "%.1fx");
	if(changed) {
		float wetness = m_State.wetness, snowCover = m_State.snowCover;
		m_State = target(m_Type);
		m_State.wetness = wetness;
		m_State.snowCover = snowCover;
	}
	ImGui::BeginDisabled(!m_Auto);
	ImGui::SliderFloat("Changes every", &m_Minutes, 0.5f, 20.0f, "~%.1f min", ImGuiSliderFlags_Logarithmic);
	ImGui::SetItemTooltip("The weather cycle: every few minutes (random around this) the next weather is picked from what fits the climate here, and everything blends over to it in about a minute");
	if(m_Auto) { ImGui::SameLine(); ImGui::TextDisabled("next in %.0fs", std::max(m_Timer, 0.0f)); }
	if(ImGui::Button("Next weather")) m_Timer = 0.0f;
	ImGui::EndDisabled();
	if(ImGui::Button("Dry the ground")) { m_State.wetness = 0.0f; m_State.snowCover = 0.0f; }
	ImGui::SameLine();
	ImGui::TextDisabled("wet %.0f%%  snow %.0f%%", m_State.wetness * 100.0f, m_State.snowCover * 100.0f);
}
