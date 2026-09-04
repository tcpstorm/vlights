// textures.cpp - see textures.h.
//
// Raw resource layout (RSC7, as the game hands it to PlaceResource):
//   block map (rage::datResourceMap, read off a live one on b3751):
//   u8 virtualCount, u8 physicalCount, pad; +8 pointer to the root object
//   (= destination of chunk 0); +16 up to 128 chunks of
//   { u64 encodedSrc; void* dst; u64 size }. Virtual chunks first.
//   Pointers inside the data are still the file's encoded form:
//   0x5XXXXXXX = virtual offset, 0x6XXXXXXX = physical offset, resolved by
//   finding the chunk whose encodedSrc covers the value. Placement is in
//   place: a chunk's dst is where its objects live afterwards.
//
//   pgDictionary<T>: entries atArray at +0x30 (ptr, u16 count).
//   grcTexture (Cfx grcTexture.h): name ptr +0x28, ID3D11Resource* +0x38,
//   width u16 +0x50, height +0x52, stride +0x56, format u32 +0x58 (FourCC
//   such as 'DXT1' in files; DXGI numbers once placed), mip levels u8 +0x5D,
//   pixel data ptr +0x70 (physical), ID3D11ShaderResourceView* +0x78.
//   gtaDrawable: grmShaderGroup ptr +0x10 -> embedded pgDictionary<grcTexture>
//   ptr at +0x08. gtaFragType: drawable ptr +0x30, extra drawables array
//   +0x38 with count at +0x48.
#include "game/textures.h"
#include "color/recolor.h"
#include "plugin/config.h"
#include "plugin/log.h"
#include "plugin/plugin.h"
#include "plugin/timing.h"

#include <windows.h>
#include <d3d11.h>
#include <MinHook.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

namespace vlights::textures
{
	namespace
	{
		using PlaceFn = void* (*)(void* store, uint32_t object, void* blockMap, const char* name);

		constexpr size_t kTexName = 0x28;
		constexpr size_t kTexResource = 0x38;
		constexpr size_t kTexWidth = 0x50;
		constexpr size_t kTexHeight = 0x52;
		constexpr size_t kTexFormat = 0x58;
		constexpr size_t kTexLevels = 0x5D;
		constexpr size_t kTexData = 0x70;
		constexpr size_t kTexSrv = 0x78;
		constexpr size_t kMaxOriginalBytes = 96u << 20; // cap on kept original pixels

		struct Hook
		{
			const char* name;
			void* store = nullptr;
			void* target = nullptr; // game routine actually detoured
			PlaceFn orig = nullptr;
			bool active = false;
		};
		Hook g_hooks[StoreKindCount] = { { "txd" }, { "ydr" }, { "yft" }, { "ydd" } };

		std::atomic<uint64_t> g_placements{ 0 };
		std::atomic<uint64_t> g_dictionaries{ 0 };
		std::atomic<uint64_t> g_texturesRecoloured{ 0 };
		std::atomic<uint64_t> g_blocksRecoloured{ 0 };
		std::atomic<int> g_dictsLogged{ 0 };

		// ------------------------------------------------------------ raw resource

		struct Block
		{
			uint64_t encodedBase;
			const uint8_t* data;
			uint64_t size;
		};
		static_assert(sizeof(Block) == 24, "Block");

		struct Map
		{
			const Block* blocks = nullptr;
			int nVirtual = 0;
			int nPhysical = 0;
		};

		bool ProbeReadable(const void* p, size_t n)
		{
			if (!p || n == 0)
				return false;
			uint8_t tmp;
			SIZE_T got = 0;
			if (!ReadProcessMemory(GetCurrentProcess(), p, &tmp, 1, &got) || got != 1)
				return false;
			if (n > 1 && (!ReadProcessMemory(GetCurrentProcess(), static_cast<const uint8_t*>(p) + n - 1, &tmp, 1, &got) || got != 1))
				return false;
			return true;
		}

		bool ReadMap(const void* blockMap, Map& out)
		{
			if (!blockMap || !ProbeReadable(blockMap, 16 + 8 * sizeof(Block)))
				return false;
			const uint8_t* m = static_cast<const uint8_t*>(blockMap);
			out.nVirtual = m[0];
			out.nPhysical = m[1];
			out.blocks = reinterpret_cast<const Block*>(m + 16);
			if (out.nVirtual < 1 || out.nVirtual + out.nPhysical > 128)
				return false;
			if (!ProbeReadable(m + 16, static_cast<size_t>(out.nVirtual + out.nPhysical) * sizeof(Block)))
				return false;
			for (int i = 0; i < out.nVirtual + out.nPhysical; ++i)
			{
				const Block& b = out.blocks[i];
				if ((b.encodedBase >> 32) != 0)
					return false;
				const uint32_t kind = static_cast<uint32_t>(b.encodedBase) >> 28;
				if ((i < out.nVirtual && kind != 5) || (i >= out.nVirtual && kind != 6))
					return false;
				if (!b.data || b.size == 0 || b.size > (256u << 20) || !ProbeReadable(b.data, static_cast<size_t>(b.size)))
					return false;
			}
			return true;
		}

		// Resolve an encoded pointer to real memory. `avail` = bytes to the end
		// of its chunk.
		uint8_t* Resolve(const Map& map, uint64_t encoded, size_t& avail)
		{
			avail = 0;
			if (encoded == 0 || (encoded >> 32) != 0)
				return nullptr;
			const uint32_t v = static_cast<uint32_t>(encoded);
			const uint32_t kind = v >> 28;
			if (kind != 5 && kind != 6)
				return nullptr;
			for (int i = 0; i < map.nVirtual + map.nPhysical; ++i)
			{
				const Block& b = map.blocks[i];
				const uint64_t base = b.encodedBase;
				if (v >= base && v < base + b.size)
				{
					avail = static_cast<size_t>(base + b.size - v);
					return const_cast<uint8_t*>(b.data) + (v - base);
				}
			}
			return nullptr;
		}

