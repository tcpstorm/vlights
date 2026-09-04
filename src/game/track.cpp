#include "game/track.h"
#include "hook/pattern.h"
#include "plugin/log.h"

#include <windows.h>
#include <MinHook.h>

#include <atomic>

#include <cstdio>
#include <string>
#include <unordered_map>

namespace lodlight::track
{
	namespace
	{
		struct PoolBase // rage::atPoolBase (Cfx code/client/shared/atPool.h)
		{
			char* data;
			int8_t* flags;
			uint32_t count;
			uint32_t entrySize;
		};
		constexpr size_t kPoolOffset = 56;
		constexpr size_t kEntryNameHashOffset = 12;

		struct Entry
		{
			void* obj = nullptr;
			void* entryObj = nullptr; // pool entry +0 at register time (may be null then)
			uint32_t nameHash = 0;    // pool entry +12 at register time
			std::vector<uint32_t> originals;
		};

		struct StoreState
		{
			void* store = nullptr;
			bool removeHooked = false;
			bool removeFailed = false;
			bool layoutLogged = false;
			std::unordered_map<uint32_t, Entry> entries;
		};

		using RemoveFn = void (*)(void* store, uint32_t id);

		SRWLOCK g_lock = SRWLOCK_INIT;
		StoreState g_stores[KindCount];
		RepaintFn g_repaint[KindCount] = {};
		RemoveFn g_origRemove[KindCount] = {};
		uintptr_t g_base = 0;
		size_t g_size = 0;

		const char* kNames[KindCount] = { "ymap", "ydr", "yft", "ydd", "ytyp" };

		const PoolBase* Pool(void* store)
		{
			return reinterpret_cast<const PoolBase*>(static_cast<char*>(store) + kPoolOffset);
		}

		const char* EntryAt(void* store, uint32_t idx)
		{
			const PoolBase* pool = Pool(store);
			if (!pool->data || !pool->flags || idx >= pool->count || pool->entrySize < kEntryNameHashOffset + 4)
				return nullptr;
			if (pool->flags[idx] < 0)
				return nullptr;
			return pool->data + static_cast<size_t>(idx) * pool->entrySize;
		}

		std::string DumpEntry(const char* entry, size_t entrySize)
		{
			char buf[256] = {};
			size_t pos = 0;
			for (size_t off = 0; off + sizeof(void*) <= entrySize && pos < sizeof(buf) - 24; off += sizeof(void*))
			{
				unsigned long long q = *reinterpret_cast<const unsigned long long*>(entry + off);
				pos += static_cast<size_t>(snprintf(buf + pos, sizeof(buf) - pos, " +%u=%016llX", (unsigned)off, q));
			}
			return buf;
		}

		// Caller holds g_lock.
		bool SlotLive(const StoreState& st, uint32_t idx, const Entry& e)
		{
			const char* entry = EntryAt(st.store, idx);
			if (!entry)
				return false;
			if (e.entryObj && *reinterpret_cast<void* const*>(entry) != e.entryObj)
				return false;
			return *reinterpret_cast<const uint32_t*>(entry + kEntryNameHashOffset) == e.nameHash;
		}

		std::atomic<uint64_t> g_removeCalls{ 0 };
		std::atomic<uint64_t> g_removeDropped{ 0 };

		void HandleRemove(void* store, uint32_t id)
		{
			const uint64_t call = ++g_removeCalls;
			bool dropped = false;
			AcquireSRWLockExclusive(&g_lock);
			for (int k = 0; k < KindCount; ++k)
			{
				if (g_stores[k].store == store)
					dropped |= g_stores[k].entries.erase(id) != 0;
			}
			ReleaseSRWLockExclusive(&g_lock);
			if (dropped)
				g_removeDropped++;
			if (call <= 5 || (dropped && g_removeDropped <= 5))
				LogDebug("remove: store %p id %u%s (call %llu)", store, id, dropped ? " -> dropped from registry" : "", (unsigned long long)call);
		}

		template <int K>
		void RemoveThunk(void* store, uint32_t id)
		{
			HandleRemove(store, id);
			g_origRemove[K](store, id);
		}

		void* RemoveThunkFor(int k)
		{
			switch (k)
			{
			case 0: return reinterpret_cast<void*>(&RemoveThunk<0>);
			case 1: return reinterpret_cast<void*>(&RemoveThunk<1>);
			case 2: return reinterpret_cast<void*>(&RemoveThunk<2>);
			case 3: return reinterpret_cast<void*>(&RemoveThunk<3>);
			default: return reinterpret_cast<void*>(&RemoveThunk<4>);
			}
		}

