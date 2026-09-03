// Recolor.h - pure colour math for the LOD light recolor pass.
//
// No Windows headers, no game structs: this file is compiled into the
// host-native unit tests (tests/test_recolor.cpp) as well as the plugin.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace lodlight
{
	struct RGB
	{
		float r, g, b; // 0..255
	};

	struct HSV
	{
		float h; // degrees, [0, 360)
		float s; // [0, 1]
		float v; // [0, 1]
	};

	// --- packing -----------------------------------------------------------
	//
	// CDistantLODLight::rgbi entries are 0xIIRRGGBB: intensity in the top
	// byte, then R, G, B. Confirmed in-game: vanilla sodium street lights
	// decode as e.g. 0xAAFF780A = (255,120,10), intensity 170.
	inline RGB Unpack(uint32_t packed)
	{
		return RGB{
			static_cast<float>((packed >> 16) & 0xFF),
			static_cast<float>((packed >> 8) & 0xFF),
			static_cast<float>(packed & 0xFF),
		};
	}

	inline uint32_t Clamp8(float v)
	{
		long i = std::lround(v);
		return static_cast<uint32_t>(std::clamp<long>(i, 0, 255));
	}

	// Keeps the original intensity byte.
	inline uint32_t Pack(const RGB& c, uint32_t original)
	{
		return (original & 0xFF000000u) | (Clamp8(c.r) << 16) | (Clamp8(c.g) << 8) | Clamp8(c.b);
	}

	// --- colour space --------------------------------------------------------
	inline HSV ToHSV(const RGB& c)
	{
		float r = c.r / 255.f, g = c.g / 255.f, b = c.b / 255.f;
		float mx = std::max({ r, g, b });
		float mn = std::min({ r, g, b });
		float d = mx - mn;

		HSV out{ 0.f, 0.f, mx };
		out.s = (mx > 0.f) ? (d / mx) : 0.f;

		if (d > 0.f)
		{
			if (mx == r)
				out.h = 60.f * ((g - b) / d);
			else if (mx == g)
				out.h = 60.f * ((b - r) / d + 2.f);
			else
				out.h = 60.f * ((r - g) / d + 4.f);

			if (out.h < 0.f)
				out.h += 360.f;
		}
		return out;
	}

	inline float HueDistance(float a, float b)
	{
		float d = std::fabs(std::fmod(a - b, 360.f));
		return std::min(d, 360.f - d);
	}

	inline RGB Lerp(const RGB& a, const RGB& b, float t)
	{
		return RGB{
			a.r + (b.r - a.r) * t,
			a.g + (b.g - a.g) * t,
			a.b + (b.b - a.b) * t,
		};
	}

	// --- matching + recolor ----------------------------------------------------
	//
	// Matching is done in hue/saturation so it is brightness-invariant: a dim
	// sodium light and a bright one both match, while yellow (hue 60), red
	// (hue 0) and anything desaturated (white/grey) do not.
	struct MatchParams
	{
		bool enabled = true;
		float sourceHue = 29.7f;     // hue of the sodium reference (255,147,41)
		float hueWindow = 13.f;      // degrees either side of sourceHue; vanilla sodium spans hue 17-30, amber runway lights sit at 44
		float minSaturation = 0.6f;  // vanilla sodium is 0.75-1.0; warm signage sits at 0.35-0.6
		float minValue = 0.05f;      // below this it is black, leave alone
		RGB target{ 235.f, 240.f, 255.f };
		float blend = 1.f;           // 1 = replace, 0 = untouched
		bool keepBrightness = true;  // scale target to the light's original V
	};

	inline bool Matches(const RGB& c, const MatchParams& p, HSV* outHsv = nullptr)
	{
		HSV hsv = ToHSV(c);
		if (outHsv)
			*outHsv = hsv;
		if (hsv.v < p.minValue)
			return false;
		if (hsv.s < p.minSaturation)
			return false;
		return HueDistance(hsv.h, p.sourceHue) <= p.hueWindow;
	}

	// Returns the recoloured value for a matching light, or the input unchanged.
	inline RGB Apply(const RGB& c, const HSV& hsv, const MatchParams& p)
	{
		RGB target = p.target;
		if (p.keepBrightness)
		{
			float targetV = std::max({ target.r, target.g, target.b }) / 255.f;
			if (targetV > 0.f)
			{
				float k = hsv.v / targetV;
				target = RGB{ target.r * k, target.g * k, target.b * k };
			}
		}
		return Lerp(c, target, std::clamp(p.blend, 0.f, 1.f));
	}

	// In-place on a packed rgbi entry. Returns true if it was changed.
	inline bool Recolor(uint32_t& packed, const MatchParams& p)
	{
		if (!p.enabled)
			return false;

		RGB c = Unpack(packed);
		HSV hsv;
		if (!Matches(c, p, &hsv))
			return false;

		uint32_t out = Pack(Apply(c, hsv, p), packed);
		if (out == packed)
			return false;
		packed = out;
		return true;
	}
}