		template <typename T>
		bool ReadAt(const Map& map, uint64_t encoded, size_t off, T& out)
		{
			size_t avail = 0;
			const uint8_t* p = Resolve(map, encoded, avail);
			if (!p || avail < off + sizeof(T))
				return false;
			std::memcpy(&out, p + off, sizeof(T));
			return true;
		}

		std::string ReadName(const Map& map, uint64_t encoded)
		{
			size_t avail = 0;
			const uint8_t* p = Resolve(map, encoded, avail);
			if (!p)
				return "?";
			std::string s;
			for (size_t i = 0; i < avail && i < 96 && p[i]; ++i)
				s += static_cast<char>(p[i]);
			return s;
		}

		// ------------------------------------------------------------ formats

		enum class Fmt { Unknown, DXT1, DXT3, DXT5, BGRA8, RGBA8 };

		Fmt Classify(uint32_t f, std::string& name)
		{
			char buf[32];
			const bool fourcc = (f >> 24) >= 0x20 && ((f >> 16) & 0xFF) >= 0x20 && ((f >> 8) & 0xFF) >= 0x20 && (f & 0xFF) >= 0x20;
			if (fourcc)
			{
				snprintf(buf, sizeof(buf), "%c%c%c%c", f & 0xFF, (f >> 8) & 0xFF, (f >> 16) & 0xFF, f >> 24);
				name = buf;
				if (name == "DXT1") return Fmt::DXT1;
				if (name == "DXT3") return Fmt::DXT3;
				if (name == "DXT5") return Fmt::DXT5;
				return Fmt::Unknown;
			}
			snprintf(buf, sizeof(buf), "fmt%u", f);
			name = buf;
			switch (f)
			{
			case 71: case 72: name = "BC1"; return Fmt::DXT1;   // DXGI BC1_UNORM(_SRGB)
			case 74: case 75: name = "BC2"; return Fmt::DXT3;   // DXGI BC2_UNORM(_SRGB)
			case 77: case 78: name = "BC3"; return Fmt::DXT5;   // DXGI BC3_UNORM(_SRGB)
			case 21: name = "A8R8G8B8"; return Fmt::BGRA8;
			case 87: case 91: name = "B8G8R8A8"; return Fmt::BGRA8; // DXGI B8G8R8A8_UNORM(_SRGB)
			case 32: name = "A8B8G8R8"; return Fmt::RGBA8;
			case 28: case 29: name = "R8G8B8A8"; return Fmt::RGBA8; // DXGI R8G8B8A8_UNORM(_SRGB)
			default: return Fmt::Unknown;
			}
		}

		bool IsBlock(Fmt f) { return f == Fmt::DXT1 || f == Fmt::DXT3 || f == Fmt::DXT5; }
		size_t BlockBytes(Fmt f) { return f == Fmt::DXT1 ? 8 : 16; }

		// Bytes of one mip level, and the row pitch D3D wants for it.
		size_t LevelBytes(Fmt f, unsigned w, unsigned h, size_t& rowPitch)
		{
			if (IsBlock(f))
			{
				rowPitch = static_cast<size_t>((w + 3) / 4) * BlockBytes(f);
				return rowPitch * ((h + 3) / 4);
			}
			rowPitch = static_cast<size_t>(w) * 4;
			return rowPitch * h;
		}

		size_t TotalBytes(Fmt f, unsigned w, unsigned h, unsigned levels)
		{
			size_t total = 0;
			for (unsigned l = 0; l < levels; ++l)
			{
				size_t pitch;
				total += LevelBytes(f, w, h, pitch);
				w = w > 1 ? w / 2 : 1;
				h = h > 1 ? h / 2 : 1;
			}
			return total;
		}

		bool LampRelated(const std::string& name)
		{
			std::string s = name;
			for (char& c : s)
				c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
			return s.find("light") != std::string::npos || s.find("lamp") != std::string::npos || s.find("lantern") != std::string::npos
				|| s.find("glow") != std::string::npos || s.find("bulb") != std::string::npos;
		}

		std::string LowerName(const std::string& name)
		{
			std::string lower = name;
			for (char& c : lower)
				c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
			return lower;
		}

		// Retint wholesale (texture_force), with all_streetlights on.
		bool NameForced(const std::string& name, const Config& cfg)
		{
			if (!cfg.allStreetLights)
				return false;
			const std::string lower = LowerName(name);
			for (const std::string& pat : cfg.textureForce)
				if (!pat.empty() && lower.find(pat) != std::string::npos)
					return true;
			return false;
		}

		bool NameSelected(const std::string& name, const Config& cfg)
		{
			const std::string lower = LowerName(name);
			for (const std::string& ex : cfg.textureExclude)
				if (!ex.empty() && lower.find(ex) != std::string::npos)
					return false;
			for (const std::string& pat : cfg.textureNames)
				if (!pat.empty() && lower.find(pat) != std::string::npos)
					return true;
			return false;
		}

		// ------------------------------------------------------------ pixel pass

		void Rgb565(uint16_t c, unsigned& r, unsigned& g, unsigned& b)
		{
			r = ((c >> 11) & 31) * 255 / 31;
			g = ((c >> 5) & 63) * 255 / 63;
			b = (c & 31) * 255 / 31;
		}

