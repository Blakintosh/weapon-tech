// BO3 asset pools (DB_XAssetPool) and rawfiles, shared by bo3_stencil.h (technique sets), stub_boot (persist=) and
// weapon_tech (its cfg baked into the map zone as a rawfile).
//
// DB_XAssetPool is filled in at runtime, so it's found as a run of plausible 32-byte pool entries in the exe's data
// whose entry 8 (technique sets) has 0x70-byte items.
#pragma once
#include "bo3_zone.h"

namespace
{
	struct XAssetPool
	{
		uint8_t *pool;
		uint32_t itemSize;
		int32_t itemCount;
		uint8_t isSingleton[4];
		int32_t itemAllocCount;
		void *freeHead;
	};
	static_assert(sizeof(XAssetPool) == 32, "XAssetPool layout");

	constexpr int kTechsetType = 8;
	constexpr uint32_t kTechsetSize = 0x70;  // name, flags, 12 technique pointers

	bool PlausiblePool(const XAssetPool &p, bool allowEmpty)
	{
		if (!p.pool)
			return allowEmpty && !p.itemSize && !p.itemCount;
		if (p.itemSize < 4 || p.itemSize > 0x40000 || p.itemCount <= 0 || p.itemCount > (1 << 22))
			return false;
		auto *free = static_cast<uint8_t *>(p.freeHead);
		return !free || (free >= p.pool && free < p.pool + static_cast<size_t>(p.itemSize) * p.itemCount);
	}

	XAssetPool *FindAssetPools()
	{
		if (!g_base)
			g_base = reinterpret_cast<uint8_t *>(GetModuleHandleA(nullptr));
		constexpr int kCheck = 0x40;
		auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(g_base + reinterpret_cast<IMAGE_DOS_HEADER *>(g_base)->e_lfanew);
		uint8_t *end = g_base + nt->OptionalHeader.SizeOfImage;
		MEMORY_BASIC_INFORMATION mbi;
		for (uint8_t *r = g_base; r < end && VirtualQuery(r, &mbi, sizeof(mbi)); r = static_cast<uint8_t *>(mbi.BaseAddress) + mbi.RegionSize)
		{
			bool writable = mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) &&
			                !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS));
			if (!writable)
				continue;
			auto *lo = static_cast<uint8_t *>(mbi.BaseAddress) + 32;
			uint8_t *hi = (std::min)(static_cast<uint8_t *>(mbi.BaseAddress) + mbi.RegionSize, end) - 32 * kCheck;
			for (uint8_t *q = lo; q < hi; q += 8)
			{
				auto *pools = reinterpret_cast<XAssetPool *>(q);
				if (pools[kTechsetType].itemSize != kTechsetSize || !PlausiblePool(pools[0], false) ||
				    PlausiblePool(pools[-1], false))
					continue;
				bool all = true;
				for (int i = 1; i < kCheck && all; i++)
					all = PlausiblePool(pools[i], true);
				if (all && PlausiblePool(pools[kTechsetType], false))
					return pools;
			}
		}
		return nullptr;
	}

	// ---- rawfiles ------------------------------------------------------------------------------------------------------
	struct RawFile
	{
		const char *name;
		int32_t len;
		int32_t pad;
		const char *buffer;
	};
	constexpr int kRawFileType = 0x2F;

	// The rawfile pool (nullptr if the pools aren't found or entry 0x2F isn't rawfile-shaped).
	const XAssetPool *RawFilePool()
	{
		XAssetPool *pools = FindAssetPools();
		if (!pools || pools[kRawFileType].itemSize != sizeof(RawFile))
			return nullptr;
		return &pools[kRawFileType];
	}

	// Calls fn(RawFile &) for every loaded rawfile named `name` whose buffer is readable; returns how many there were.
	template <typename Fn>
	int ForEachRawFile(const XAssetPool &pool, const char *name, Fn fn)
	{
		auto *items = reinterpret_cast<RawFile *>(pool.pool);
		size_t want = strlen(name) + 1;
		int n = 0;
		ReadCache rc;  // one VirtualQuery per region, resident pages from the working set (was a VirtualQuery per name)
		for (int i = 0; i < pool.itemCount; i++)
		{
			RawFile &item = items[i];
			if (!item.name || !rc.Readable(item.name, want) || memcmp(item.name, name, want) != 0)
				continue;
			if (item.len <= 0 || item.len > 0x1000000 || !Readable(item.buffer, item.len))  // once, for the match
				continue;
			fn(item);
			n++;
		}
		return n;
	}
}
