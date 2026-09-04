#pragma once
#include "color/recolor.h"

#include <string>
#include <vector>

namespace vlights
{
	struct Config
	{
		MatchParams match;
		RGB source{ 255.f, 147.f, 41.f }; // sourceHue is derived from this
		RGB source2{ 255.f, 227.f, 166.f }; // cream freeway lamps; source2Hue is derived from this
		RGB source3{ 120.f, 255.f, 232.f }; // teal "cool" lamps (prop_streetlight_01b); source3Hue is derived from this
		bool allStreetLights = true;      // force every light on a street-lamp model / LOD street-light entry to the target
		std::vector<std::string> streetlightNames{ "streetlight" }; // model-name fragments that make a model a street lamp
		// Archetype names whose map placements get their light overrides forced
		// too (the ymap entity light effects). Models seen at load with a
		// streetlight name are added at runtime.
		std::vector<std::string> streetlightModels{
			"prop_streetlight_01", "prop_streetlight_01b", "prop_streetlight_02", "prop_streetlight_03", "prop_streetlight_03b",
			"prop_streetlight_03c", "prop_streetlight_03d", "prop_streetlight_03e", "prop_streetlight_04", "prop_streetlight_05",
			"prop_streetlight_05_b", "prop_streetlight_06", "prop_streetlight_07a", "prop_streetlight_07b", "prop_streetlight_08",
			"prop_streetlight_09", "prop_streetlight_10", "prop_streetlight_11a", "prop_streetlight_11b", "prop_streetlight_11c",
			"prop_streetlight_12a", "prop_streetlight_12b", "prop_streetlight_14a", "prop_streetlight_15a", "prop_streetlight_16a",
			"prop_streetlight_16b" };
		std::vector<std::string> textureExclude{ "rsn_" }; // texture-name fragments never recoloured (Rockstar sign art)
		// Textures retinted wholesale (every pixel toward the target, brightness
		// kept): lens textures whose colour is a tint too faint for any zone.
		std::vector<std::string> textureForce{ "prop_streetlight_01_bulb" };
		bool debug = false;               // master switch for all per-object logging below
		int logSamples = 0;               // log the first N raw entries seen
		bool logBlocks = false;           // one line per map data block
		int reloadKey = 0x78;             // VK_F9
		int menuKey = 0x79;               // VK_F10, 0 disables the overlay
		bool liveRepaint = true;          // remember loaded objects for instant repaint
		bool nearEnabled = true;          // also recolour model (near-tier) lights
		bool nearLog = false;             // log every model with lights
		bool nearSweep = true;
		float probeX = 0.f, probeY = 0.f; // debug: F9 dumps map entities / LOD lights near this point
		float probeRadius = 25.f;
		bool texturesEnabled = true;      // recolour lamp glow textures at load
		bool updateCheck = true;          // one GET of the latest GitHub release at startup; notice in the menu
		std::vector<std::string> textureNames{ "streetlight", "wall_light", "lamppost", "lamp_post", "ind_light", "oldlight" }; // lowercase substrings selecting textures
	};

	// Full ini text (with comments) for the given values. DefaultIniText()
	// is RenderIni(Config{}).
	std::string RenderIni(const Config& cfg);
	std::string DefaultIniText();

	// Writes RenderIni(cfg) to path.
	bool SaveConfig(const std::wstring& path, const Config& cfg, std::string& error);

	// Parses key = value lines. Unknown keys and bad values are reported in
	// `report` (one line each) but never abort loading; missing keys keep
	// their defaults. Returns false only if the file could not be opened.
	bool LoadConfig(const std::wstring& path, Config& out, std::string& report);

	// Writes DefaultIniText() to path if the file does not exist.
	bool EnsureDefaultConfig(const std::wstring& path);
}
