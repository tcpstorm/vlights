// Host-native tests for Recolor.h. Built and run inside the Docker build
// before the .asi is cross-compiled, so a colour-math regression fails the
// build.
#include "Recolor.h"

#include <cmath>
#include <cstdio>

static int g_failures = 0;

#define CHECK(cond)                                                     \
	do {                                                                \
		if (!(cond)) {                                                  \
			std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			++g_failures;                                               \
		}                                                               \
	} while (0)

static bool Near(float a, float b, float eps = 0.6f)
{
	return std::fabs(a - b) <= eps;
}

int main()
{
	using namespace lodlight;

	// packing: 0xIIRRGGBB, top byte preserved
	{
		RGB c = Unpack(0x80FF9329u);
		CHECK(c.r == 255.f && c.g == 147.f && c.b == 41.f);
		CHECK(Pack(c, 0x80FF9329u) == 0x80FF9329u);
		CHECK(Pack(RGB{ 235.f, 240.f, 255.f }, 0x80FF9329u) == 0x80EBF0FFu);
		CHECK(Pack(RGB{ -5.f, 300.f, 127.5f }, 0) == 0x0000FF80u); // clamp + round
	}

	// HSV
	{
		HSV h = ToHSV(RGB{ 255.f, 147.f, 41.f });
		CHECK(Near(h.h, 29.7f));
		CHECK(Near(h.s, 0.84f, 0.01f));
		CHECK(Near(h.v, 1.0f, 0.001f));

		HSV w = ToHSV(RGB{ 255.f, 255.f, 255.f });
		CHECK(w.s == 0.f && w.v == 1.f);

		HSV k = ToHSV(RGB{ 0.f, 0.f, 0.f });
		CHECK(k.s == 0.f && k.v == 0.f);

		CHECK(Near(ToHSV(RGB{ 255.f, 0.f, 0.f }).h, 0.f));
		CHECK(Near(ToHSV(RGB{ 255.f, 255.f, 0.f }).h, 60.f));
		CHECK(Near(ToHSV(RGB{ 0.f, 0.f, 255.f }).h, 240.f));
		CHECK(Near(ToHSV(RGB{ 255.f, 0.f, 128.f }).h, 330.f));
	}

	// hue distance wraps
	{
		CHECK(Near(HueDistance(350.f, 10.f), 20.f, 0.01f));
		CHECK(Near(HueDistance(10.f, 350.f), 20.f, 0.01f));
		CHECK(Near(HueDistance(0.f, 180.f), 180.f, 0.01f));
	}

	// matching: sodium yes, white/red/yellow/blue/dark no, dim orange yes
	{
		MatchParams p;
		CHECK(Matches(RGB{ 255.f, 147.f, 41.f }, p));
		CHECK(Matches(RGB{ 128.f, 74.f, 20.f }, p));    // dim sodium, hue 30
		CHECK(Matches(RGB{ 255.f, 120.f, 60.f }, p));   // warmer, hue ~18
		CHECK(!Matches(RGB{ 255.f, 255.f, 255.f }, p)); // white
		CHECK(!Matches(RGB{ 235.f, 240.f, 255.f }, p)); // our own target
		CHECK(!Matches(RGB{ 255.f, 0.f, 0.f }, p));     // red, hue 0
		CHECK(!Matches(RGB{ 255.f, 255.f, 0.f }, p));   // yellow, hue 60
		CHECK(!Matches(RGB{ 0.f, 0.f, 255.f }, p));     // blue
		CHECK(!Matches(RGB{ 5.f, 3.f, 1.f }, p));       // basically black
		CHECK(!Matches(RGB{ 255.f, 220.f, 190.f }, p)); // warm white, low sat

		p.enabled = false;
		uint32_t v = 0x80FF9329u;
		CHECK(!Recolor(v, p));
		CHECK(v == 0x80FF9329u);
	}

	// recolor: hard replace, brightness not kept
	{
		MatchParams p;
		p.keepBrightness = false;
		uint32_t v = 0x80FF9329u;
		CHECK(Recolor(v, p));
		CHECK(v == 0x80EBF0FFu);

		uint32_t white = 0x80FFFFFFu;
		CHECK(!Recolor(white, p));
		CHECK(white == 0x80FFFFFFu);
	}

	// recolor: keep brightness scales target by the light's V
	{
		MatchParams p; // keepBrightness = true
		uint32_t v = 0x00804A14u; // (128,74,20), V = 128/255
		CHECK(Recolor(v, p));
		RGB out = Unpack(v);
		// target (235,240,255) * (128/255) = (118,120,128)
		CHECK(Near(out.r, 118.f) && Near(out.g, 120.f) && Near(out.b, 128.f));

		uint32_t bright = 0x00FF9329u; // V = 1 -> unscaled target
		CHECK(Recolor(bright, p));
		CHECK(bright == 0x00EBF0FFu);
	}

	// blend
	{
		MatchParams p;
		p.keepBrightness = false;
		p.blend = 0.5f;
		uint32_t v = 0x00FF9329u;
		CHECK(Recolor(v, p));
		RGB out = Unpack(v);
		CHECK(Near(out.r, 245.f) && Near(out.g, 194.f) && Near(out.b, 148.f));

		p.blend = 0.f;
		uint32_t u = 0x00FF9329u;
		CHECK(!Recolor(u, p)); // no-op is reported as unchanged
		CHECK(u == 0x00FF9329u);
	}

	if (g_failures == 0)
		std::printf("all recolor tests passed\n");
	return g_failures == 0 ? 0 : 1;
}
