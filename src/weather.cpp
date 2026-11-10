#include "weather.hpp"
#include <imgui.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {
// the biome lines of biometools.glsl's groundCover
bool cold(glm::vec3 c) { return c.x < 0.36f; }
bool desert(glm::vec3 c) { return c.x > 0.6f && c.y < 0.45f; }
float approach(float value, float target, float dt, float seconds) { return value + (target - value) * std::min(dt / seconds, 1.0f); }
float smoothstep(float a, float b, float x) { float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f); return t * t * (3.0f - 2.0f * t); }

// clouds.glsl's cloudHash / cloudNoise, the same float math
float fract(float x) { return x - std::floor(x); }
float cloudHash(glm::vec2 p) {
	glm::vec3 p3(fract(p.x * 0.1031f), fract(p.y * 0.1031f), fract(p.x * 0.1031f));
	p3 += glm::dot(p3, glm::vec3(p3.y, p3.z, p3.x) + 33.33f);
	return fract((p3.x + p3.y) * p3.z);
}
float cloudNoise(glm::vec2 p) {
	glm::vec2 i = glm::floor(p), f = p - i;
	glm::vec2 u = f * f * (3.0f - 2.0f * f);
	float a = cloudHash(i), b = cloudHash(i + glm::vec2(1, 0)), c = cloudHash(i + glm::vec2(0, 1)), d = cloudHash(i + glm::vec2(1, 1));
	return glm::mix(glm::mix(a, b, u.x), glm::mix(c, d, u.x), u.y);
}
}

float weatherCellsAt(glm::vec2 worldKm) {
	glm::vec2 p = worldKm * 0.09f + glm::vec2(13.7f, -41.2f);
	return cloudNoise(p) * 0.65f + cloudNoise(p * 2.03f + glm::vec2(7.1f, 3.3f)) * 0.35f;
}
float weatherCellCore(float cells) { return smoothstep(0.55f, 0.78f, cells); }

