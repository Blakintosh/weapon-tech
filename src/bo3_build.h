// Which BlackOps3 exe the weapon tech runs in, and the per-build address switch.
//
// Two exes are supported, each pinned by SizeOfImage + PE TimeDateStamp (any other exe: nothing is installed):
//   Enhanced: the BO3 Enhanced BlackOps3.exe (CL 20659811), 0x1A53F000 / 0x67363F2A. Plain .text, no Arxan.
//   Retail:   the stock Steam BlackOps3.exe (CL 13892626; BlackOps3b.exe on the dev machine), 0x1D74B000 /
//             0x693D731E. Arxan: .text is decrypted at start-up and integrity checks run all the time, so
//             bo3_arxan.h neutralises them (once per process, shared with stub_boot.dll; it recognises
//             T7Overcharged's patches too) before any code is patched.
// The two are different compiles: every code RVA, and most register contracts at hook sites, differ. Struct
// layouts (playerState, cg, DObj, XAnim, WeaponDef...) were checked to be the same.
//
// Each header keeps its Enhanced values as the default (mutable, not constexpr) and bo3_retail.h overwrites them
// once, at init, on the retail exe (ApplyRetailAddresses). Code that depends on registers or instruction bytes at a
// site branches on IsRetailExe().
#pragma once
#include "bo3_zone.h"

namespace
{
	enum class WtExe { Unknown, Enhanced, Retail };
	WtExe g_wtExe = WtExe::Unknown;

	constexpr DWORD kEnhancedImageSize = 0x1A53F000, kEnhancedStamp = 0x67363F2A;
	constexpr DWORD kRetailImageSize = 0x1D74B000, kRetailStamp = 0x693D731E;

	inline bool IsRetailExe() { return g_wtExe == WtExe::Retail; }
	inline bool IsEnhancedExe() { return g_wtExe == WtExe::Enhanced; }
	// The per-feature install gate: one of the two exes this DLL has addresses for.
	inline bool WtExeSupported() { return g_wtExe != WtExe::Unknown; }
	inline const char *WtExeName() { return IsRetailExe() ? "retail" : IsEnhancedExe() ? "enhanced" : "unknown"; }

	// Debug aid: WEAPONTECH_SKIP=<name>[,<name>...] in the game's environment leaves those install groups out (bisecting
	// a crash). Names: melee, recoil, anim, perf, ik, inspect, intmelee, interrupt, segreload, slide, vmfov.
	bool WtDebugSkip(const char *name)
	{
		static char s_skip[256];
		static int s_read;
		if (!s_read)
		{
			s_read = 1;
			if (!GetEnvironmentVariableA("WEAPONTECH_SKIP", s_skip + 1, sizeof(s_skip) - 2))
				s_skip[1] = 0;
			s_skip[0] = ',';
			strcat_s(s_skip, ",");
		}
		char key[40];
		snprintf(key, sizeof(key), ",%s,", name);
		if (!strstr(s_skip, key))
			return false;
		Log("weapon_tech: '%s' skipped (WEAPONTECH_SKIP)", name);
		return true;
	}

	// The image size of the exe running (0 before DetectWtExe).
	DWORD g_wtImageSize;

	// Sets g_wtExe from the running exe's PE header. g_base must be set (KnownBuild()).
	WtExe DetectWtExe()
	{
		const auto *nt = reinterpret_cast<const IMAGE_NT_HEADERS *>(g_base + reinterpret_cast<const IMAGE_DOS_HEADER *>(g_base)->e_lfanew);
		const DWORD size = nt->OptionalHeader.SizeOfImage, stamp = nt->FileHeader.TimeDateStamp;
		g_wtImageSize = size;
		if (size == kEnhancedImageSize && stamp == kEnhancedStamp)
			g_wtExe = WtExe::Enhanced;
		else if (size == kRetailImageSize && stamp == kRetailStamp)
			g_wtExe = WtExe::Retail;
		else
			g_wtExe = WtExe::Unknown;
		return g_wtExe;
	}
}
