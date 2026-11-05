#include "application.hpp"
#include "worldgen.hpp"
#include <algorithm>
#include <cctype>
#include <string>

// the weather follows the climate under the camera (worldgen.hpp mirrors heightmap.comp's), into the atmosphere's
// UBO (clouds, wind, the weather vectors), the volumetric fog (rain haze, the sandstorm's dust) and the falling
// precipitation. VULKAN2_WEATHER=rain|thunderstorm|snow|sandstorm|... fixes it (for the captures)
void Application::updateWeather(float dt) {
	static bool fixedFromEnv = false;
	if(!fixedFromEnv) {
		fixedFromEnv = true;
		if(const char* env = std::getenv("VULKAN2_WEATHER")) {
			for(int i = 0; i < static_cast<int>(WeatherType::Count); i++) {
				std::string name = Weather::name(static_cast<WeatherType>(i));
				std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
				if(name == env) { m_Weather.m_Auto = false; m_Weather.forceType(static_cast<WeatherType>(i)); }
			}
		}
	}
	float terrainSize = static_cast<float>(m_WorldSize) * m_TerrainScale;
	glm::vec2 p = worldgen::noisePosition(glm::vec2(m_Camera.position.x, m_Camera.position.z), terrainSize, static_cast<float>(m_HeightmapSize) / terrainSize, static_cast<float>(m_HeightmapSize));
	m_Weather.update(dt, worldgen::climateAt(p));
	m_AtmosphereManager.setWeather(m_Weather.state());
}