		// Caller holds g_lock.
		bool EnsureRemoveHookLocked(int k, void* store)
		{
			StoreState& st = g_stores[k];
			if (st.removeHooked)
				return true;
			if (st.removeFailed)
				return false;

			const int slot = 3 + StreamingVtableShift(); // strStreamingModule::Remove
			void** vt = *static_cast<void***>(store);
			void* fn = vt[slot];
			uintptr_t a = reinterpret_cast<uintptr_t>(fn);
			if (a < g_base || a >= g_base + g_size)
			{
				Log("%s store vtable slot %d = %p is outside the game image; live repaint disabled for it", kNames[k], slot, fn);
				st.removeFailed = true;
				return false;
			}

			MH_STATUS cs = MH_CreateHook(fn, RemoveThunkFor(k), reinterpret_cast<void**>(&g_origRemove[k]));
			if (cs == MH_ERROR_ALREADY_CREATED)
			{
				// Another kind's store shares this Remove; that thunk erases by
				// store pointer, so it covers this store too.
				st.removeHooked = true;
				Log("%s store: Remove at %p already detoured (shared)", kNames[k], fn);
				return true;
			}
			if (cs != MH_OK || MH_EnableHook(fn) != MH_OK)
			{
				Log("%s store: detouring Remove at %p failed (%s); live repaint disabled for it", kNames[k], fn, MH_StatusToString(cs));
				g_origRemove[k] = nullptr;
				st.removeFailed = true;
				return false;
			}

			st.removeHooked = true;
			const PoolBase* pool = Pool(store);
			Log("%s store %p: Remove (vtable slot %d) at %p (base+0x%llX) detoured; pool count=%u entrySize=%u",
				kNames[k], store, slot, fn, (unsigned long long)(a - g_base), pool->count, pool->entrySize);
			return true;
		}
	}

	void Init(uintptr_t imageBase, size_t imageSize)
	{
		g_base = imageBase;
		g_size = imageSize;
	}

	void SetRepaint(Kind k, RepaintFn fn)
	{
		g_repaint[k] = fn;
	}

	const char* KindName(Kind k)
	{
		return kNames[k];
	}

	void Register(Kind k, void* store, uint32_t idx, void* obj, std::vector<uint32_t>&& originals)
	{
		AcquireSRWLockExclusive(&g_lock);
		StoreState& st = g_stores[k];
		if (!st.store)
			st.store = store;
		if (st.store != store)
		{
			ReleaseSRWLockExclusive(&g_lock);
			Log("%s: object from a second store %p (tracking only %p)", kNames[k], store, st.store);
			return;
		}
		if (EnsureRemoveHookLocked(k, store))
		{
			const char* entry = EntryAt(store, idx);
			Entry& e = st.entries[idx];
			e.obj = obj;
			e.entryObj = entry ? *reinterpret_cast<void* const*>(entry) : nullptr;
			e.nameHash = entry ? *reinterpret_cast<const uint32_t*>(entry + kEntryNameHashOffset) : 0;
			e.originals = std::move(originals);
			if (!st.layoutLogged && entry)
			{
				st.layoutLogged = true;
				Log("%s pool entry for slot %u (obj %p):%s", kNames[k], idx, obj, DumpEntry(entry, Pool(store)->entrySize).c_str());
			}
			if (!entry)
				st.entries.erase(idx); // cannot verify liveness later; do not keep it
		}
		ReleaseSRWLockExclusive(&g_lock);
	}

	Totals ReapplyAll(const Config& cfg)
	{
		Totals t;
		AcquireSRWLockExclusive(&g_lock);
		for (int k = 0; k < KindCount; ++k)
		{
			StoreState& st = g_stores[k];
			if (!st.store || !g_repaint[k])
				continue;
			std::vector<uint32_t> dead;
			for (auto& kv : st.entries)
			{
				const uint32_t idx = kv.first;
				Entry& e = kv.second;
				if (!SlotLive(st, idx, e))
				{
					t.stale++;
					dead.push_back(idx);
					continue;
				}
				uint64_t lights = 0, changed = 0;
				if (!g_repaint[k](e.obj, e.originals, cfg, lights, changed))
				{
					t.mismatched++;
					dead.push_back(idx);
					continue;
				}
				t.objects++;
				t.lights += lights;
				t.changed += changed;
			}
			for (uint32_t idx : dead)
				st.entries.erase(idx);
		}
		ReleaseSRWLockExclusive(&g_lock);
		return t;
	}

	void ForEachLoaded(Kind k, VisitFn fn, void* user)
	{
		AcquireSRWLockShared(&g_lock);
		StoreState& st = g_stores[k];
		if (st.store)
		{
			for (auto& kv : st.entries)
			{
				if (SlotLive(st, kv.first, kv.second))
					fn(kv.second.obj, kv.first, user);
			}
		}
		ReleaseSRWLockShared(&g_lock);
	}

	void RemoveStats(uint64_t& calls, uint64_t& dropped)
	{
		calls = g_removeCalls.load();
		dropped = g_removeDropped.load();
	}

	uint64_t CountLoaded()
	{
		uint64_t n = 0;
		AcquireSRWLockShared(&g_lock);
		for (int k = 0; k < KindCount; ++k)
			n += g_stores[k].entries.size();
		ReleaseSRWLockShared(&g_lock);
		return n;
	}
}
