// timing.h - how much time the hooks take on the game's threads. Two
// QueryPerformanceCounter reads per hook call; the totals go on the F9
// stats line so "near zero impact" is a number, not a claim.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>

namespace vlights::timing
{
	enum Hook : int
	{
		MapBlock = 0,     // ymap FinishLoading: LOD arrays + entity light overrides (main thread)
		Model = 1,        // model load-complete: light attributes (main thread)
		Placement = 2,    // PlaceResource: texture pixels (streaming thread)
		Repaint = 3,      // live repaint of lights (worker / menu thread)
		HookCount = 4
	};

	struct Counter
	{
		std::atomic<uint64_t> calls{ 0 };
		std::atomic<uint64_t> ticks{ 0 };
		std::atomic<uint64_t> maxTicks{ 0 };
	};

	inline Counter& Get(Hook h)
	{
		static Counter counters[HookCount];
		return counters[h];
	}

	inline uint64_t Frequency()
	{
		static uint64_t f = [] { LARGE_INTEGER li; QueryPerformanceFrequency(&li); return static_cast<uint64_t>(li.QuadPart); }();
		return f;
	}

	inline uint64_t Now()
	{
		LARGE_INTEGER li;
		QueryPerformanceCounter(&li);
		return static_cast<uint64_t>(li.QuadPart);
	}

	class Scope
	{
	public:
		explicit Scope(Hook h) : m_h(h), m_start(Now()) {}
		~Scope()
		{
			const uint64_t d = Now() - m_start;
			Counter& c = Get(m_h);
			c.calls.fetch_add(1, std::memory_order_relaxed);
			c.ticks.fetch_add(d, std::memory_order_relaxed);
			uint64_t prev = c.maxTicks.load(std::memory_order_relaxed);
			while (d > prev && !c.maxTicks.compare_exchange_weak(prev, d, std::memory_order_relaxed)) {}
		}
	private:
		Hook m_h;
		uint64_t m_start;
	};

	inline double Ms(uint64_t ticks) { return ticks * 1000.0 / static_cast<double>(Frequency()); }
	inline double Us(uint64_t ticks) { return ticks * 1000000.0 / static_cast<double>(Frequency()); }

	inline const char* Name(Hook h)
	{
		switch (h)
		{
		case MapBlock: return "map blocks";
		case Model: return "models";
		case Placement: return "placement";
		case Repaint: return "repaint";
		default: return "?";
		}
	}
}