		uint16_t Pack565(unsigned r, unsigned g, unsigned b)
		{
			return static_cast<uint16_t>(((r * 31 + 127) / 255) << 11 | ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
		}

		bool Recolour565(uint16_t& c, const MatchParams& m, bool force)
		{
			unsigned r, g, b;
			Rgb565(c, r, g, b);
			uint32_t packed = (r << 16) | (g << 8) | b;
			if (!(force ? ForceRecolor(packed, m) : Recolor(packed, m)))
				return false;
			c = Pack565((packed >> 16) & 0xFF, (packed >> 8) & 0xFF, packed & 0xFF);
			return true;
		}

		// One DXT colour block (8 bytes: c0, c1, 16 x 2-bit indices). DXT1 picks
		// its mode from the endpoint order (c0 > c1: four colours; else three +
		// transparent), so the order is preserved by swapping back and remapping
		// the indices when a recolour flips it.
		bool RecolourBlock(uint8_t* blk, const MatchParams& m, bool dxt1, bool force)
		{
			uint16_t c0 = *reinterpret_cast<uint16_t*>(blk);
			uint16_t c1 = *reinterpret_cast<uint16_t*>(blk + 2);
			const bool fourColour = c0 > c1;
			uint16_t n0 = c0, n1 = c1;
			const bool ch0 = Recolour565(n0, m, force);
			const bool ch1 = Recolour565(n1, m, force);
			if (!ch0 && !ch1)
				return false;
			if (dxt1)
			{
				uint32_t& idx = *reinterpret_cast<uint32_t*>(blk + 4);
				if (fourColour)
				{
					if (n0 == n1)
					{
						if (n1 > 0) n1--; else n0++;
					}
					if (n0 < n1)
					{
						std::swap(n0, n1);
						idx ^= 0x55555555u; // 0<->1, 2<->3
					}
				}
				else if (n0 > n1)
				{
					std::swap(n0, n1);
					uint32_t v = idx, out = 0;
					for (int i = 0; i < 16; ++i)
					{
						uint32_t s = (v >> (i * 2)) & 3;
						if (s == 0) s = 1; else if (s == 1) s = 0;
						out |= s << (i * 2);
					}
					idx = out;
				}
			}
			*reinterpret_cast<uint16_t*>(blk) = n0;
			*reinterpret_cast<uint16_t*>(blk + 2) = n1;
			return true;
		}

		// Recolour `levels` mips of pixel data in place. Returns blocks/pixels
		// changed.
		long RecolourPixels(uint8_t* px, size_t avail, Fmt kind, unsigned w, unsigned h, unsigned levels, const MatchParams& m, bool force = false)
		{
			long changed = 0;
			size_t off = 0;
			unsigned lw = w, lh = h;
			for (unsigned lvl = 0; lvl < levels; ++lvl)
			{
				size_t pitch;
				const size_t bytes = LevelBytes(kind, lw, lh, pitch);
				if (off + bytes > avail)
					break;
				if (IsBlock(kind))
				{
					const bool dxt1 = kind == Fmt::DXT1;
					const size_t bs = BlockBytes(kind);
					const size_t colourOff = dxt1 ? 0 : 8;
					for (size_t b = 0; b < bytes / bs; ++b)
						if (RecolourBlock(px + off + b * bs + colourOff, m, dxt1, force))
							changed++;
				}
				else
				{
					const bool bgra = kind == Fmt::BGRA8;
					for (size_t i = 0; i < bytes / 4; ++i)
					{
						uint8_t* q = px + off + i * 4;
						const unsigned rr = bgra ? q[2] : q[0], gg = q[1], bb = bgra ? q[0] : q[2];
						uint32_t packed = (rr << 16) | (gg << 8) | bb;
						if (!(force ? ForceRecolor(packed, m) : Recolor(packed, m)))
							continue;
						const uint8_t nr = (packed >> 16) & 0xFF, ng = (packed >> 8) & 0xFF, nb = packed & 0xFF;
						if (bgra) { q[2] = nr; q[1] = ng; q[0] = nb; } else { q[0] = nr; q[1] = ng; q[2] = nb; }
						changed++;
					}
				}
				off += bytes;
				lw = lw > 1 ? lw / 2 : 1;
				lh = lh > 1 ? lh / 2 : 1;
			}
			return changed;
		}

		// FNV-1a over a pixel buffer: cheap identity for "is this already shown".
		uint64_t HashBytes(const uint8_t* p, size_t n)
		{
			uint64_t h = 1469598103934665603ull;
			for (size_t i = 0; i < n; ++i)
			{
				h ^= p[i];
				h *= 1099511628211ull;
			}
			return h;
		}

		// ------------------------------------------------------------ live registry

		struct Reg
		{
			void* store = nullptr;
			uint32_t idx = 0;
			uint8_t* obj = nullptr;          // grcTexture, final address
			uint8_t* root = nullptr;         // the resource's root object (pool entry +0 while loaded)
			std::string name;
			Fmt kind = Fmt::Unknown;
			unsigned w = 0, h = 0, levels = 0;
			bool force = false;              // texture_force: every pixel retinted
			std::vector<uint8_t> original;   // all mips, untouched
			uint64_t placedHash = 0;         // pixels the game's GPU texture was built from
			uint64_t shownHash = 0;          // pixels currently on screen
			ID3D11Resource* gameTexture = nullptr; // the game's own, once seen
			ID3D11ShaderResourceView* gameSrv = nullptr;
			ID3D11Resource* ourTexture = nullptr;  // currently swapped in (null = game's)
			ID3D11ShaderResourceView* ourSrv = nullptr;
			// Earlier textures of ours for this object. Kept alive as long as the
			// entry lives: the game's own hi-detail swap (+hi dictionaries) moves
			// resource pointers between texture objects and restores them later,
			// so a pointer we retired can still be referenced somewhere we never
			// wrote. Released when the object unloads.
			std::vector<std::pair<ID3D11Resource*, ID3D11ShaderResourceView*>> retired;
		};

		SRWLOCK g_regLock = SRWLOCK_INIT;
		std::vector<Reg> g_regs;
		size_t g_originalBytes = 0;
		std::atomic<bool> g_repaintPending{ false };
		ULONGLONG g_lastRepaint = 0;
		int g_liveFailuresLogged = 0;

		void RetireLocked(Reg& r, ID3D11Resource* t, ID3D11ShaderResourceView* v)
		{
			if (t || v)
				r.retired.emplace_back(t, v);
		}

		// The object the store currently holds in that slot (pool entry +0), or
		// null. Read-only, probed.
		uint8_t* SlotObject(void* store, uint32_t idx)
		{
			const uint8_t* pool = static_cast<const uint8_t*>(store) + 56; // atPoolBase
			if (!ProbeReadable(pool, 24))
				return nullptr;
			const uint8_t* data = *reinterpret_cast<const uint8_t* const*>(pool);
			const uint32_t count = *reinterpret_cast<const uint32_t*>(pool + 16);
			const uint32_t entrySize = *reinterpret_cast<const uint32_t*>(pool + 20);
			if (!data || idx >= count || entrySize < 8 || entrySize > 256)
				return nullptr;
			const uint8_t* entry = data + static_cast<size_t>(idx) * entrySize;
			if (!ProbeReadable(entry, 8))
				return nullptr;
			return *reinterpret_cast<uint8_t* const*>(entry);
		}

		// Put the game's pointers back (if the object is still ours to touch),
		// release everything we created for it, forget the entry. Caller holds
		// g_regLock. `objectAlive` = the object memory is still the resource we
		// registered (false after an unload we only noticed late, or a move).
		void RestoreAndDropLocked(size_t i, bool objectAlive)
		{
			Reg& r = g_regs[i];
			if (objectAlive && r.ourTexture && r.gameTexture && ProbeReadable(r.obj, kTexSrv + 8))
			{
				InterlockedExchangePointer(reinterpret_cast<void* volatile*>(r.obj + kTexSrv), r.gameSrv);
				InterlockedExchangePointer(reinterpret_cast<void* volatile*>(r.obj + kTexResource), r.gameTexture);
			}
			RetireLocked(r, r.ourTexture, r.ourSrv);
			for (auto& t : r.retired)
			{
				if (t.second) t.second->Release();
				if (t.first) t.first->Release();
			}
			g_originalBytes -= r.original.size();
			g_regs.erase(g_regs.begin() + static_cast<ptrdiff_t>(i));
		}

		bool IsOursLocked(const Reg& r, ID3D11Resource* t)
		{
			if (t == r.ourTexture)
				return true;
			for (const auto& o : r.retired)
				if (o.first == t)
					return true;
			return false;
		}

		// Build a GPU texture from `pixels` on the device of `like`, with the
		// same description; false if anything about it is not as expected.
		bool CreateLike(ID3D11Resource* like, ID3D11ShaderResourceView* likeSrv, const Reg& r, const uint8_t* pixels,
			ID3D11Resource*& outTex, ID3D11ShaderResourceView*& outSrv, std::string& why)
		{
			outTex = nullptr;
			outSrv = nullptr;
			ID3D11Texture2D* tex2d = nullptr;
			if (FAILED(like->QueryInterface(__uuidof(ID3D11Texture2D), reinterpret_cast<void**>(&tex2d))) || !tex2d)
			{
				why = "not a 2D texture";
				return false;
			}
			D3D11_TEXTURE2D_DESC desc{};
			tex2d->GetDesc(&desc);
			ID3D11Device* dev = nullptr;
			tex2d->GetDevice(&dev);
			tex2d->Release();
			if (!dev)
			{
				why = "no device";
				return false;
			}
			bool ok = false;
			do
			{
				if (desc.ArraySize != 1 || desc.SampleDesc.Count != 1 || desc.Width != r.w || desc.Height != r.h)
				{
					why = "unexpected description";
					break;
				}
				std::string fname;
				if (Classify(desc.Format, fname) != r.kind)
				{
					why = "format differs from the resource's";
					break;
				}
				const unsigned levels = desc.MipLevels ? desc.MipLevels : r.levels;
				if (levels > r.levels || TotalBytes(r.kind, r.w, r.h, levels) > r.original.size())
				{
					why = "fewer mips kept than the GPU texture has";
					break;
				}
				std::vector<D3D11_SUBRESOURCE_DATA> init(levels);
				size_t off = 0;
				unsigned lw = r.w, lh = r.h;
				for (unsigned l = 0; l < levels; ++l)
				{
					size_t pitch;
					const size_t bytes = LevelBytes(r.kind, lw, lh, pitch);
					init[l].pSysMem = pixels + off;
					init[l].SysMemPitch = static_cast<UINT>(pitch);
					init[l].SysMemSlicePitch = static_cast<UINT>(bytes);
					off += bytes;
					lw = lw > 1 ? lw / 2 : 1;
					lh = lh > 1 ? lh / 2 : 1;
				}
				D3D11_TEXTURE2D_DESC nd = desc;
				nd.MipLevels = levels;
				nd.Usage = D3D11_USAGE_IMMUTABLE;
				nd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
				nd.CPUAccessFlags = 0;
				nd.MiscFlags = 0;
				ID3D11Texture2D* created = nullptr;
				if (FAILED(dev->CreateTexture2D(&nd, init.data(), &created)) || !created)
				{
					why = "CreateTexture2D failed";
					break;
				}
				D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
				D3D11_SHADER_RESOURCE_VIEW_DESC* psd = nullptr;
				if (likeSrv)
				{
					likeSrv->GetDesc(&sd);
					if (sd.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D)
					{
						sd.Texture2D.MostDetailedMip = 0;
						sd.Texture2D.MipLevels = levels;
						psd = &sd;
					}
				}
				ID3D11ShaderResourceView* srv = nullptr;
				if (FAILED(dev->CreateShaderResourceView(created, psd, &srv)) || !srv)
				{
					created->Release();
					why = "CreateShaderResourceView failed";
					break;
				}
				outTex = created;
				outSrv = srv;
				ok = true;
			} while (false);
			dev->Release();
			return ok;
		}

		// Rebuild every registered texture with the current config. Worker
		// thread.
		void RepaintAll()
		{
			const ConfigPtr cfgPtr = GetConfigPtr();
			const Config& cfg = *cfgPtr;
			const bool paint = cfg.texturesEnabled && cfg.match.enabled;
			unsigned rebuilt = 0, restored = 0, skipped = 0, failed = 0;
			std::vector<uint8_t> work;
			AcquireSRWLockExclusive(&g_regLock);
			unsigned dropped = 0;
			for (size_t i = 0; i < g_regs.size();)
			{
				Reg& r = g_regs[i];
				// Still the resource we registered? The store's pool says what it
				// holds in that slot; a move or an unload we did not hear ends here.
				if (SlotObject(r.store, r.idx) != r.root || !ProbeReadable(r.obj, kTexSrv + 8))
				{
					RestoreAndDropLocked(i, false);
					dropped++;
					continue;
				}
				++i;
				ID3D11Resource* curTex = *reinterpret_cast<ID3D11Resource**>(r.obj + kTexResource);
				ID3D11ShaderResourceView* curSrv = *reinterpret_cast<ID3D11ShaderResourceView**>(r.obj + kTexSrv);
				if (!curTex)
				{
					skipped++; // not created yet (placement still running)
					continue;
				}
				if (!r.gameTexture && !IsOursLocked(r, curTex))
				{
					r.gameTexture = curTex;
					r.gameSrv = curSrv;
				}
				if (curTex != r.gameTexture && !IsOursLocked(r, curTex))
				{
					// Neither the game's original nor any of ours: the game has swapped
					// something in (its hi-detail texture). Leave it alone this round.
					skipped++;
					continue;
				}

				// What this config wants on screen: the untouched pixels when the
				// plugin is disabled, otherwise the originals recoloured anew.
				work = r.original;
				if (paint)
					RecolourPixels(work.data(), work.size(), r.kind, r.w, r.h, r.levels, cfg.match, r.force && cfg.allStreetLights);
				const uint64_t wanted = HashBytes(work.data(), work.size());
				if (wanted == r.shownHash)
				{
					skipped++;
					continue;
				}
				if (wanted == r.placedHash)
				{
					// The game's own GPU texture already holds exactly this.
					InterlockedExchangePointer(reinterpret_cast<void* volatile*>(r.obj + kTexSrv), r.gameSrv);
					InterlockedExchangePointer(reinterpret_cast<void* volatile*>(r.obj + kTexResource), r.gameTexture);
					RetireLocked(r, r.ourTexture, r.ourSrv);
					r.ourTexture = nullptr;
					r.ourSrv = nullptr;
					r.shownHash = r.placedHash;
					restored++;
					continue;
				}
				ID3D11Resource* nt = nullptr;
				ID3D11ShaderResourceView* ns = nullptr;
				std::string why;
				if (!CreateLike(r.gameTexture, r.gameSrv, r, work.data(), nt, ns, why))
				{
					failed++;
					if (cfg.debug && g_liveFailuresLogged < 10)
					{
						g_liveFailuresLogged++;
						Log("tex: live rebuild of '%s' failed: %s", r.name.c_str(), why.c_str());
					}
					continue;
				}
				InterlockedExchangePointer(reinterpret_cast<void* volatile*>(r.obj + kTexSrv), ns);
				InterlockedExchangePointer(reinterpret_cast<void* volatile*>(r.obj + kTexResource), nt);
				RetireLocked(r, r.ourTexture, r.ourSrv);
				r.ourTexture = nt;
				r.ourSrv = ns;
				r.shownHash = wanted;
				rebuilt++;
			}
			ReleaseSRWLockExclusive(&g_regLock);
			if (cfg.debug)
				LogDebug("tex: live repaint: %u textures rebuilt, %u restored, %u unchanged, %u failed, %u dropped", rebuilt, restored, skipped, failed, dropped);
		}

		// ------------------------------------------------------------ walking resources

		// Encoded pointers of every grcTexture in a pgDictionary<grcTexture>.
		void TexturesOfDictionary(const Map& map, uint64_t dict, std::vector<uint64_t>& out)
		{
			uint64_t entries = 0;
			uint16_t count = 0;
			if (!dict || !ReadAt(map, dict, 0x30, entries) || !ReadAt(map, dict, 0x38, count) || count > 4096)
				return;
			for (uint16_t i = 0; i < count; ++i)
			{
				uint64_t tex = 0;
				if (!ReadAt(map, entries, static_cast<size_t>(i) * 8, tex) || !tex)
					break;
				out.push_back(tex);
			}
		}

		// The embedded dictionary of a gtaDrawable (via its shader group).
		void TexturesOfDrawable(const Map& map, uint64_t drawable, std::vector<uint64_t>& out)
		{
			uint64_t shaderGroup = 0, dict = 0;
			if (!drawable || !ReadAt(map, drawable, 0x10, shaderGroup) || !shaderGroup || !ReadAt(map, shaderGroup, 0x08, dict) || !dict)
				return;
			TexturesOfDictionary(map, dict, out);
		}

		void TexturesOfResource(StoreKind kind, const Map& map, std::vector<uint64_t>& out)
		{
			const uint64_t root = map.blocks[0].encodedBase;
			switch (kind)
			{
			case Txd:
				TexturesOfDictionary(map, root, out);
				break;
			case Ydr:
				TexturesOfDrawable(map, root, out);
				break;
			case Yft:
			{
				uint64_t drawable = 0;
				if (ReadAt(map, root, 0x30, drawable))
					TexturesOfDrawable(map, drawable, out);
				uint64_t extra = 0;
				uint8_t extraCount = 0;
				if (ReadAt(map, root, 0x38, extra) && ReadAt(map, root, 0x48, extraCount) && extra && extraCount && extraCount < 64)
				{
					for (uint8_t i = 0; i < extraCount; ++i)
					{
						uint64_t d = 0;
						if (ReadAt(map, extra, static_cast<size_t>(i) * 8, d) && d)
							TexturesOfDrawable(map, d, out);
					}
				}
				break;
			}
			case Ydd:
			{
				uint64_t entries = 0;
				uint16_t count = 0;
				if (ReadAt(map, root, 0x30, entries) && ReadAt(map, root, 0x38, count) && count <= 4096)
				{
					for (uint16_t i = 0; i < count; ++i)
					{
						uint64_t d = 0;
						if (!ReadAt(map, entries, static_cast<size_t>(i) * 8, d) || !d)
							break;
						TexturesOfDrawable(map, d, out);
					}
				}
				break;
			}
			default:
				break;
			}
			std::vector<uint64_t> uniq;
			for (uint64_t t : out)
			{
				bool seen = false;
				for (uint64_t u : uniq)
					seen |= u == t;
				if (!seen)
					uniq.push_back(t);
			}
			out.swap(uniq);
		}

		// Debug: the most common block-endpoint colours of mip 0, so a selected
		// texture that matched nothing can be read in RGB/HSV from the log.
		std::string EndpointHistogram(const uint8_t* px, size_t avail, Fmt kind, unsigned w, unsigned h)
		{
			if (!IsBlock(kind))
				return "";
			const size_t bs = BlockBytes(kind);
			const size_t colourOff = kind == Fmt::DXT1 ? 0 : 8;
			size_t pitch;
			const size_t bytes = LevelBytes(kind, w, h, pitch);
			if (bytes > avail)
				return "";
			struct Bin { uint16_t c; unsigned n; };
			std::vector<Bin> bins;
			for (size_t b = 0; b < bytes / bs; ++b)
			{
				for (int k = 0; k < 2; ++k)
				{
					const uint16_t c = *reinterpret_cast<const uint16_t*>(px + b * bs + colourOff + k * 2);
					bool found = false;
					for (Bin& bin : bins)
						if (bin.c == c) { bin.n++; found = true; break; }
					if (!found && bins.size() < 512)
						bins.push_back({ c, 1 });
				}
			}
			std::sort(bins.begin(), bins.end(), [](const Bin& a, const Bin& b) { return a.n > b.n; });
			std::string out;
			char buf[64];
			for (size_t i = 0; i < bins.size() && i < 6; ++i)
			{
				unsigned r, g, bl;
				Rgb565(bins[i].c, r, g, bl);
				const HSV hsv = ToHSV(RGB{ float(r), float(g), float(bl) });
				snprintf(buf, sizeof(buf), " (%u,%u,%u h%.0f s%.2f)x%u", r, g, bl, hsv.h, hsv.s, bins[i].n);
				out += buf;
			}
			return out;
		}

		struct TexResult
		{
			std::string name;
			std::string fmt;
			bool selected = false;
			bool handled = true;
			bool registered = false;
			long changed = 0;
			std::string histogram; // debug, only when nothing changed
		};

		// Recolour one texture in the raw resource and register it for live
		// repaint.
		TexResult ProcessTexture(void* store, uint32_t idx, const Map& map, uint64_t texPtr, const Config& cfg)
		{
			TexResult r;
			uint64_t namePtr = 0, dataPtr = 0;
			uint16_t w = 0, h = 0;
			uint32_t fmt = 0;
			uint8_t levels = 0;
			if (!ReadAt(map, texPtr, kTexName, namePtr) || !ReadAt(map, texPtr, kTexWidth, w) || !ReadAt(map, texPtr, kTexHeight, h)
				|| !ReadAt(map, texPtr, kTexFormat, fmt) || !ReadAt(map, texPtr, kTexLevels, levels) || !ReadAt(map, texPtr, kTexData, dataPtr))
			{
				r.handled = false;
				return r;
			}
			r.name = ReadName(map, namePtr);
			const bool force = NameForced(r.name, cfg);
			if (!force && !NameSelected(r.name, cfg))
				return r;
			r.selected = true;
			const Fmt kind = Classify(fmt, r.fmt);
			size_t avail = 0, objAvail = 0;
			uint8_t* px = Resolve(map, dataPtr, avail);
			uint8_t* obj = Resolve(map, texPtr, objAvail);
			if (!px || !obj || w == 0 || h == 0 || kind == Fmt::Unknown)
			{
				r.handled = false;
				return r;
			}
			if (levels == 0)
				levels = 1;
			const size_t total = TotalBytes(kind, w, h, levels);
			const size_t bytes = total <= avail ? total : avail;

			// Keep the untouched pixels, recolour in place, remember what the
			// game's GPU texture will be built from.
			std::vector<uint8_t> original;
			const bool keep = cfg.liveRepaint && objAvail >= kTexSrv + 8;
			if (keep)
				original.assign(px, px + bytes);

			r.changed = RecolourPixels(px, bytes, kind, w, h, levels, cfg.match, force);
			if (r.changed)
			{
				g_texturesRecoloured++;
				g_blocksRecoloured += r.changed;
			}
			else if (cfg.debug)
				r.histogram = EndpointHistogram(px, bytes, kind, w, h);

			if (keep)
			{
				AcquireSRWLockExclusive(&g_regLock);
				if (g_originalBytes + bytes <= kMaxOriginalBytes)
				{
					Reg reg;
					reg.store = store;
					reg.idx = idx;
					reg.obj = obj;
					reg.root = const_cast<uint8_t*>(map.blocks[0].data);
					reg.name = r.name;
					reg.kind = kind;
					reg.w = w;
					reg.h = h;
					reg.levels = levels;
					reg.force = force;
					reg.placedHash = HashBytes(px, bytes);
					reg.shownHash = reg.placedHash;
					reg.original = std::move(original);
					g_originalBytes += bytes;
					g_regs.push_back(std::move(reg));
					r.registered = true;
				}
				ReleaseSRWLockExclusive(&g_regLock);
			}
			return r;
		}

		void OnPlace(StoreKind kind, uint32_t object, void* blockMap)
		{
			timing::Scope timed(timing::Placement);
			const uint64_t n = ++g_placements;
			const ConfigPtr cfgPtr = GetConfigPtr();
			const Config& cfg = *cfgPtr;
			if (!cfg.texturesEnabled || !cfg.match.enabled)
				return;
			Map map;
			if (!ReadMap(blockMap, map))
			{
				if (cfg.debug && n <= 3)
					Log("tex: %s place #%llu slot %u: map %p not in the expected layout", g_hooks[kind].name, (unsigned long long)n, object, blockMap);
				return;
			}
			std::vector<uint64_t> textures;
			TexturesOfResource(kind, map, textures);
			if (textures.empty())
				return;

			// A slot being re-placed drops whatever was registered for it before.
			AcquireSRWLockExclusive(&g_regLock);
			for (size_t i = g_regs.size(); i-- > 0;)
				if (g_regs[i].store == g_hooks[kind].store && g_regs[i].idx == object)
					RestoreAndDropLocked(i, false);
			ReleaseSRWLockExclusive(&g_regLock);

			bool any = false;
			std::string names;
			bool related = false;
			for (uint64_t t : textures)
			{
				TexResult r = ProcessTexture(g_hooks[kind].store, object, map, t, cfg);
				if (r.selected)
				{
					any = true;
					if (cfg.debug)
						Log("tex: %s slot %u '%s' %s: %ld blocks recoloured%s%s%s%s", g_hooks[kind].name, object, r.name.c_str(), r.fmt.c_str(), r.changed,
							r.handled ? "" : " (format not handled)", r.registered ? "" : " (not registered for live repaint)",
							r.histogram.empty() ? "" : "; top colours:", r.histogram.c_str());
				}
				else if (cfg.debug && kind == Txd)
				{
					related |= LampRelated(r.name);
					if (names.size() < 600)
					{
						names += ' ';
						names += r.name;
					}
				}
			}
			if (any)
				g_dictionaries++;
			else if (cfg.debug && related && g_dictsLogged < 120)
			{
				g_dictsLogged++;
				Log("tex: unselected light-related dictionary (slot %u, %u textures):%s", object, (unsigned)textures.size(), names.c_str());
			}
		}

		template <int K>
		void* PlaceThunk(void* store, uint32_t object, void* blockMap, const char* name)
		{
			for (int k = 0; k < StoreKindCount; ++k)
			{
				if (g_hooks[k].active && g_hooks[k].store == store)
				{
					OnPlace(static_cast<StoreKind>(k), object, blockMap);
					break;
				}
			}
			return g_hooks[K].orig(store, object, blockMap, name);
		}

		void* ThunkFor(int k)
		{
			switch (k)
			{
			case 0: return reinterpret_cast<void*>(&PlaceThunk<0>);
			case 1: return reinterpret_cast<void*>(&PlaceThunk<1>);
			case 2: return reinterpret_cast<void*>(&PlaceThunk<2>);
			default: return reinterpret_cast<void*>(&PlaceThunk<3>);
			}
		}

		// ------------------------------------------------------------ finding the routine

		// Name of the module containing `p` (empty if none): FiveM replaces
		// some vtable entries with runtime-generated heap code.
		std::string ModuleOf(const void* p, uintptr_t* base = nullptr)
		{
			HMODULE mod = nullptr;
			if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(p), &mod) || !mod)
				return "";
			if (base)
				*base = reinterpret_cast<uintptr_t>(mod);
			wchar_t path[MAX_PATH];
			DWORD n = GetModuleFileNameW(mod, path, MAX_PATH);
			std::wstring w(path, n);
			size_t slash = w.find_last_of(L"\\/");
			std::wstring nm = slash == std::wstring::npos ? w : w.substr(slash + 1);
			return std::string(nm.begin(), nm.end());
		}