// rain / snow / sand onto the output image (GENERAL, written by the TAA's compute before, read by the bloom's after)
void Application::drawPrecipitation(VkCommandBuffer cmd) {
	const WeatherState& w = m_Weather.state();
	PrecipitationPushConstants pc{};
	pc.counts = glm::ivec4(static_cast<int>(Precipitation::MAX_RAIN * std::min(w.rain, 1.5f) / 1.5f),
						   static_cast<int>(Precipitation::MAX_SNOW * std::min(w.snow, 1.5f) / 1.5f),
						   static_cast<int>(Precipitation::MAX_SAND * std::min(w.sand, 1.5f) / 1.5f), 0);
	if(pc.counts.x + pc.counts.y + pc.counts.z == 0) return;

	auto barrier = [&](VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess) {
		VkMemoryBarrier2 b{ .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
		b.srcStageMask = srcStage; b.srcAccessMask = srcAccess; b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
		VkDependencyInfo dependency{ .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
		dependency.memoryBarrierCount = 1;
		dependency.pMemoryBarriers = &b;
		vkCmdPipelineBarrier2(cmd, &dependency);
	};
	barrier(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
			VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

	pc.viewProj = m_UnjitteredProjection * m_View;
	pc.campos = m_CameraPos;
	pc.time = m_Time;
	pc.offsetWorld = glm::vec2(m_MapOffset) * (m_TerrainScale / static_cast<float>(m_CoordinateMultiplier));
	// the wind (the clouds' direction): up to ~18 m/s in a storm
	pc.wind = glm::normalize(glm::vec2(0.8f, 0.35f)) * (0.3f + w.wind * 2.7f);
	float projScale = static_cast<float>(m_SwapchainExtent.height) * 0.5f * std::abs(m_UnjitteredProjection[1][1]);
	pc.view = glm::vec4(static_cast<float>(m_SwapchainExtent.width), static_cast<float>(m_SwapchainExtent.height), projScale, 0.0f);
	pc.sundir = m_SunDir;
	// the camera's velocity (continuous world space, smoothed; a teleport or a map shift isn't motion)
	static glm::vec3 lastPosition = m_Camera.position, velocity(0.0f);
	glm::vec3 moved = (m_Camera.position - lastPosition) / std::max(m_Deltatime, 1e-4f);
	lastPosition = m_Camera.position;
	if(glm::length(moved) < 60.0f) velocity += (moved - velocity) * std::min(m_Deltatime * 10.0f, 1.0f);
	pc.motion = glm::vec4(velocity, 0.0f);
	m_Precipitation.draw(cmd, pc, m_AtmosphereManager.getRenderDescriptor(), m_OutputImage.imageView, m_SwapchainExtent);

	barrier(VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
			VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
}

// GPU memory: the device local heaps (on an integrated GPU that's the shared system memory), what the whole
// system uses of them against what the driver says is available, and this program's own share
Application::GpuMemory Application::gpuMemory() const {
	VkPhysicalDeviceMemoryProperties properties;
	vkGetPhysicalDeviceMemoryProperties(m_GPU, &properties);
	VmaBudget budgets[VK_MAX_MEMORY_HEAPS];
	vmaGetHeapBudgets(m_Allocator, budgets);
	GpuMemory memory{};
	for(uint32_t heap = 0; heap < properties.memoryHeapCount; heap++) {
		if(!(properties.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)) continue;
		memory.used += static_cast<double>(budgets[heap].usage);
		memory.available += static_cast<double>(budgets[heap].budget);
		memory.ours += static_cast<double>(budgets[heap].statistics.blockBytes);
	}
	return memory;
}

void Application::drawMemoryBudget() {
	GpuMemory memory = gpuMemory();
	const double MB = 1024.0 * 1024.0;
	float fraction = memory.available > 0.0 ? static_cast<float>(memory.used / memory.available) : 0.0f;
	char text[96];
	snprintf(text, sizeof(text), "%.0f / %.0f MB (this program %.0f MB)", memory.used / MB, memory.available / MB, memory.ours / MB);
	ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0.0f), text);
	ImGui::SetItemTooltip(m_MemoryBudgetExtension ? "GPU memory, the device local heaps: everything using them / the driver's budget"
												  : "GPU memory, the device local heaps (estimated: no VK_EXT_memory_budget)");
}

// the settings window: presets on top, then every option grouped. any change applies right away and switches the
// preset to Custom
void Application::drawSettings() {
	GraphicsSettings& s = m_Settings;
	bool changed = false;

	const char* presets[] = { "Low", "Medium", "High", "Ultra", "Custom" };
	int quality = static_cast<int>(m_Quality);
	if(ImGui::Combo("Quality", &quality, presets, 5) && quality != static_cast<int>(Quality::Custom)) {
		m_Quality = static_cast<Quality>(quality);
		bool vsync = s.vsync; // a display choice, not a quality one
		s = qualityPreset(m_Quality);
		s.vsync = vsync;
		applySettings();
	}

	drawMemoryBudget();

	auto check = [&](const char* label, bool* value) { changed |= ImGui::Checkbox(label, value); };
	auto slider = [&](const char* label, float* value, float lo, float hi, const char* format = "%.2f") { changed |= ImGui::SliderFloat(label, value, lo, hi, format); };

	ImGui::SeparatorText("Display");
	check("VSync", &s.vsync);
	{
		// resolution presets instead of a free slider, each showing the resolution it renders at
		int windowW = 0, windowH = 0;
		SDL_GetWindowSizeInPixels(m_pWindow, &windowW, &windowH);
		char labels[RESOLUTION_PRESET_COUNT][64];
		int current = -1;
		for(int i = 0; i < RESOLUTION_PRESET_COUNT; i++) {
			const ResolutionPreset& preset = RESOLUTION_PRESETS[i];
			snprintf(labels[i], sizeof(labels[i]), "%s (%dx%d, %.0f%%)", preset.name, static_cast<int>(windowW * preset.scale),
					 static_cast<int>(windowH * preset.scale), preset.scale * 100.0f);
			if(std::abs(s.renderScale - preset.scale) < 0.005f) current = i;
		}
		char custom[64];
		snprintf(custom, sizeof(custom), "Custom (%.0f%%)", s.renderScale * 100.0f);
		if(ImGui::BeginCombo("Resolution", current >= 0 ? labels[current] : custom)) {
			for(int i = 0; i < RESOLUTION_PRESET_COUNT; i++) {
				if(ImGui::Selectable(labels[i], i == current)) {
					s.renderScale = RESOLUTION_PRESETS[i].scale;
					changed = true;
				}
				if(i == current) ImGui::SetItemDefaultFocus();
			}
			ImGui::EndCombo();
		}
		ImGui::SetItemTooltip("Below Native, TAA upscales to the window. Supersampling renders above it and is expensive");
	}
	check("TAA", &s.taa);
	ImGui::SameLine();
	slider("Sharpening", &s.sharpening, 0.0f, 1.0f);

	ImGui::SeparatorText("Terrain");
	slider("Tessellation", &s.tessellation, 1.0f, 10.0f, "%.1f");
	check("Cliff texturing (triplanar)", &s.triplanar);
	ImGui::SameLine();
	check("Detail bumps", &s.detailBumps);
	ImGui::SameLine();
	check("Anti-tiling layer", &s.macroLayer);
	slider("Parallax distance (m)", &s.pomDistance, 0.0f, 150.0f, "%.0f");
	changed |= ImGui::SliderInt("Parallax steps", &s.pomSteps, 2, 64);
	check("Parallax self shadows", &s.pomShadows);

	ImGui::SeparatorText("Shadows");
	check("Shadows", &s.shadows);
	check("Soft shadows (PCSS)", &s.softShadows);
	changed |= ImGui::SliderInt("Shadow smoothing (px)", &s.shadowSmoothing, 0, 8);
	const char* shadowResolutions[] = { "1024 (low)", "2048", "4096 (ultra, 256MB)" };
	changed |= ImGui::Combo("Shadow resolution", &s.shadowResolution, shadowResolutions, 3);
	ImGui::SetItemTooltip("Texels per side of every cascade. Higher is sharper, costs memory and a little re-render time");
	const char* shadowRanges[] = { SHADOW_RANGES[0].name, SHADOW_RANGES[1].name, SHADOW_RANGES[2].name };
	changed |= ImGui::Combo("Shadow cascades", &s.shadowRange, shadowRanges, 3);
	ImGui::SetItemTooltip("Where the sharp cascades end. Close is crispest around you, but trees and rocks stop casting shadows sooner");
	check("Grass shadows", &s.grassShadows);
	ImGui::SameLine();
	check("Cloud shadows", &s.cloudShadows);

	ImGui::SeparatorText("Vegetation");
	check("Grass + flowers", &s.foliage);
	const char* ranges[] = { "Near (~27m)", "Medium (~110m)", "Far (~215m, wheat ~420m)" };
	changed |= ImGui::Combo("Grass distance", &s.grassRange, ranges, 3);
	check("Trees", &s.trees);
	slider("Tree density", &s.treeDensity, 0.0f, 3.0f);
	check("Rocks + logs", &s.props);
	check("Ground cover", &s.groundCover);
	slider("Ground cover density", &s.groundCoverDensity, 0.0f, 2.0f);

	ImGui::SeparatorText("Lighting + post");
	check("SSAO", &s.ssao);
	ImGui::SameLine();
	check("Contact shadows", &s.contactShadows);
	ImGui::SameLine();
	check("Bounce light", &s.bounceLight);
	ImGui::BeginDisabled(!s.bounceLight);
	slider("Bounce light strength", &s.bounceStrength, 0.0f, 3.0f);
	ImGui::SetItemTooltip("The far field bounce: sunlit ground lighting whatever faces down or sideways (under overhangs, tree trunks, cliff faces). Cheap, analytic, no noise");
	ImGui::EndDisabled();
	check("Denoise SSAO", &s.denoiseEffects);
	ImGui::SetItemTooltip("A depth aware 3x3 filter as the lighting reads them: most of the grain gone before the TAA, a little softer. Nearly free");
	ImGui::TextDisabled("Full resolution (Ultra):");
	ImGui::SameLine();
	check("SSAO##full", &s.fullResSSAO);
	ImGui::SameLine();
	check("Clouds##full", &s.fullResClouds);
	ImGui::SetItemTooltip("Rendered at full instead of half resolution: sharper, no halos around thin objects, ~4x the cost each");
	slider("Bloom", &s.bloom, 0.0f, 0.2f, "%.3f");

	ImGui::SeparatorText("Sky");
	check("Clouds", &s.clouds);
	ImGui::SameLine();
	check("Volumetric clouds", &s.cloudDetail);
	check("Atmospheric haze", &s.aerialPerspective);
	check("Volumetric fog", &s.volumetricFog);
	ImGui::SetItemTooltip("Mist pooling in the valleys (thickest around sunrise), haze, and light shafts through the trees");
	ImGui::BeginDisabled(!s.volumetricFog);
	const char* fogQualities[] = { "160x90x16", "240x135x32 (ultra)" };
	changed |= ImGui::Combo("Fog resolution", &s.fogQuality, fogQualities, 2);
	slider("Fog density", &s.fogDensity, 0.0f, 4.0f);
	slider("Valley mist", &s.valleyMist, 0.0f, 4.0f);
	ImGui::EndDisabled();

	if(ImGui::CollapsingHeader("Advanced")) {
		slider("Parallax depth (cm)", &s.pomDepth, 0.0f, 80.0f, "%.0f");
		slider("Shadow softness (sun size)", &s.shadowSoftness, 0.25f, 4.0f);
		ImGui::SetItemTooltip("How quickly shadows soften away from what casts them (PCSS light size, 1 = the real sun)");
		check("Grass bends around you", &s.grassInteraction);
		check("Occlusion culling", &s.occlusionCulling);
		ImGui::SetItemTooltip("Skips trees hidden behind hills. Only turn off to compare, it's free performance");
		check("Far terrain cache", &s.terrainCache);
		ImGui::SetItemTooltip("Bakes the distant terrain's look into a texture. Off shades every pixel in full: slower, a touch sharper far away");
	}

	if(ImGui::Button("Everything off")) {
		s = everythingOff(s);
		changed = true;
	}

	if(changed) {
		m_Quality = Quality::Custom;
		applySettings();
	}
}

void Application::applySettings() {
	const GraphicsSettings& s = m_Settings;

	if(s.vsync != m_Vsync || s.renderScale != m_RenderScale) m_Resized = true;
	// the effect images are recreated at their new resolution along with the swapchain
	if(s.fullResSSAO != m_FullResSSAO || s.fullResClouds != m_FullResClouds) m_Resized = true;
	m_FullResSSAO = s.fullResSSAO;
	m_FullResClouds = s.fullResClouds;
	m_Vsync = s.vsync;
	m_RenderScale = s.renderScale;
	m_TAA = s.taa;
	m_Sharpness = s.sharpening;

	m_TerrainPC.tessellationFactor = s.tessellation;

	// the shadow casters change with these, the cached cascades have to be re-rendered
	bool recast = m_Foliage.m_CastShadows != s.grassShadows || m_Trees.m_Enabled != s.trees || m_Trees.m_Density != s.treeDensity
				|| m_Props.m_Enabled != s.props || m_Foliage.m_Enabled != s.foliage
				|| m_GroundCover.m_Enabled != s.groundCover || m_GroundCover.m_Density != s.groundCoverDensity;
	m_Shadows.setQuality(s.shadows, s.softShadows);
	{
		uint32_t resolution = 1024u << std::clamp(s.shadowResolution, 0, 2);
		if(m_Device && m_Shadows.getResolution() != resolution) { // frames in flight sample the old maps
			std::lock_guard<std::mutex> lock(m_QueueMutex); // the loading screen presents from the main thread meanwhile
			vkDeviceWaitIdle(m_Device);
		}
		if(m_Shadows.setResolution(resolution)) m_DebugView.setFallbackArray(m_Shadows.getArrayView());
		const ShadowRange& range = SHADOW_RANGES[std::clamp(s.shadowRange, 0, 2)];
		m_Shadows.setNearSplits(range.splits[0], range.splits[1], range.splits[2]);
	}
	if(recast) m_Shadows.invalidate();
	m_ShadowFilter.m_Radius = s.shadowSmoothing;
	m_Shadows.setSoftness(s.shadowSoftness);
	m_GrassInteraction = s.grassInteraction;
	m_OcclusionCulling = s.occlusionCulling;
	if(s.terrainCache && !m_TerrainCacheEnabled) m_TerrainCache.invalidate(); // it wasn't kept up to date while off
	m_TerrainCacheEnabled = s.terrainCache;

	// the forest canopy in the sky visibility map follows the trees: all of it re-baked when they change
	bool canopyChanged = m_Trees.m_Enabled != s.trees || m_Trees.m_Density != s.treeDensity;
	m_Foliage.m_Enabled = s.foliage;
	m_Foliage.m_Rings = s.grassRange >= 2 ? 4 : static_cast<uint32_t>(s.grassRange) + 1; // far includes the wheat ring
	m_Foliage.m_CastShadows = s.grassShadows;
	m_Trees.m_Enabled = s.trees;
	m_Trees.m_Density = s.treeDensity;
	m_Props.m_Enabled = s.props;
	m_GroundCover.m_Enabled = s.groundCover;
	m_GroundCover.m_Density = s.groundCoverDensity;
	if(canopyChanged && m_Device && m_SkyVisibilityReady) {
		{
			std::lock_guard<std::mutex> lock(m_QueueMutex);
			vkDeviceWaitIdle(m_Device); // frames in flight sample it
		}
		immediateSubmit([&](VkCommandBuffer cmd) { dispatchHorizonAO(cmd, glm::ivec2(0), glm::ivec2(m_HeightmapSize)); });
	}

	m_SSAO = s.ssao;
	m_ContactShadows = s.contactShadows;
	m_BounceLight = s.bounceLight;
	m_BounceStrength = s.bounceStrength;
	m_DenoiseEffects = s.denoiseEffects;
	m_AtmosphereManager.setBloom(s.bloom);
	m_AtmosphereManager.m_AerialPerspective = s.aerialPerspective;

	// the terrain's options live in the atmosphere UBO (terrain.frag, lighting.comp read them)
	AtmosphereUBO& ubo = m_AtmosphereManager.m_AtmosphereData;
	float metersPerUnit = ubo.worldScale * 1000.0f;
	ubo.pomDistance = s.pomDistance / metersPerUnit;
	ubo.pomSteps = static_cast<float>(s.pomSteps);
	ubo.pomShadow = s.pomShadows ? 1.0f : 0.0f;
	ubo.pomDepth = s.pomDepth * 0.01f / metersPerUnit;
	ubo.terrainCache = s.terrainCache ? 1.0f : 0.0f;
	ubo.terrainTriplanar = s.triplanar ? 1.0f : 0.0f;
	ubo.terrainBumps = s.detailBumps ? 1.0f : 0.0f;
	ubo.terrainMacro = s.macroLayer ? 1.0f : 0.0f;
	m_TerrainCache.invalidate(); // the look it baked may have changed (triplanar, ...)
	ubo.cloudShadows = s.cloudShadows ? 1.0f : 0.0f;
	ubo.clouds = s.clouds ? 1.0f : 0.0f;
	ubo.cloudQuality = s.cloudDetail ? 1.0f : 0.0f;
	ubo.aerialPerspective = s.aerialPerspective ? 1.0f : 0.0f;

	// volumetric fog: 2.4km of froxels (the aerial perspective takes over past that)
	m_VolumetricFog = s.volumetricFog;
	m_FogDensity = s.fogDensity;
	m_FogMist = s.valleyMist;
	ubo.volumetricFog = s.volumetricFog ? 1.0f : 0.0f;
	ubo.fogDistance = 2400.0f / metersPerUnit;
	glm::uvec3 fogGrid = s.fogQuality >= 1 ? glm::uvec3(240, 135, 32) : glm::uvec3(160, 90, 16); // (a quarter of the slices they once had: the frame barely changes, measured with the slices multiplier)
	if(fogGrid != m_FogGrid) {
		if(m_FogInjectLayout == VK_NULL_HANDLE) m_FogGrid = fogGrid; // not set up yet, initVolumetricFog uses it
		else {
			{
				std::lock_guard<std::mutex> lock(m_QueueMutex);
				vkDeviceWaitIdle(m_Device); // frames in flight still read the old volumes
			}
			createFogVolumes(fogGrid);
		}
	}
	if(m_Device) {
		{
			std::lock_guard<std::mutex> lock(m_QueueMutex); // (applySettings also runs on the loading thread)
			vkDeviceWaitIdle(m_Device); // the UBO is shared with the frames in flight
		}
		m_AtmosphereManager.updateUBO(ubo);
	}
}
