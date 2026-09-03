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
#include "plugin/config.h"
#include "plugin/log.h"
#include "plugin/plugin.h"

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
		if (lodlight::GetConfig().menuKey > 0)
		{
			if (!lodlight::MenuInit())
				lodlight::Log("menu unavailable; ini + reload key still work");
		}
		else
		{
			lodlight::Log("menu disabled (menu_key = 0)");
		}

		bool reloadWasDown = false;
		bool menuWasDown = false;
		for (;;)
		{
			Sleep(100);
			const lodlight::Config cfg = lodlight::GetConfig();

			if (cfg.reloadKey > 0)
			{
				bool down = (GetAsyncKeyState(cfg.reloadKey) & 0x8000) != 0;
				if (down && !reloadWasDown)
				{
					lodlight::Stats st = lodlight::GetStats();
					lodlight::Log("stats: calls=%llu blocks_with_lights=%llu lights=%llu recolored=%llu near_models=%llu near_lights=%llu near_recolored=%llu loaded_now=%llu",
						(unsigned long long)st.calls, (unsigned long long)st.blocksWithLights, (unsigned long long)st.lights,
						(unsigned long long)st.recolored, (unsigned long long)st.nearModels, (unsigned long long)st.nearLights,
						(unsigned long long)st.nearRecolored, (unsigned long long)st.loadedNow);
					lodlight::Log("unmatched warm light colours so far:%s", lodlight::nearlights::UnmatchedWarmSummary().c_str());
					if (lodlight::ReloadConfigFromDisk("hotkey"))
						lodlight::ReapplyAll();
				}
				reloadWasDown = down;
			}

			if (cfg.menuKey > 0)
			{
				bool down = (GetAsyncKeyState(cfg.menuKey) & 0x8000) != 0;
				if (down && !menuWasDown)
					lodlight::MenuToggle();
				menuWasDown = down;
			}
		}
	}

	void Startup()
	{
		const std::wstring dir = PluginDir();
		lodlight::SetConfigPath(dir + L"\\lodlight_recolor.ini");
		lodlight::LogInit(dir + L"\\lodlight_recolor.log");
		lodlight::Log("LodLightRecolor " LODLIGHT_VERSION " starting (thread 0x%lX)", GetCurrentThreadId());

		if (!lodlight::EnsureDefaultConfig(lodlight::ConfigPath()))
			lodlight::Log("could not write default config to %ls", lodlight::ConfigPath().c_str());
		lodlight::ReloadConfigFromDisk("startup");

		if (!lodlight::lodlights::Install())
		{
			lodlight::Log("inactive");
			return;
		}

		HANDLE h = CreateThread(nullptr, 0, WorkerThread, nullptr, 0, nullptr);
		if (h)
			CloseHandle(h);
		else
			lodlight::Log("could not start worker thread; hotkeys and menu disabled");
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