		// Follow plain jump stubs for a few hops.
		const uint8_t* FollowStubs(const uint8_t* p)
		{
			for (int hop = 0; hop < 6; ++hop)
			{
				if (!ProbeReadable(p, 16))
					return nullptr;
				const uint8_t* next = nullptr;
				if (p[0] == 0xE9)
					next = p + 5 + *reinterpret_cast<const int32_t*>(p + 1);
				else if (p[0] == 0xFF && p[1] == 0x25)
				{
					const uint8_t* slot = p + 6 + *reinterpret_cast<const int32_t*>(p + 2);
					if (ProbeReadable(slot, 8))
						next = *reinterpret_cast<const uint8_t* const*>(slot);
				}
				else if (p[0] == 0x48 && p[1] == 0xB8 && p[10] == 0xFF && p[11] == 0xE0)
					next = *reinterpret_cast<const uint8_t* const*>(p + 2);
				else if (p[0] == 0x49 && p[1] == 0xBB && p[10] == 0x41 && p[11] == 0xFF && p[12] == 0xE3)
					next = *reinterpret_cast<const uint8_t* const*>(p + 2);
				if (!next)
					return p;
				p = next;
			}
			return p;
		}

		// FiveM's generated handler calls the game's own routine once; find
		// the single call target inside the game image.
		const uint8_t* GameCallTargetOf(const uint8_t* body, uintptr_t imageBase, size_t imageSize, const char* who)
		{
			const uint8_t* found[8];
			int n = 0;
			const size_t span = 768;
			if (!ProbeReadable(body, span))
				return nullptr;
			for (size_t i = 0; i + 12 < span; ++i)
			{
				const uint8_t* t = nullptr;
				if (body[i] == 0xE8)
					t = body + i + 5 + *reinterpret_cast<const int32_t*>(body + i + 1);
				else if (body[i] == 0xFF && body[i + 1] == 0x15)
				{
					const uint8_t* slot = body + i + 6 + *reinterpret_cast<const int32_t*>(body + i + 2);
					if (ProbeReadable(slot, 8))
						t = *reinterpret_cast<const uint8_t* const*>(slot);
				}
				else if ((body[i] == 0x48 || body[i] == 0x49) && (body[i + 1] & 0xF8) == 0xB8)
					t = *reinterpret_cast<const uint8_t* const*>(body + i + 2);
				if (!t)
					continue;
				const uintptr_t ta = reinterpret_cast<uintptr_t>(t);
				if (ta < imageBase || ta >= imageBase + imageSize)
					continue;
				bool dup = false;
				for (int k = 0; k < n; ++k)
					dup |= found[k] == t;
				if (!dup && n < 8)
					found[n++] = t;
			}
			if (n != 1)
			{
				Log("tex: %s: %d distinct game-image call targets in FiveM's handler; not hooking (need exactly one)", who, n);
				return nullptr;
			}
			return found[0];
		}
	}

	bool InstallPlacementHook(StoreKind kind, void* store, int placeSlot, uintptr_t imageBase, size_t imageSize)
	{
		Hook& h = g_hooks[kind];
		h.store = store;
		void** vt = *static_cast<void***>(store);
		void* fn = vt[placeSlot];
		uintptr_t a = reinterpret_cast<uintptr_t>(fn);
		if (a < imageBase || a >= imageBase + imageSize)
		{
			const uint8_t* body = FollowStubs(static_cast<const uint8_t*>(fn));
			const std::string mod = body ? ModuleOf(body) : "";
			if (body && mod.empty())
				body = GameCallTargetOf(body, imageBase, imageSize, h.name);
			else if (body && !mod.empty())
				Log("tex: %s: PlaceResource slot %d leads into %s; treating it as the routine", h.name, placeSlot, mod.c_str());
			if (!body)
			{
				Log("tex: %s: PlaceResource slot %d = %p could not be resolved; not hooked", h.name, placeSlot, fn);
				h.store = nullptr;
				return false;
			}
			fn = const_cast<uint8_t*>(body);
			a = reinterpret_cast<uintptr_t>(fn);
		}
		h.target = fn;

		MH_STATUS st = MH_CreateHook(fn, ThunkFor(kind), reinterpret_cast<void**>(&h.orig));
		if (st == MH_ERROR_ALREADY_CREATED)
		{
			for (int k = 0; k < StoreKindCount; ++k)
			{
				if (k != kind && g_hooks[k].active && g_hooks[k].target == fn)
				{
					h.orig = g_hooks[k].orig;
					h.active = true;
					Log("tex: %s store %p: PlaceResource shares %s's routine %p (already detoured)", h.name, store, g_hooks[k].name, fn);
					return true;
				}
			}
			Log("tex: %s store %p: PlaceResource routine %p is detoured by something else; not hooked", h.name, store, fn);
			h.store = nullptr;
			return false;
		}
		if (st == MH_OK)
			st = MH_EnableHook(fn);
		if (st != MH_OK)
		{
			Log("tex: %s: detouring PlaceResource at %p failed: %s", h.name, fn, MH_StatusToString(st));
			h.store = nullptr;
			h.orig = nullptr;
			return false;
		}
		h.active = true;
		Log("tex: %s store %p: PlaceResource vtable[%d] -> %p (%s+0x%llX) detoured", h.name, store, placeSlot, fn, ModuleOf(fn).c_str(), (unsigned long long)(a - imageBase));
		return true;
	}

	void OnStoreRemove(void* store, uint32_t idx)
	{
		AcquireSRWLockExclusive(&g_regLock);
		for (size_t i = g_regs.size(); i-- > 0;)
			if (g_regs[i].store == store && g_regs[i].idx == idx)
				RestoreAndDropLocked(i, SlotObject(store, idx) == g_regs[i].root);
		ReleaseSRWLockExclusive(&g_regLock);
	}

	void RequestRepaint()
	{
		g_repaintPending = true;
	}

	void Tick()
	{
		const ULONGLONG now = GetTickCount64();
		if (g_repaintPending && now - g_lastRepaint >= 250)
		{
			g_repaintPending = false;
			g_lastRepaint = now;
			RepaintAll();
		}
	}

	uint64_t Placements() { return g_placements.load(); }
	uint64_t Dictionaries() { return g_dictionaries.load(); }
	uint64_t TexturesRecoloured() { return g_texturesRecoloured.load(); }
	uint64_t BlocksRecoloured() { return g_blocksRecoloured.load(); }
	uint64_t Registered()
	{
		AcquireSRWLockShared(&g_regLock);
		const uint64_t n = g_regs.size();
		ReleaseSRWLockShared(&g_regLock);
		return n;
	}
}
