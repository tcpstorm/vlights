// main.cpp - DllMain, startup, and the hotkey thread.
//
// Load order in FiveM: the asi-five component LoadLibrary()s every .asi in
// FiveM.app/plugins/ from its DoGameLoad step, on the main thread, unless
// the server's sv_pureLevel is >= 2 (code/components/asi-five/src/Component.cpp).
// That is after the game image is mapped and before the world streams in.
//
// The map hook is installed synchronously inside DllMain, on purpose. The
// main thread is the only caller of fwMapDataStore::FinishLoading, and while
// we are in DllMain it is blocked inside LoadLibrary, so the target cannot
// be executing while its first bytes are overwritten. Every other detour
// (store Remove, model load-complete) is installed from inside that hook,
// i.e. from the main thread that is their only caller. That is why MinHook
// never needs to suspend threads here (see third_party/minhook/src/hook.c;
// FiveM stubs out the Toolhelp snapshot anyway).
#include "game/lod_lights.h"
#include "game/near_lights.h"
#include "game/textures.h"
#include "game/track.h"
#include "hook/pattern.h"
#include "plugin/config.h"
#include "plugin/log.h"
#include "plugin/plugin.h"
#include "plugin/update.h"

#include <windows.h>

#include <string>

namespace
{
	HMODULE g_module = nullptr;

	std::wstring PluginDir()
	{
		wchar_t buf[MAX_PATH];
		DWORD n = GetModuleFileNameW(g_module, buf, MAX_PATH);
		std::wstring path(buf, n);
		size_t slash = path.find_last_of(L"\\/");
		return (slash == std::wstring::npos) ? L"." : path.substr(0, slash);
	}

	DWORD WINAPI WorkerThread(LPVOID)
	{
		if (vlights::GetConfig().menuKey > 0)
		{
			if (!vlights::MenuInit())
				vlights::Log("menu unavailable; ini + reload key still work");
		}
		else
		{
			vlights::Log("menu disabled (menu_key = 0)");
		}

		if (vlights::GetConfig().updateCheck)
			vlights::update::Start();

		bool reloadWasDown = false;
		bool menuWasDown = false;
		for (;;)
		{
			Sleep(100);
			vlights::textures::Tick();
			const vlights::Config cfg = vlights::GetConfig();

			if (cfg.reloadKey > 0)
			{
				bool down = (GetAsyncKeyState(cfg.reloadKey) & 0x8000) != 0;
				if (down && !reloadWasDown)
				{
					vlights::Stats st = vlights::GetStats();
					vlights::Log("stats: calls=%llu blocks_with_lights=%llu lights=%llu recolored=%llu near_models=%llu near_lights=%llu near_recolored=%llu loaded_now=%llu",
						(unsigned long long)st.calls, (unsigned long long)st.blocksWithLights, (unsigned long long)st.lights,
						(unsigned long long)st.recolored, (unsigned long long)st.nearModels, (unsigned long long)st.nearLights,
						(unsigned long long)st.nearRecolored, (unsigned long long)st.loadedNow);
					uint64_t rmCalls = 0, rmDropped = 0;
					vlights::track::RemoveStats(rmCalls, rmDropped);
					vlights::Log("unload detour: fired %llu times, dropped %llu tracked slots", (unsigned long long)rmCalls, (unsigned long long)rmDropped);
					vlights::Log("unmatched warm light colours so far:%s", vlights::nearlights::UnmatchedWarmSummary().c_str());
					vlights::Log("textures: %llu resources placed, %llu with selected textures, %llu textures recoloured (%llu blocks); %llu registered for live repaint", (unsigned long long)vlights::textures::Placements(), (unsigned long long)vlights::textures::Dictionaries(), (unsigned long long)vlights::textures::TexturesRecoloured(), (unsigned long long)vlights::textures::BlocksRecoloured(), (unsigned long long)vlights::textures::Registered());
					if (vlights::ReloadConfigFromDisk("hotkey"))
						vlights::ReapplyAll();
				}
				reloadWasDown = down;
			}

			if (cfg.menuKey > 0)
			{
				bool down = (GetAsyncKeyState(cfg.menuKey) & 0x8000) != 0;
				if (down && !menuWasDown)
					vlights::MenuToggle();
				menuWasDown = down;
			}
		}
	}

	void Startup()
	{
		const std::wstring dir = PluginDir();
		vlights::SetConfigPath(dir + L"\\vlights.ini");
		vlights::LogInit(dir + L"\\vlights.log");
		vlights::Log("VLights " VLIGHTS_VERSION " starting (thread 0x%lX, game build %d, streaming vtable shift %d)", GetCurrentThreadId(), vlights::GameBuild(), vlights::StreamingVtableShift());

		if (!vlights::EnsureDefaultConfig(vlights::ConfigPath()))
			vlights::Log("could not write default config to %ls", vlights::ConfigPath().c_str());
		vlights::ReloadConfigFromDisk("startup");

		if (!vlights::lodlights::Install())
		{
			vlights::Log("inactive");
			return;
		}

		HANDLE h = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
		if (h)
			CloseHandle(h);
		else
			vlights::Log("could not start worker thread; hotkeys and menu disabled");
	}
}

extern "C" BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
	{
		g_module = instance;
		DisableThreadLibraryCalls(instance);
		Startup();
	}
	return TRUE;
}
