#pragma once
#include <cstdint>

// every graphics option in one place, with quality presets. Application::applySettings pushes them into the
// renderer's parts (terrain UBO flags, shadow maps, foliage rings, ...) whenever something changed
enum class Quality : int { Low, Medium, High, Ultra, Custom };

struct GraphicsSettings {
	// display
	float renderScale = 1.0f;
	bool vsync = true;
	bool taa = true;
	float sharpening = 0.5f;

	// terrain
	float tessellation = 4.0f; // triangles get 40 / this many pixels
	bool triplanar = true;	 // cliffs textured from the side (2 more projections where it's steep)
	bool detailBumps = true; // small procedural bumps in the shading normal up close
	bool macroLayer = true;	 // a third, huge texture scale far away against tiling
	bool cliffNoTile = true;  // cliffs' rock texture varied per patch instead of a visible 7m repeat (~1ms on a screen of cliffs)
	float pomDistance = 35.0f; // metres, 0 = off
	int pomSteps = 12;

	// shadows
	bool shadows = true;
	bool softShadows = true;		// contact hardening (PCSS) up close, a plain filter otherwise
	int shadowSmoothing = 3;		// screen space blur, pixels
	int shadowResolution = 1;		// per cascade: 0 = 1024, 1 = 2048, 2 = 4096 (8 cascades, 256MB at 4096)
	int shadowRange = 1;			// how far the sharp cascades reach: 0 = close, 1 = balanced, 2 = far (SHADOW_RANGES)
	bool grassShadows = true;
	bool cloudShadows = true;

	// vegetation + props
	bool foliage = true;
	int grassRange = 2; // 0 = ~27m, 1 = ~110m, 2 = ~215m
	bool trees = true;
	float treeDensity = 1.0f;
	bool props = true;
	bool groundCover = true;		 // ferns, bushes, weeds, dead branches
	float groundCoverDensity = 1.0f;

	// lighting extras + post
	bool ssao = true;
	bool contactShadows = true;
	bool bounceLight = true;
	float bounceStrength = 1.0f; // the far field bounce (sunlit ground lighting what faces down / sideways)
	bool denoiseEffects = true; // the SSAO filtered (depth aware 3x3) as the lighting reads them
	// full instead of half resolution: sharper AO in grass / under trees, no halo around thin things. each roughly
	// 4x its half resolution cost (the clouds are the expensive one, ~1-3ms more at 1080p on an RTX 4060)
	bool fullResSSAO = false;
	int cloudResolution = 1; // the volumetric clouds (and the far fog): 0 = quarter, 1 = half, 2 = full resolution
	float bloom = 0.015f; // the mix of the bloom chain, energy conserving

	// sky
	bool clouds = true;
	bool aerialPerspective = true; // the haze over distance

	// volumetric fog (volfog.glsl): mist pooling in valleys, haze, beams through trees and past ridges
	bool volumetricFog = true;
	int fogQuality = 0;		  // froxel grid: 0 = 160x90x16, 1 = 240x135x32
	float fogDensity = 1.0f;  // everything
	float valleyMist = 1.0f;  // the ground mist on top of the haze

	// advanced: the systems behind the scenes
	float pomDepth = 27.0f;		  // cm between a height map's lowest and highest point
	float shadowSoftness = 1.5f;  // PCSS light size, 1 = the real sun
	bool grassInteraction = true; // grass + low leaves bend away from your feet while walking
	bool occlusionCulling = true; // trees hidden behind hills skipped (last frame's depth pyramid)
	bool terrainCache = true;	  // the far terrain baked into a texture instead of shaded per pixel
};

// where shadow cascades 0-2 end (world units, ~6m each). tighter = sharper near the player, but cascade 1 is also
// how far trees and rocks cast shadows (cascades 2-3 only have the terrain)
struct ShadowRange { const char* name; float splits[3]; };
inline constexpr ShadowRange SHADOW_RANGES[] = {
	{ "Close (60m / 360m / 2km)", { 10.0f, 60.0f, 350.0f } },
	{ "Balanced (85m / 540m / 3km)", { 14.0f, 90.0f, 500.0f } },
	{ "Far (120m / 840m / 5.4km)", { 20.0f, 140.0f, 900.0f } },
};

// the render resolution presets (a scale of the window, per axis). TAA upsamples below 100%, above it supersamples
struct ResolutionPreset { const char* name; float scale; };
inline constexpr ResolutionPreset RESOLUTION_PRESETS[] = {
	{ "Performance", 0.5f },
	{ "Balanced", 0.58f },
	{ "Quality", 0.67f },
	{ "Ultra quality", 0.77f },
	{ "Native", 1.0f },
	{ "Supersample 1.5x", 1.5f },
	{ "Supersample 2x", 2.0f },
};
inline constexpr int RESOLUTION_PRESET_COUNT = sizeof(RESOLUTION_PRESETS) / sizeof(RESOLUTION_PRESETS[0]);

inline GraphicsSettings qualityPreset(Quality q) {
	GraphicsSettings s; // High = the defaults
	switch(q) {
	case Quality::Low:
		s.renderScale = 0.75f;
		s.tessellation = 3.0f;
		s.triplanar = false; s.detailBumps = false; s.macroLayer = false; s.cliffNoTile = false;
		s.pomDistance = 0.0f;
		s.softShadows = false; s.shadowSmoothing = 1; s.shadowResolution = 0;
		s.grassShadows = false; s.cloudShadows = false;
		s.grassRange = 0; s.treeDensity = 0.7f; s.groundCoverDensity = 0.5f;
		s.ssao = false; s.contactShadows = false; s.bounceLight = false;
		s.volumetricFog = false;
		s.cloudResolution = 0; // quarter: the clouds are soft, the temporal resolve hides most of it
		break;
	case Quality::Medium:
		s.volumetricFog = false;
		s.tessellation = 4.0f;
		s.pomDistance = 25.0f; s.pomSteps = 8;
		s.shadowSmoothing = 2;
		s.grassShadows = false;
		s.grassRange = 1; s.groundCoverDensity = 0.75f;
		s.contactShadows = false; s.bounceLight = false;
		break;
	case Quality::Ultra: // tuned for a desktop GPU at 1080p (RTX 4060 class)
		s.renderScale = 1.0f; // native, TAA only cleans up
		s.tessellation = 7.0f;
		s.pomDistance = 80.0f; s.pomSteps = 24;
		s.shadowSmoothing = 4;
		s.shadowResolution = 2; // 4096: ~4cm texels around the player
		s.treeDensity = 1.3f;
		s.fullResSSAO = true; s.cloudResolution = 2;
		s.fogQuality = 1;
		break;
	default: break;
	}
	return s;
}

// every optional feature off (the "Everything off" button, VULKAN2_PRESET=off), resolution and vsync untouched
inline GraphicsSettings everythingOff(GraphicsSettings s) {
	s.taa = false; s.sharpening = 0.0f;
	s.triplanar = false; s.detailBumps = false; s.macroLayer = false; s.cliffNoTile = false;
	s.pomDistance = 0.0f;
	s.shadows = false; s.softShadows = false; s.shadowSmoothing = 0; s.grassShadows = false; s.cloudShadows = false;
	s.foliage = false; s.trees = false; s.props = false; s.groundCover = false;
	s.ssao = false; s.contactShadows = false; s.bounceLight = false; s.bloom = 0.0f;
	s.clouds = false; s.aerialPerspective = false;
	s.volumetricFog = false;
	return s;
}
