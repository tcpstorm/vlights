// menu.cpp - the settings menu, as a separate always-on-top window.
//
// This deliberately does NOT hook the game's renderer. An earlier build
// detoured IDXGISwapChain::Present and subclassed the game window, and
// FiveM's anti-cheat (adhesive) terminated the game about a minute after
// start, every time. So instead the plugin owns a small top-level window
// with its own D3D11 device and swapchain and renders Dear ImGui into that,
// exactly like ImGui's stock Win32 + DirectX 11 example. It floats over the
// (borderless) game window, takes its own input, and touches nothing of the
// game's.
//
// Camera lock: the game reads the mouse through raw input registered for
// the whole process (with RIDEV_INPUTSINK it keeps receiving movement even
// when our window has focus, so the camera would spin while you drag a
// slider). While the menu is open the mouse is unregistered from raw input
// and the game's exact original registration is restored on close. That is
// an OS call on our own process, no game hook involved.
//
// Threads: MenuInit() (worker thread) spawns the UI thread, which owns the
// window, the device, the ImGui context and the message loop. MenuToggle()
// just posts a message to it.
#include "color/recolor.h"
#include "plugin/log.h"
#include "plugin/plugin.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

#include <atomic>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
	// Avoid __uuidof so this builds identically under mingw and MSVC.
	const GUID kIID_ID3D11Texture2D = { 0x6f15aaf2, 0xd208, 0x4e89, { 0x9a, 0xb4, 0x48, 0x95, 0x35, 0xd3, 0x4f, 0x9c } };

	constexpr UINT WM_LODLIGHT_TOGGLE = WM_APP + 1;
	constexpr int kWidth = 470;
	constexpr int kHeight = 600;
	constexpr DWORD kFrameMs = 16; // ~60 fps cap for the menu

	HWND g_hwnd = nullptr;
	std::atomic<bool> g_visible{ false };
	std::atomic<bool> g_ready{ false };
	std::atomic<bool> g_failed{ false };

	ID3D11Device* g_device = nullptr;
	ID3D11DeviceContext* g_context = nullptr;
	IDXGISwapChain* g_swapChain = nullptr;
	ID3D11RenderTargetView* g_rtv = nullptr;
	bool g_needResize = false;
	UINT g_resizeW = 0, g_resizeH = 0;

	// Menu-local copy of the config; pushed to the core on every change.
	lodlight::Config g_menuCfg;
	bool g_menuCfgLoaded = false;
	std::string g_status;

	// ------------------------------------------------------------ camera lock

	std::vector<RAWINPUTDEVICE> g_savedMouse;
	bool g_mouseSuspended = false;

	void SuspendGameMouse()
	{
		if (g_mouseSuspended)
			return;
		UINT n = 0;
		if (GetRegisteredRawInputDevices(nullptr, &n, sizeof(RAWINPUTDEVICE)) != 0 || n == 0)
		{
			lodlight::Log("menu: no raw input devices registered in this process; camera lock not needed");
			return;
		}
		std::vector<RAWINPUTDEVICE> all(n);
		UINT got = GetRegisteredRawInputDevices(all.data(), &n, sizeof(RAWINPUTDEVICE));
		if (got == static_cast<UINT>(-1))
		{
			lodlight::Log("menu: GetRegisteredRawInputDevices failed, error=%lu", GetLastError());
			return;
		}
		g_savedMouse.clear();
		for (UINT i = 0; i < got; ++i)
			if (all[i].usUsagePage == 0x01 && all[i].usUsage == 0x02) // generic desktop / mouse
				g_savedMouse.push_back(all[i]);
		if (g_savedMouse.empty())
		{
			lodlight::Log("menu: the game has no raw mouse registration; camera lock not needed");
			return;
		}
		RAWINPUTDEVICE remove{};
		remove.usUsagePage = 0x01;
		remove.usUsage = 0x02;
		remove.dwFlags = RIDEV_REMOVE;
		remove.hwndTarget = nullptr;
		if (!RegisterRawInputDevices(&remove, 1, sizeof(remove)))
		{
			lodlight::Log("menu: could not unregister raw mouse input, error=%lu", GetLastError());
			return;
		}
		g_mouseSuspended = true;
		lodlight::Log("menu: camera locked (raw mouse input suspended; %u registration(s) saved, flags 0x%lX)",
			(unsigned)g_savedMouse.size(), (unsigned long)g_savedMouse[0].dwFlags);
	}

	void RestoreGameMouse()
	{
		if (!g_mouseSuspended)
			return;
		if (!RegisterRawInputDevices(g_savedMouse.data(), static_cast<UINT>(g_savedMouse.size()), sizeof(RAWINPUTDEVICE)))
			lodlight::Log("menu: could not restore raw mouse input, error=%lu", GetLastError());
		else
			lodlight::Log("menu: camera unlocked (raw mouse input restored)");
		g_mouseSuspended = false;
	}

	// ------------------------------------------------------------ D3D

	template <typename T>
	void SafeRelease(T*& p)
	{
		if (p)
		{
			p->Release();
			p = nullptr;
		}
	}

	bool CreateRenderTarget()
	{
		ID3D11Texture2D* back = nullptr;
		if (FAILED(g_swapChain->GetBuffer(0, kIID_ID3D11Texture2D, reinterpret_cast<void**>(&back))) || !back)
			return false;
		HRESULT hr = g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
		back->Release();
		return SUCCEEDED(hr) && g_rtv != nullptr;
	}

	bool CreateDevice(HWND hwnd)
	{
		DXGI_SWAP_CHAIN_DESC sd{};
		sd.BufferCount = 2;
		sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		sd.BufferDesc.RefreshRate.Numerator = 60;
		sd.BufferDesc.RefreshRate.Denominator = 1;
		sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.OutputWindow = hwnd;
		sd.SampleDesc.Count = 1;
		sd.Windowed = TRUE;
		sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

		D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
		D3D_FEATURE_LEVEL got{};
		HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
			D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &got, &g_context);
		if (FAILED(hr))
		{
			hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
				D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device, &got, &g_context);
		}
		if (FAILED(hr))
		{
			lodlight::Log("menu: D3D11CreateDeviceAndSwapChain failed, hr=0x%08lX", (unsigned long)hr);
			return false;
		}
		return CreateRenderTarget();
	}

	void DestroyDevice()
	{
		SafeRelease(g_rtv);
		SafeRelease(g_swapChain);
		SafeRelease(g_context);
		SafeRelease(g_device);
	}

	// ------------------------------------------------------------ window

	void ShowMenu(HWND hwnd)
	{
		HWND game = FindWindowW(L"grcWindow", nullptr);
		RECT r{};
		if (game && GetWindowRect(game, &r))
			SetWindowPos(hwnd, HWND_TOPMOST, r.left + 40, r.top + 60, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
		ShowWindow(hwnd, SW_SHOWNA);
		SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
		g_menuCfgLoaded = false;
		g_visible = true;
		SuspendGameMouse();
	}

	void HideMenu(HWND hwnd)
	{
		ShowWindow(hwnd, SW_HIDE);
		g_visible = false;
		RestoreGameMouse();
	}

	LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
	{
		if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
			return 1;

		switch (msg)
		{
		case WM_SIZE:
			if (wParam != SIZE_MINIMIZED)
			{
				g_needResize = true;
				g_resizeW = LOWORD(lParam);
				g_resizeH = HIWORD(lParam);
			}
			return 0;
		case WM_SYSCOMMAND:
			if ((wParam & 0xFFF0) == SC_KEYMENU) // no ALT menu
				return 0;
			break;
		case WM_CLOSE:
			HideMenu(hwnd);
			return 0;
		case WM_LODLIGHT_TOGGLE:
			if (g_visible.load())
				HideMenu(hwnd);
			else
				ShowMenu(hwnd);
			return 0;
		case WM_DESTROY:
			RestoreGameMouse();
			PostQuitMessage(0);
			return 0;
		default:
			break;
		}
		return DefWindowProcW(hwnd, msg, wParam, lParam);
	}

	// ------------------------------------------------------------ ImGui content

	void SetTarget(float r, float g, float b)
	{
		g_menuCfg.match.target = lodlight::RGB{ r, g, b };
	}

	void Push()
	{
		lodlight::SetConfig(g_menuCfg);
		lodlight::ReapplyAll();
		g_menuCfg = lodlight::GetConfig(); // pick up derived/clamped fields
	}

	void DrawMenu()
	{
		if (!g_menuCfgLoaded)
		{
			g_menuCfg = lodlight::GetConfig();
			g_menuCfgLoaded = true;
		}
		lodlight::Config& cfg = g_menuCfg;

		const ImGuiViewport* vp = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(vp->WorkPos);
		ImGui::SetNextWindowSize(vp->WorkSize);
		ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
			| ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

		if (ImGui::Begin("LOD Light Recolor", nullptr, flags))
		{
			ImGui::SeparatorText("LOD Light Recolor");
			bool changed = false;

			changed |= ImGui::Checkbox("Enabled", &cfg.match.enabled);

			float target[3] = { cfg.match.target.r / 255.f, cfg.match.target.g / 255.f, cfg.match.target.b / 255.f };
			if (ImGui::ColorEdit3("Target colour", target))
			{
				SetTarget(target[0] * 255.f, target[1] * 255.f, target[2] * 255.f);
				changed = true;
			}

			ImGui::TextDisabled("Presets:");
			ImGui::SameLine();
			if (ImGui::SmallButton("Cool LED"))   { SetTarget(235.f, 240.f, 255.f); changed = true; }
			ImGui::SameLine();
			if (ImGui::SmallButton("Neutral"))    { SetTarget(255.f, 255.f, 255.f); changed = true; }
			ImGui::SameLine();
			if (ImGui::SmallButton("Warm LED"))   { SetTarget(255.f, 244.f, 229.f); changed = true; }
			ImGui::SameLine();
			if (ImGui::SmallButton("Test green")) { SetTarget(0.f, 255.f, 0.f); changed = true; }

			changed |= ImGui::SliderFloat("Blend", &cfg.match.blend, 0.f, 1.f, "%.2f");
			changed |= ImGui::Checkbox("Keep each light's brightness", &cfg.match.keepBrightness);
			changed |= ImGui::Checkbox("Also recolour nearby lamp lights (model lights)", &cfg.nearEnabled);
			if (cfg.nearEnabled && !lodlight::NearAvailable())
			{
				ImGui::SameLine();
				ImGui::TextDisabled("(needs a game restart)");
			}

			ImGui::SeparatorText("What counts as sodium orange");

			float source[3] = { cfg.source.r / 255.f, cfg.source.g / 255.f, cfg.source.b / 255.f };
			if (ImGui::ColorEdit3("Match colour", source))
			{
				cfg.source = lodlight::RGB{ source[0] * 255.f, source[1] * 255.f, source[2] * 255.f };
				changed = true;
			}
			ImGui::SameLine();
			ImGui::TextDisabled("(hue %.0f)", lodlight::ToHSV(cfg.source).h);

			changed |= ImGui::SliderFloat("Hue window", &cfg.match.hueWindow, 0.f, 90.f, "%.0f deg");
			changed |= ImGui::SliderFloat("Min saturation", &cfg.match.minSaturation, 0.f, 1.f, "%.2f");

			if (changed)
				Push();

			ImGui::SeparatorText("Status");
			lodlight::Stats st = lodlight::GetStats();
			ImGui::Text("Loaded objects: %llu", (unsigned long long)st.loadedNow);
			ImGui::Text("Lights now: %llu   recolored: %llu",
				(unsigned long long)st.lastRepaintLights, (unsigned long long)st.lastRepaintRecolored);
			ImGui::TextDisabled("Since start: %llu blocks, %llu LOD lights, %llu recolored",
				(unsigned long long)st.blocksWithLights, (unsigned long long)st.lights, (unsigned long long)st.recolored);
			ImGui::TextDisabled("Models: %llu with %llu lights, %llu recolored at load",
				(unsigned long long)st.nearModels, (unsigned long long)st.nearLights, (unsigned long long)st.nearRecolored);
			ImGui::TextDisabled(g_mouseSuspended ? "Camera locked while this window is open." : "Camera not locked (no raw mouse registration found).");

			ImGui::Separator();
			if (ImGui::Button("Save to ini"))
			{
				std::string err;
				g_status = lodlight::SaveConfigToDisk(err) ? "Saved." : ("Save failed: " + err);
			}
			ImGui::SameLine();
			if (ImGui::Button("Reload ini"))
			{
				if (lodlight::ReloadConfigFromDisk("menu"))
				{
					lodlight::ReapplyAll();
					g_menuCfgLoaded = false;
					g_status = "Reloaded from ini.";
				}
				else
				{
					g_status = "Reload failed (see log).";
				}
			}
			ImGui::SameLine();
			if (ImGui::Button("Close"))
				PostMessageW(g_hwnd, WM_LODLIGHT_TOGGLE, 0, 0);

			if (!g_status.empty())
				ImGui::TextWrapped("%s", g_status.c_str());

			ImGui::TextDisabled("Changes apply instantly to all loaded lights.");
			ImGui::TextDisabled("Press the menu key again, or Close, to hide this window.");
		}
		ImGui::End();
	}

	void RenderFrame()
	{
		if (g_needResize && g_swapChain)
		{
			SafeRelease(g_rtv);
			g_swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
			CreateRenderTarget();
			g_needResize = false;
		}
		if (!g_rtv)
			return;

		ImGui_ImplDX11_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();
		DrawMenu();
		ImGui::Render();

		const float clear[4] = { 0.09f, 0.09f, 0.11f, 1.0f };
		g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
		g_context->ClearRenderTargetView(g_rtv, clear);
		ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
		g_swapChain->Present(0, 0); // no vsync wait: never contend with the game's own swapchain
	}

	DWORD WINAPI UiThread(LPVOID)
	{
		WNDCLASSEXW wc{};
		wc.cbSize = sizeof(wc);
		wc.style = CS_CLASSDC;
		wc.lpfnWndProc = WndProc;
		wc.hInstance = GetModuleHandleW(nullptr);
		wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
		wc.lpszClassName = L"LodLightRecolorMenu";
		if (!RegisterClassExW(&wc))
		{
			lodlight::Log("menu: RegisterClassEx failed, error=%lu", GetLastError());
			g_failed = true;
			return 0;
		}

		g_hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"LOD Light Recolor",
			WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME, 100, 100, kWidth, kHeight,
			nullptr, nullptr, wc.hInstance, nullptr);
		if (!g_hwnd)
		{
			lodlight::Log("menu: CreateWindowEx failed, error=%lu", GetLastError());
			g_failed = true;
			return 0;
		}

		if (!CreateDevice(g_hwnd))
		{
			DestroyDevice();
			DestroyWindow(g_hwnd);
			g_hwnd = nullptr;
			g_failed = true;
			return 0;
		}

		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO();
		io.IniFilename = nullptr;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		ImGui::StyleColorsDark();
		ImGui::GetStyle().WindowRounding = 0.f;
		ImGui::GetStyle().FrameRounding = 4.f;

		if (!ImGui_ImplWin32_Init(g_hwnd) || !ImGui_ImplDX11_Init(g_device, g_context))
		{
			lodlight::Log("menu: ImGui backend init failed");
			g_failed = true;
			return 0;
		}

		g_ready = true;
		lodlight::Log("menu: window ready (own D3D11 device, no game hooks)");

		MSG msg{};
		for (;;)
		{
			bool quit = false;
			while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
			{
				if (msg.message == WM_QUIT)
					quit = true;
				TranslateMessage(&msg);
				DispatchMessageW(&msg);
			}
			if (quit)
				break;

			if (g_visible.load())
			{
				RenderFrame();
				Sleep(kFrameMs);
			}
			else
			{
				WaitMessage(); // idle until toggled
			}
		}

		ImGui_ImplDX11_Shutdown();
		ImGui_ImplWin32_Shutdown();
		ImGui::DestroyContext();
		DestroyDevice();
		return 0;
	}
}

namespace lodlight
{
	bool MenuInit()
	{
		HANDLE h = CreateThread(nullptr, 0, UiThread, nullptr, 0, nullptr);
		if (!h)
		{
			Log("menu: could not start UI thread, error=%lu", GetLastError());
			return false;
		}
		CloseHandle(h);
		return true;
	}

	void MenuToggle()
	{
		if (!g_ready.load() || g_failed.load() || !g_hwnd)
			return;
		PostMessageW(g_hwnd, WM_LODLIGHT_TOGGLE, 0, 0);
	}

	bool MenuVisible()
	{
		return g_visible.load();
	}
}
