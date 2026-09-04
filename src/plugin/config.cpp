#include "plugin/config.h"

#include <windows.h>
#include <cmath>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace lodlight
{
	static std::string ColorText(const RGB& c)
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "%ld %ld %ld", std::lround(c.r), std::lround(c.g), std::lround(c.b));
		return buf;
	}

	static std::string KeyText(int vk)
	{
		if (vk >= VK_F1 && vk <= VK_F24)
			return "F" + std::to_string(vk - VK_F1 + 1);
		char buf[16];
		snprintf(buf, sizeof(buf), "0x%X", vk);
		return buf;
	}

	static std::string FloatText(float v)
	{
		char buf[32];
		snprintf(buf, sizeof(buf), "%.2f", v);
		return buf;
	}

	std::string RenderIni(const Config& cfg)
	{
		std::string s;
		s += "# LOD Light Recolor - config\n";
		s += "# Lives next to LodLightRecolor.asi. Press the reload key in-game to\n";
		s += "# re-read it, or use the in-game menu (menu key) and Save.\n";
		s += "\n";
		s += "enabled = " + std::string(cfg.match.enabled ? "1" : "0") + "\n";
		s += "\n";
		s += "# Colours are 'R G B' (0-255) or '#RRGGBB'.\n";
		s += "# source: the colour being hunted (only its hue is used).\n";
		s += "source = " + ColorText(cfg.source) + "\n";
		s += "# target: what matching lights become.\n";
		s += "target = " + ColorText(cfg.match.target) + "\n";
		s += "\n";
		s += "# A light matches when its hue is within hue_window degrees of source's\n";
		s += "# hue AND its saturation is at least min_saturation (0-1). Vanilla sodium\n";
		s += "# street lights decode to hue 17-30 with saturation 0.75-1.0; warm amber\n";
		s += "# signage sits at hue 36-45 / saturation 0.35-0.6; whites are ~0.1.\n";
		s += "hue_window = " + FloatText(cfg.match.hueWindow) + "\n";
		s += "min_saturation = " + FloatText(cfg.match.minSaturation) + "\n";
		s += "\n";
		s += "# Second match zone for the cream freeway lamps (prop_streetlight_06/_08,\n";
		s += "# light colour 255 227 166: hue 41, saturation 0.35). A saturation ceiling\n";
		s += "# keeps amber runway lights (hue 44, saturation 0.99) out of it.\n";
		s += "match_cream = " + std::string(cfg.match.zone2 ? "1" : "0") + "\n";
		s += "source2 = " + ColorText(cfg.source2) + "\n";
		s += "hue_window2 = " + FloatText(cfg.match.hueWindow2) + "\n";
		s += "min_saturation2 = " + FloatText(cfg.match.minSaturation2) + "\n";
		s += "max_saturation2 = " + FloatText(cfg.match.maxSaturation2) + "\n";
		s += "\n";
		s += "# blend: 1 = replace with target, 0.5 = halfway, 0 = leave untouched.\n";
		s += "blend = " + FloatText(cfg.match.blend) + "\n";
		s += "# keep_brightness: scale target so each light keeps its own brightness\n";
		s += "# instead of every light becoming identical.\n";
		s += "keep_brightness = " + std::string(cfg.match.keepBrightness ? "1" : "0") + "\n";
		s += "\n";
		s += "# near_enabled: also recolour near-tier lights (the light definitions baked\n";
		s += "# into lamp-post models and the per-entity light overrides stored in map\n";
		s += "# blocks), using the same match and target. 0 = LOD/distant lights only.\n";
		s += "# Takes effect at game start (hooks are installed then). Near lights take\n";
		s += "# a new colour as they stream in again, not instantly.\n";
		s += "near_enabled = " + std::string(cfg.nearEnabled ? "1" : "0") + "\n";
		s += "\n";
		s += "# Diagnostics (written to lodlight_recolor.log next to the .asi).\n";
		s += "# debug: master switch. With 0 nothing below is logged during play, whatever\n";
		s += "# the individual keys say. Leave at 0 unless investigating: per-object\n";
		s += "# logging costs frame time while models stream in.\n";
		s += "debug = " + std::string(cfg.debug ? "1" : "0") + "\n";
		s += "# log_samples: log the first N raw entries seen, with their decoded\n";
		s += "# colour. log_blocks: one line per map data block with counts.\n";
		s += "log_samples = " + std::to_string(cfg.logSamples) + "\n";
		s += "log_blocks = " + std::string(cfg.logBlocks ? "1" : "0") + "\n";
		s += "# near_log: one line per model / map block that has lights, with colours.\n";
		s += "near_log = " + std::string(cfg.nearLog ? "1" : "0") + "\n";
		s += "# probe: 'x y radius'. With debug on, every map block that streams in is\n";
		s += "# scanned and its entities / LOD lights within radius of that world point\n";
		s += "# are logged (model hash, extensions, original colour). 0 0 = off.\n";
		s += "probe = " + FloatText(cfg.probeX) + " " + FloatText(cfg.probeY) + " " + FloatText(cfg.probeRadius) + "\n";
		s += "\n";
		s += "# Hotkeys: F1-F24, or a hex/decimal virtual-key code. 0 disables.\n";
		s += "# reload_key re-reads this file and repaints loaded lights.\n";
		s += "# menu_key toggles the in-game menu.\n";
		s += "reload_key = " + KeyText(cfg.reloadKey) + "\n";
		s += "menu_key = " + KeyText(cfg.menuKey) + "\n";
		s += "\n";
		s += "# live_repaint: remember loaded blocks so reload/menu changes repaint them\n";
		s += "# instantly (detours the ymap store's Remove; no vtable writes). 0 = off,\n";
		s += "# changes then apply only to blocks that stream in later.\n";
		s += "live_repaint = " + std::string(cfg.liveRepaint ? "1" : "0") + "\n";
		return s;
	}

	std::string DefaultIniText()
	{
		return RenderIni(Config{});
	}

	bool SaveConfig(const std::wstring& path, const Config& cfg, std::string& error)
	{
		FILE* f = _wfopen(path.c_str(), L"w");
		if (!f)
		{
			error = "could not open config file for writing";
			return false;
		}
		std::string text = RenderIni(cfg);
		bool ok = fputs(text.c_str(), f) >= 0;
		fclose(f);
		if (!ok)
			error = "write failed";
		return ok;
	}

	static std::string Trim(const std::string& s)
	{
		size_t a = 0, b = s.size();
		while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
			++a;
		while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
			--b;
		return s.substr(a, b - a);
	}

	static std::string Lower(std::string s)
	{
		for (auto& c : s)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		return s;
	}

	static bool ParseBool(const std::string& v, bool& out)
	{
		std::string s = Lower(v);
		if (s == "1" || s == "true" || s == "yes" || s == "on") { out = true; return true; }
		if (s == "0" || s == "false" || s == "no" || s == "off") { out = false; return true; }
		return false;
	}

	static bool ParseFloat(const std::string& v, float& out)
	{
		char* end = nullptr;
		float f = std::strtof(v.c_str(), &end);
		if (!end || end == v.c_str() || *end != '\0')
			return false;
		out = f;
		return true;
	}

	static bool ParseInt(const std::string& v, int& out)
	{
		char* end = nullptr;
		long i = std::strtol(v.c_str(), &end, 0);
		if (!end || end == v.c_str() || *end != '\0')
			return false;
		out = static_cast<int>(i);
		return true;
	}

	static bool ParseColor(const std::string& v, RGB& out)
	{
		std::string s = Trim(v);
		if (!s.empty() && s[0] == '#')
			s = s.substr(1);

		// #RRGGBB
		if (s.size() == 6 && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isxdigit(c); }))
		{
			unsigned long x = std::strtoul(s.c_str(), nullptr, 16);
			out = RGB{ float((x >> 16) & 0xFF), float((x >> 8) & 0xFF), float(x & 0xFF) };
			return true;
		}

		// R G B  /  R,G,B
		for (auto& c : s)
			if (c == ',')
				c = ' ';
		std::istringstream ss(s);
		int r, g, b;
		if (!(ss >> r >> g >> b))
			return false;
		std::string rest;
		if (ss >> rest)
			return false;
		if (r < 0 || g < 0 || b < 0 || r > 255 || g > 255 || b > 255)
			return false;
		out = RGB{ float(r), float(g), float(b) };
		return true;
	}

	static bool ParseKey(const std::string& v, int& out)
	{
		std::string s = Lower(Trim(v));
		if (s.size() >= 2 && s[0] == 'f')
		{
			int n = 0;
			if (ParseInt(s.substr(1), n) && n >= 1 && n <= 24)
			{
				out = VK_F1 + (n - 1);
				return true;
			}
			return false;
		}
		return ParseInt(s, out);
	}

	bool LoadConfig(const std::wstring& path, Config& out, std::string& report)
	{
		std::ifstream in(path.c_str());
		if (!in)
			return false;

		Config cfg; // start from defaults so missing keys are well-defined
		std::string line;
		int lineNo = 0;
		while (std::getline(in, line))
		{
			++lineNo;
			std::string t = Trim(line);
			if (t.empty() || t[0] == '#' || t[0] == ';')
				continue;
			size_t eq = t.find('=');
			if (eq == std::string::npos)
			{
				report += "line " + std::to_string(lineNo) + ": expected key = value\n";
				continue;
			}
			std::string key = Lower(Trim(t.substr(0, eq)));
			std::string val = Trim(t.substr(eq + 1));

			bool ok = true;
			if (key == "enabled")             ok = ParseBool(val, cfg.match.enabled);
			else if (key == "source")         ok = ParseColor(val, cfg.source);
			else if (key == "target")         ok = ParseColor(val, cfg.match.target);
			else if (key == "hue_window")     ok = ParseFloat(val, cfg.match.hueWindow);
			else if (key == "min_saturation") ok = ParseFloat(val, cfg.match.minSaturation);
			else if (key == "match_cream")    ok = ParseBool(val, cfg.match.zone2);
			else if (key == "source2")        ok = ParseColor(val, cfg.source2);
			else if (key == "hue_window2")    ok = ParseFloat(val, cfg.match.hueWindow2);
			else if (key == "min_saturation2") ok = ParseFloat(val, cfg.match.minSaturation2);
			else if (key == "max_saturation2") ok = ParseFloat(val, cfg.match.maxSaturation2);
			else if (key == "blend")          ok = ParseFloat(val, cfg.match.blend);
			else if (key == "keep_brightness") ok = ParseBool(val, cfg.match.keepBrightness);
			else if (key == "debug")          ok = ParseBool(val, cfg.debug);
			else if (key == "log_samples")    ok = ParseInt(val, cfg.logSamples);
			else if (key == "log_blocks")     ok = ParseBool(val, cfg.logBlocks);
			else if (key == "reload_key")     ok = ParseKey(val, cfg.reloadKey);
			else if (key == "menu_key")       ok = ParseKey(val, cfg.menuKey);
			else if (key == "live_repaint")   ok = ParseBool(val, cfg.liveRepaint);
			else if (key == "near_enabled")   ok = ParseBool(val, cfg.nearEnabled);
			else if (key == "near_log")       ok = ParseBool(val, cfg.nearLog);
			else if (key == "probe")
			{
				std::string t = val;
				std::replace(t.begin(), t.end(), ',', ' ');
				char* end = nullptr;
				float x = std::strtof(t.c_str(), &end);
				float y = std::strtof(end, &end);
				float r = std::strtof(end, &end);
				ok = end != t.c_str();
				if (ok) { cfg.probeX = x; cfg.probeY = y; cfg.probeRadius = r > 0.f ? r : 25.f; }
			}
			else
			{
				report += "line " + std::to_string(lineNo) + ": unknown key '" + key + "'\n";
				continue;
			}
			if (!ok)
				report += "line " + std::to_string(lineNo) + ": bad value for '" + key + "': '" + val + "'\n";
		}

		cfg.match.sourceHue = ToHSV(cfg.source).h;
		cfg.match.source2Hue = ToHSV(cfg.source2).h;
		cfg.match.hueWindow2 = std::clamp(cfg.match.hueWindow2, 0.f, 180.f);
		cfg.match.minSaturation2 = std::clamp(cfg.match.minSaturation2, 0.f, 1.f);
		cfg.match.maxSaturation2 = std::clamp(cfg.match.maxSaturation2, 0.f, 1.f);
		cfg.match.hueWindow = std::clamp(cfg.match.hueWindow, 0.f, 180.f);
		cfg.match.minSaturation = std::clamp(cfg.match.minSaturation, 0.f, 1.f);
		cfg.match.blend = std::clamp(cfg.match.blend, 0.f, 1.f);
		cfg.logSamples = std::max(cfg.logSamples, 0);

		out = cfg;
		return true;
	}

	bool EnsureDefaultConfig(const std::wstring& path)
	{
		if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
			return true;
		FILE* f = _wfopen(path.c_str(), L"w");
		if (!f)
			return false;
		fputs(DefaultIniText().c_str(), f);
		fclose(f);
		return true;
	}
}