const char* Weather::name(WeatherType type) {
	switch(type) {
	case WeatherType::Clear: return "Clear";
	case WeatherType::Cloudy: return "Cloudy";
	case WeatherType::Overcast: return "Overcast";
	case WeatherType::Rain: return "Rain";
	case WeatherType::Showers: return "Showers";
	case WeatherType::Thunderstorm: return "Thunderstorm";
	case WeatherType::Snow: return "Snow";
	case WeatherType::Sandstorm: return "Sandstorm";
	case WeatherType::Fog: return "Fog";
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

// rain where it's too cold falls as snow, sand only blows where it's a desert, nothing falls from a desert sky, fog
// needs damp air
bool Weather::fits(WeatherType type, glm::vec3 c) const {
	switch(type) {
	case WeatherType::Rain:
	case WeatherType::Showers:
	case WeatherType::Thunderstorm: return !cold(c) && !desert(c);
	case WeatherType::Snow: return cold(c);
	case WeatherType::Sandstorm: return desert(c);
	case WeatherType::Fog: return !desert(c) && c.y > 0.35f;
	default: return true;
	}
}

// what comes next: a weighted pick of what fits the climate. wetter places rain more, warm wet ones storm, deserts
// are mostly clear, damp cool ones get fog
WeatherType Weather::pick(glm::vec3 c) {
	float weights[static_cast<int>(WeatherType::Count)] = {};
	auto set = [&](WeatherType t, float w) { weights[static_cast<int>(t)] = w; };
	if(desert(c)) {
		set(WeatherType::Clear, 5.0f); set(WeatherType::Cloudy, 1.5f); set(WeatherType::Sandstorm, 2.5f);
	} else if(cold(c)) {
		set(WeatherType::Clear, 2.0f); set(WeatherType::Cloudy, 2.0f); set(WeatherType::Overcast, 1.5f); set(WeatherType::Snow, 3.0f);
		set(WeatherType::Fog, 0.8f * glm::clamp(c.y, 0.0f, 1.0f));
	} else {
		float wet = glm::clamp(c.y, 0.0f, 1.0f), warm = smoothstep(0.4f, 0.7f, c.x);
		set(WeatherType::Clear, 3.5f - wet * 1.5f); set(WeatherType::Cloudy, 3.0f); set(WeatherType::Overcast, 1.2f);
		set(WeatherType::Rain, 0.8f + wet * 1.6f); set(WeatherType::Showers, 1.0f + wet * 1.5f);
		set(WeatherType::Thunderstorm, 0.3f + wet * (0.5f + warm * 1.5f)); set(WeatherType::Fog, wet * 1.2f * (1.0f - warm * 0.6f));
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

// what each weather settles at (at intensity 1), regionally: what you get depends on what's overhead (localWeather).
// rain / snow amounts are for the field: rainDeck everywhere under a thick deck, rainCells under the cells
WeatherState Weather::target(WeatherType type) const {
	WeatherState s;
	switch(type) {
	case WeatherType::Clear:		s.coverage = 0.25f; s.density = 0.8f; s.wind = 0.2f; s.instability = 0.15f; s.cloudBase = 2.4f; break;
	case WeatherType::Cloudy:		s.coverage = 0.5f; s.density = 1.1f; s.overcast = 0.12f; s.wind = 0.35f; s.instability = 0.35f; s.cloudBase = 2.0f; break;
	case WeatherType::Overcast:		s.coverage = 0.9f; s.density = 1.6f; s.overcast = 0.65f; s.wind = 0.45f; s.haze = 0.2f; s.instability = 0.0f; s.cloudBase = 1.3f; break;
	case WeatherType::Rain:			s.coverage = 0.95f; s.density = 2.0f; s.overcast = 0.8f; s.rainDeck = 0.75f; s.wind = 0.55f; s.haze = 0.3f; s.instability = 0.15f; s.cloudBase = 1.0f; s.mist = 1.3f; break;
	case WeatherType::Showers:		s.coverage = 0.38f; s.density = 1.5f; s.overcast = 0.15f; s.rainCells = 0.9f; s.wind = 0.45f; s.instability = 0.75f; s.cloudBase = 1.6f; break;
	case WeatherType::Thunderstorm: s.coverage = 0.72f; s.density = 3.0f; s.overcast = 0.3f; s.rainDeck = 0.2f; s.rainCells = 1.0f; s.wind = 0.85f; s.haze = 0.2f; s.instability = 1.0f; s.cloudBase = 1.3f; break;
	case WeatherType::Snow:			s.coverage = 0.92f; s.density = 1.7f; s.overcast = 0.75f; s.rainDeck = 0.8f; s.wind = 0.35f; s.haze = 0.4f; s.instability = 0.1f; s.cloudBase = 1.1f; break;
	case WeatherType::Sandstorm:	s.coverage = 0.3f; s.density = 0.9f; s.overcast = 0.45f; s.sand = 1.0f; s.wind = 1.0f; s.haze = 3.5f; s.instability = 0.1f; s.cloudBase = 2.8f; break;
	case WeatherType::Fog:			s.coverage = 0.45f; s.density = 1.2f; s.overcast = 0.35f; s.wind = 0.08f; s.haze = 0.9f; s.instability = 0.0f; s.cloudBase = 0.7f; s.mist = 6.0f; break;
	default: break;
	}
	s.rainDeck *= m_Intensity; s.rainCells *= m_Intensity; s.sand *= m_Intensity; s.haze *= m_Intensity;
	s.wind = std::min(s.wind * m_WindScale, 1.0f);
	return s;
}

void Weather::forceType(WeatherType type) {
	m_Type = type;
	m_Regional = target(type);
	m_Snap = true;
	bool falls = m_Regional.rainDeck + m_Regional.rainCells > 0.0f;
	if(falls && type != WeatherType::Snow) m_State.wetness = 1.0f;
	if(type == WeatherType::Snow) m_State.snowCover = 1.0f;
}

// what reaches the camera: the regional weather plus what's overhead in the field. under a cell (a shower, a storm)
// it rains, the sky goes dark grey and the haze closes in; between the cells it's dry and brighter
void Weather::localWeather(float dt, glm::vec3 climate, glm::vec2 cameraKm, glm::vec2 windKm, bool snap) {
	const WeatherState& g = m_Regional;
	WeatherState& s = m_State;
	float cells = weatherCellsAt(cameraKm + windKm);
	float core = weatherCellCore(cells) * g.instability;
	s.cellsHere = core;
	// clouds.glsl's localCoverage / localPrecipitation (the rain amounts already have the weather's intensity in)
	float coverage = glm::clamp(g.coverage * (1.0f - g.instability * 0.45f) + core, 0.0f, 1.0f);
	float precipitation = glm::clamp(g.rainDeck * smoothstep(0.75f, 0.95f, coverage) + g.rainCells * smoothstep(0.2f, 0.8f, weatherCellCore(cells)), 0.0f, 1.5f);
	// it starts once the clouds are mostly in (blending into a rainy weather, the deck has to arrive first)
	precipitation *= smoothstep(0.55f, 0.85f, coverage);

	// what falls follows the climate you're in (automatic only, picked by hand it's what you asked for): walked or
	// teleported out of the snow into the desert, it stops within a few seconds
	bool snowFits = !m_Auto || fits(WeatherType::Snow, climate);
	bool rainFits = !m_Auto || fits(WeatherType::Rain, climate);
	bool sandFits = !m_Auto || fits(WeatherType::Sandstorm, climate);
	bool snowing = m_Type == WeatherType::Snow || (cold(climate) && m_Auto);
	float rainGoal = rainFits && !snowing ? precipitation : 0.0f;
	float snowGoal = snowFits && snowing ? precipitation : 0.0f;
	float sandGoal = sandFits ? g.sand : 0.0f;
	// a shower comes and goes as its cell drifts over in a minute or two: follow it within seconds
	auto ease = [&](float& v, float goal, float seconds) { v = snap ? goal : approach(v, goal, dt, seconds); };
	ease(s.rain, rainGoal, rainFits ? 4.0f : 2.5f);
	ease(s.snow, snowGoal, snowFits ? 6.0f : 2.5f);
	ease(s.sand, sandGoal, sandFits ? 10.0f : 2.5f);
	// the sky: the regional greyness, darker under a cell (a storm's base is near black). the cloud layer itself is
	// the regional values: the field does the local part in the shaders
	s.coverage = g.coverage;
	s.density = g.density;
	ease(s.overcast, glm::clamp(g.overcast + core * 0.55f + precipitation * 0.2f, 0.0f, 1.0f), 6.0f);
	ease(s.haze, g.haze + (s.rain + s.snow) * 0.9f, 5.0f);
	s.wind = std::min(g.wind + core * 0.25f, 1.0f); // gusts under the storms
	s.instability = g.instability;
	s.rainDeck = g.rainDeck;
	s.rainCells = g.rainCells;
	s.cloudBase = g.cloudBase;
	s.mist = g.mist;
}

// lightning: from the storm cells only. a strike every few seconds somewhere in a cell within ~20km, more often the
// more of the sky is storm; each a few quick flickers over ~half a second. the flash reaching you falls off with the
// strike's distance (the bolt itself and the cloud it lights are drawn by the clouds, ubo.weather4)
void Weather::lightning(float dt, glm::vec2 cameraKm, glm::vec2 windKm) {
	WeatherState& s = m_State;
	m_StrikeAge += dt;
	static const char* placed = std::getenv("VULKAN2_LIGHTNING"); // (the captures: one strike, placed below)
	if(m_Regional.instability > 0.8f && m_Regional.rainCells > 0.3f && !(placed && std::strchr(placed, ','))) {
		m_StrikeIn -= dt;
		if(m_StrikeIn <= 0.0f) {
			// somewhere in a cell: random spots around the camera, the first inside a storm core wins
			std::uniform_real_distribution<float> angle(0.0f, 6.2831853f), distance(0.8f, 1.0f);
			for(int attempt = 0; attempt < 32; attempt++) {
				float a = angle(m_Random), d = 1.5f + 18.5f * std::pow(distance(m_Random) * std::uniform_real_distribution<float>(0.0f, 1.0f)(m_Random), 0.7f);
				glm::vec2 offset = glm::vec2(std::cos(a), std::sin(a)) * d;
				if(weatherCellCore(weatherCellsAt(cameraKm + windKm + offset)) > 0.55f) {
					s.strikeKm = cameraKm + offset;
					m_StrikeDistance = d;
					m_StrikeAge = 0.0f;
					s.strikeSeed = std::uniform_real_distribution<float>(0.0f, 100.0f)(m_Random);
					break;
				}
			}
			m_StrikeIn = std::uniform_real_distribution<float>(2.0f, 10.0f)(m_Random) / std::max(m_Intensity, 0.3f);
		}
	}
	// VULKAN2_LIGHTNING (the captures): strikes right away and holds the first one at its brightest
	// (=dx,dz: at that offset from the camera, km: a strike in view)
	static const char* frozenEnv = std::getenv("VULKAN2_LIGHTNING");
	static const bool frozen = frozenEnv != nullptr;
	if(frozen && std::strchr(frozenEnv, ',')) { // (kept at that offset: the capture moves the camera to its spot first)
		s.strikeKm = cameraKm + glm::vec2(static_cast<float>(std::atof(frozenEnv)), static_cast<float>(std::atof(std::strchr(frozenEnv, ',') + 1)));
		m_StrikeDistance = glm::length(s.strikeKm - cameraKm);
		s.strikeSeed = 37.0f;
		if(m_StrikeAge > 1e8f) m_StrikeAge = 0.0f;
	}
	if(frozen) {
		if(m_StrikeAge > 1e8f) m_StrikeIn = std::min(m_StrikeIn, 0.0f);
		else m_StrikeAge = std::min(m_StrikeAge, 0.01f);
	}
	float pulse = 0.0f;
	if(m_StrikeAge < 0.7f) {
		// three pulses, the first the brightest, each decaying fast
		for(int i = 0; i < 3; i++) {
			float start = i * (0.08f + 0.05f * std::fmod(s.strikeSeed * (i + 1), 1.0f));
			float t = m_StrikeAge - start;
			if(t >= 0.0f) pulse += std::exp(-t * 18.0f) * (i == 0 ? 1.0f : 0.55f);
		}
		pulse *= 0.6f + 0.4f * std::fmod(s.strikeSeed, 1.0f);
	}
	s.strikeFlash = pulse;
	// what lights your surroundings: full right under it, a faint flicker on the horizon 20km out
	s.flash = pulse / (1.0f + (m_StrikeDistance / 4.0f) * (m_StrikeDistance / 4.0f));
}

void Weather::update(float dt, glm::vec3 climate, glm::vec2 cameraKm, glm::vec2 windKm) {
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

	// the regional weather eases towards its new values over a minute or so: the deck thickens, the air turns unstable
	// (cells grow out of the field and drift in with the wind), the cloud base comes down
	bool snap = m_Snap;
	m_Snap = false;
	WeatherState goal = target(m_Type);
	WeatherState& r = m_Regional;
	auto ease = [&](float& v, float g, float seconds) { v = snap ? g : approach(v, g, dt, seconds); };
	ease(r.coverage, goal.coverage, 40.0f);
	ease(r.density, goal.density, 40.0f);
	ease(r.overcast, goal.overcast, 35.0f);
	ease(r.wind, goal.wind, 25.0f);
	ease(r.haze, goal.haze, 30.0f);
	ease(r.sand, goal.sand, 20.0f);
	ease(r.instability, goal.instability, 50.0f);
	ease(r.rainDeck, goal.rainDeck, 30.0f);
	ease(r.rainCells, goal.rainCells, 30.0f);
	ease(r.cloudBase, goal.cloudBase, 60.0f);
	ease(r.mist, goal.mist, 45.0f);

	localWeather(dt, climate, cameraKm, windKm, snap);
	WeatherState& s = m_State;

	// the ground: soaked within a minute of heavy rain, dries in a few minutes (slower under an overcast sky)
	if(s.rain > 0.05f) s.wetness = std::min(s.wetness + s.rain * dt / 45.0f, 1.0f);
	else s.wetness = std::max(s.wetness - dt / (150.0f * (1.0f + s.overcast * 2.0f)), 0.0f);
	// snow: settles over a minute or two, melts over several (fast where it's warm, the rain washes it away)
	if(s.snow > 0.05f) s.snowCover = std::min(s.snowCover + s.snow * dt / 70.0f, 1.0f);
	else s.snowCover = std::max(s.snowCover - dt * ((desert(climate) ? 1.0f / 5.0f : climate.x > 0.4f ? 1.0f / 60.0f : 1.0f / 400.0f) + s.rain / 40.0f), 0.0f);
	// (the lighting only lets it settle where the ground's own climate is cold enough, lighting.comp)

	lightning(dt, cameraKm, windKm);
}

void Weather::drawImGui() {
	ImGui::SeparatorText("Weather");
	ImGui::Text("Now: %s   (%s here)", name(m_Type), climateName(m_Climate));
	ImGui::TextDisabled("overhead: %s, rain %.0f%%, cell %.0f%%", m_State.rain + m_State.snow > 0.05f ? "precipitation" : "dry",
						(m_State.rain + m_State.snow) * 100.0f, m_State.cellsHere * 100.0f);
	ImGui::Checkbox("Automatic", &m_Auto);
	ImGui::SetItemTooltip("Picks the weather by itself from where you are: rain, showers and storms in temperate and tropical land, snow where it's cold, sandstorms in deserts, fog in damp places");
	int current = static_cast<int>(m_Type);
	const char* names[] = { "Clear", "Cloudy", "Overcast", "Rain", "Showers", "Thunderstorm", "Snow", "Sandstorm", "Fog" };
	static_assert(IM_ARRAYSIZE(names) == static_cast<int>(WeatherType::Count));
	// what's set by hand applies right away, no minute of blending: the sky, what falls, the wind, the ground too
	if(ImGui::Combo("Weather", &current, names, IM_ARRAYSIZE(names))) {
		m_Auto = false; // picked by hand: it stays
		forceType(static_cast<WeatherType>(current));
	}
	bool changed = ImGui::SliderFloat("Intensity", &m_Intensity, 0.2f, 2.0f, "%.1fx");
	changed |= ImGui::SliderFloat("Wind", &m_WindScale, 0.0f, 2.0f, "%.1fx");
	if(changed) { m_Regional = target(m_Type); m_Snap = true; }
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
