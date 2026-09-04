// update.cpp - see update.h.
#include "plugin/update.h"
#include "plugin/log.h"

#include <vlights/version.h>

#include <windows.h>
#include <winhttp.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <string>

namespace vlights::update
{
	namespace
	{
		std::atomic<bool> g_started{ false };
		std::atomic<int> g_state{ 0 }; // 0 idle, 1 checking, 2 up to date, 3 update available, 4 failed
		SRWLOCK g_lock = SRWLOCK_INIT;
		std::string g_latest;
		std::string g_url;
		std::string g_why;

		// "https://github.com/owner/repo" -> owner/repo
		bool RepoFromUrl(std::string& ownerRepo)
		{
			std::string u = VLIGHTS_URL;
			const char* host = "github.com/";
			size_t p = u.find(host);
			if (p == std::string::npos)
				return false;
			ownerRepo = u.substr(p + strlen(host));
			while (!ownerRepo.empty() && (ownerRepo.back() == '/' || ownerRepo.back() == ' '))
				ownerRepo.pop_back();
			if (ownerRepo.size() > 4 && ownerRepo.compare(ownerRepo.size() - 4, 4, ".git") == 0)
				ownerRepo.resize(ownerRepo.size() - 4);
			return ownerRepo.find('/') != std::string::npos;
		}

		bool ParseVersion(const std::string& s, int out[3])
		{
			const char* p = s.c_str();
			if (*p == 'v' || *p == 'V')
				++p;
			char* end = nullptr;
			for (int i = 0; i < 3; ++i)
			{
				long v = strtol(p, &end, 10);
				if (end == p || v < 0)
					return false;
				out[i] = static_cast<int>(v);
				p = end;
				if (i < 2)
				{
					if (*p != '.')
						return false;
					++p;
				}
			}
			return true;
		}

		bool Newer(const int a[3], const int b[3]) // a > b
		{
			for (int i = 0; i < 3; ++i)
			{
				if (a[i] != b[i])
					return a[i] > b[i];
			}
			return false;
		}

		// One GET, response body into `out`. WinHTTP only; no redirects needed
		// for the API.
		bool HttpsGet(const std::wstring& host, const std::wstring& path, std::string& out, std::string& why)
		{
			HINTERNET session = WinHttpOpen(L"VLights/" VLIGHTS_VERSION_W, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
			if (!session)
			{
				why = "WinHttpOpen failed";
				return false;
			}
			WinHttpSetTimeouts(session, 5000, 5000, 5000, 5000);
			bool ok = false;
			HINTERNET conn = WinHttpConnect(session, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
			HINTERNET req = conn ? WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
			if (!req)
			{
				why = "could not open the request";
			}
			else
			{
				const wchar_t* headers = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
				if (!WinHttpSendRequest(req, headers, static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(req, nullptr))
				{
					why = "no response (offline, or blocked)";
				}
				else
				{
					DWORD status = 0, len = sizeof(status);
					WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
					if (status != 200)
					{
						char buf[64];
						snprintf(buf, sizeof(buf), "HTTP %lu", status);
						why = buf;
					}
					else
					{
						for (;;)
						{
							DWORD avail = 0;
							if (!WinHttpQueryDataAvailable(req, &avail) || avail == 0)
								break;
							std::string chunk(avail, '\0');
							DWORD got = 0;
							if (!WinHttpReadData(req, chunk.data(), avail, &got) || got == 0)
								break;
							out.append(chunk.data(), got);
							if (out.size() > (1u << 20))
								break;
						}
						ok = true;
					}
				}
			}
			if (req) WinHttpCloseHandle(req);
			if (conn) WinHttpCloseHandle(conn);
			WinHttpCloseHandle(session);
			return ok;
		}

		// "tag_name":"v1.2.3" -> v1.2.3 (the API's JSON is flat enough for this)
		bool JsonString(const std::string& json, const char* key, std::string& out)
		{
			std::string k = std::string("\"") + key + "\"";
			size_t p = json.find(k);
			if (p == std::string::npos)
				return false;
			p = json.find(':', p + k.size());
			if (p == std::string::npos)
				return false;
			p = json.find('"', p);
			if (p == std::string::npos)
				return false;
			size_t e = json.find('"', p + 1);
			if (e == std::string::npos)
				return false;
			out = json.substr(p + 1, e - p - 1);
			return true;
		}

		void SetResult(int state, const std::string& latest, const std::string& url, const std::string& why)
		{
			AcquireSRWLockExclusive(&g_lock);
			g_latest = latest;
			g_url = url;
			g_why = why;
			ReleaseSRWLockExclusive(&g_lock);
			g_state = state;
		}

		DWORD WINAPI CheckThread(LPVOID)
		{
			Sleep(8000); // let the game finish its own startup traffic first
			std::string ownerRepo;
			if (!RepoFromUrl(ownerRepo))
			{
				SetResult(4, "", "", "no GitHub repository in the build's URL");
				return 0;
			}
			std::wstring path = L"/repos/" + std::wstring(ownerRepo.begin(), ownerRepo.end()) + L"/releases/latest";
			std::string body, why;
			if (!HttpsGet(L"api.github.com", path, body, why))
			{
				SetResult(4, "", "", why);
				Log("update check: %s", why.c_str());
				return 0;
			}
			std::string tag, url;
			if (!JsonString(body, "tag_name", tag))
			{
				SetResult(4, "", "", "no release found");
				return 0;
			}
			JsonString(body, "html_url", url);
			int latest[3], mine[3];
			if (!ParseVersion(tag, latest) || !ParseVersion(VLIGHTS_VERSION, mine))
			{
				SetResult(4, "", "", "unrecognised version tag " + tag);
				return 0;
			}
			const std::string latestStr = tag[0] == 'v' || tag[0] == 'V' ? tag.substr(1) : tag;
			if (Newer(latest, mine))
			{
				SetResult(3, latestStr, url.empty() ? std::string(VLIGHTS_URL) + "/releases" : url, "");
				Log("update check: %s available (running %s): %s", latestStr.c_str(), VLIGHTS_VERSION, url.c_str());
			}
			else
			{
				SetResult(2, latestStr, "", "");
				Log("update check: up to date (latest release %s)", latestStr.c_str());
			}
			return 0;
		}
	}

	void Start()
	{
		if (g_started.exchange(true))
			return;
		g_state = 1;
		HANDLE h = CreateThread(nullptr, 0, CheckThread, nullptr, 0, nullptr);
		if (h)
			CloseHandle(h);
		else
			SetResult(4, "", "", "could not start the check thread");
	}

	bool Available(std::string& latest, std::string& url)
	{
		if (g_state != 3)
			return false;
		AcquireSRWLockShared(&g_lock);
		latest = g_latest;
		url = g_url;
		ReleaseSRWLockShared(&g_lock);
		return true;
	}

	const char* Status()
	{
		switch (g_state.load())
		{
		case 1: return "checking for updates";
		case 2: return "up to date";
		case 3: return "update available";
		case 4:
		{
			static char buf[128];
			AcquireSRWLockShared(&g_lock);
			snprintf(buf, sizeof(buf), "update check skipped: %s", g_why.c_str());
			ReleaseSRWLockShared(&g_lock);
			return buf;
		}
		default: return "update check off";
		}
	}
}
