#pragma once
#include "color/recolor.h"

#include <string>

namespace lodlight
{
	struct Config
	{
		MatchParams match;
		RGB source{ 255.f, 147.f, 41.f }; // sourceHue is derived from this
		int logSamples = 0;               // log the first N raw entries seen
		bool logBlocks = false;           // one line per map data block
		int reloadKey = 0x78;             // VK_F9
		int menuKey = 0x79;               // VK_F10, 0 disables the overlay
		bool liveRepaint = true;          // remember loaded objects for instant repaint
		bool nearEnabled = true;          // also recolour model (near-tier) lights
		bool nearLog = false;             // log every model with lights
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
