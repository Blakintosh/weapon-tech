// Viewmodel FOV pin (opt-in, global, off unless vmfov= says otherwise). Included by weapon_tech.h after bo3_additive.h.
//
// What BO3 does (Enhanced exe; research: xpakcap\vmfov\):
//   * One projection for the whole frame. CG_CalcFov 0x1405AFA30 (wrapper 0x1405B6690) turns the FOV into a lens
//     (focal length = 7 / (0.75 tan(fov/2)), clamped by the lens table, focus breathing) and writes the refdef
//     (cg+0x131CF0): +0x78 tanHalfFovX, +0x7C tanHalfFovY (+0x80 the same), +0x84 the final FOV in degrees (cg_fov's
//     convention, below). R_SetupViewParms 0x141DE3510 builds the one projection from +0x78/+0x7C. The viewmodel is
//     drawn in the depth-hack draw lists with that projection; the depth hack only changes depth (zNear code
//     constant: viewmodel near 0.1, and adsZScale = min(1, cg_adsZScaleMax - adsFrac^2), a depth squeeze). There is no
//     weapon / viewmodel FOV dvar, and no second projection for the gun.
//   * The only FOV handling for the gun is in CG_CalculateWeaponPosition 0x14126D750: at hip it pulls the gun toward
//     the eye by 2 * clamp((cg_fov - 65) * 0.05, 0, 1) * (1 - adsFrac) units (0 at 65, 2 at 85 and above).
//
// What MW2019 does (IW8 Xbox build with PDB, cg_view.cpp): the gun has its own projection (refdef depthHackFov) only
// when cg_fov_viewmodel >= 1 (default 0, so off) or the weapon has its own zoomSettings.weapon.adsZoomFov; otherwise
// the gun is drawn at the world FOV. At hip it pulls the gun back by cg_gun_fovcomp_x (-6) times
// clamp((clamp(worldFov, 70, 85) - 70) / 15) * (1 - adsFrac) (CG_View_CalcFovCompensation /
// CG_CalculateWeaponMovement_FovCompensation). All IW8 FOVs are horizontal at 4:3, Hor+ (TanHalfAngles:
// tanY = 0.75 tan(fov/2), tanX = tanY * aspect), the same convention as BO3's cg_fov.
//
// What this does: BO3 can't draw the gun with its own projection, so the pin is a placement compensation: the gun is
// moved along the view axis (CG_CalculateWeaponPosition's view-space offset, forward) so that at a reference depth
// (vmfov_depth) it has the on-screen size it would have at the pinned FOV:
//     shift = (tan(vmfov/2) / tan(worldFov/2) - 1) * vmfov_depth,   clamped to +-vmfov_max
// (negative = toward the eye). A pure view-axis move keeps everything on the view axis on the view axis, so sights
// still sit on the crosshair at full ADS. It is exact at vmfov_depth only: the parts of the gun nearer than that get a
// little less of the change than a real projection would give them, the parts farther away a little more. BO3's own
// pull (above) is taken out, so the result replaces it. MW2019's own pull equals this at depth ~19.7 (vmfov 65, world
// 85: (0.637 / 0.916 - 1) * 19.7 = -6), hence the default depth of 20 and the default limit of 6.
// The world FOV is the one the frame is drawn with (refdef +0x84, degrees, after the lens), latched at hip (adsFrac 0),
// so the ADS zoom never feeds the shift.
//
// Config (weapon_tech.cfg; all live while the game runs, but the hook is only installed when vmfov is on at load):
//   vmfov=off|mw|<degrees>   default off: nothing installed, nothing changes.
//                            <degrees>: pin the gun at this FOV, in cg_fov's convention: HORIZONTAL degrees AT 4:3
//                              (Hor+; on 16:9 that is 2*atan(tan(v/2) * 4/3), 65 -> 80.7 horizontal, 51.1 vertical).
//                              65 = BO3's and MW2019's base FOV (what the viewmodels are authored for).
//                            mw: MW2019's default: the gun at the world FOV with IW8's hip pull,
//                              -6 * clamp((worldFov - 70) / 15, 0, 1) * (1 - adsFrac), in place of BO3's.
//   vmfov_ads=fade|hold      fade (default, MW2019 / BO3 behaviour): the shift fades out with the ADS fraction, so at
//                            full ADS the gun is exactly the stock one (zooms with the world, optics as authored).
//                            hold: the hip shift is kept through ADS (the gun keeps its hip distance from the eye).
//   vmfov_depth=<units>      reference depth for <degrees> (default 20)
//   vmfov_max=<units>        largest shift either way (default 6, MW2019's largest pull)
//   vmfov_debug=0|1          1 Hz log line: world FOV (now / hip), ADS fraction, shift
#pragma once
#include <cmath>
#include <cstring>
#include <cstdint>

namespace
{
	// ---- Enhanced addresses (RVAs, checked < image size 0x1A53F000) ----------------------------------------------
	uintptr_t &kVmCallCalcWeaponPos = kCallCalcWeaponPos;  // CG_AddViewWeapon: E8 call CG_CalculateWeaponPosition
	uintptr_t &kVmCalcWeaponPos = kCalcWeaponPos;      // CG_CalculateWeaponPosition(cg*, placement*, float ang[3])
	// cg_t offsets (cg = the hook's first argument)
	constexpr size_t kVmCgRefdefFov = 0x131D74;            // refdef (cg+0x131CF0) +0x84: final FOV, degrees, 4:3-horizontal
	constexpr size_t kVmCgAdsFrac = 0x11A8B0 + 0x2FC;      // predicted playerState + 0x2FC: ADS fraction
	// placement: +0x10 view-space offset (forward, left, up) before CG_CalculateWeaponPosition moves it to world
	constexpr int kVmPlacementForward = 4;

	enum VmMode : int { kVmOff = 0, kVmMw = 1, kVmPin = 2 };
	struct VmFovConfig
	{
		int mode = kVmOff;
		float fov = 65.0f;     // kVmPin: degrees, 4:3-horizontal
		bool hold = false;     // vmfov_ads=hold
		float depth = 20.0f;   // vmfov_depth
		float maxShift = 6.0f; // vmfov_max
		bool debug = false;
	};
	VmFovConfig g_vm;          // the one the hook reads (a reload replaces it as a whole)
	bool g_vmHooked;
	using VmCalcWeaponPosFn = void (*)(void *cg, float *placement, float *ang);
	VmCalcWeaponPosFn g_vmNext;  // what the call went to before us: D750, or bo3_additive.h's gun hook stub

	const char *VmModeText(const VmFovConfig &c, char *buf, size_t n)
	{
		if (c.mode == kVmPin)
			snprintf(buf, n, "%.1f deg (4:3-horizontal; %.1f horizontal at 16:9)", c.fov,
			         2.0f * atanf(tanf(c.fov * 0.00872664626f) * (4.0f / 3.0f)) * 57.2957795f);
		else
			snprintf(buf, n, "%s", c.mode == kVmMw ? "mw (world FOV + MW2019 hip pull)" : "off");
		return buf;
	}

	// One vmfov* line into `c`. known = whether it is a vmfov key; false return = a vmfov key it can't read.
	bool ParseVmFovInto(const char *line, VmFovConfig &c, bool &known)
	{
		known = strncmp(line, "vmfov", 5) == 0;
		if (!known)
			return true;
		char word[16] = {};
		float f;
		int v;
		if (strncmp(line, "vmfov=", 6) == 0)
		{
			const char *s = line + 6;
			if (strncmp(s, "off", 3) == 0 || (s[0] == '0' && (s[1] < '0' || s[1] > '9') && s[1] != '.'))
				return c.mode = kVmOff, true;
			if (strncmp(s, "mw", 2) == 0)
				return c.mode = kVmMw, true;
			if (sscanf_s(s, "%f", &f) != 1 || f < 20.0f || f > 150.0f)
				return false;
			c.mode = kVmPin;
			c.fov = f;
			return true;
		}
		if (strncmp(line, "vmfov_ads=", 10) == 0)
		{
			if (sscanf_s(line + 10, "%15[a-z]", word, static_cast<unsigned>(sizeof(word))) != 1)
				return false;
			if (strcmp(word, "fade") == 0)
				return c.hold = false, true;
			if (strcmp(word, "hold") == 0)
				return c.hold = true, true;
			return false;
		}
		if (strncmp(line, "vmfov_depth=", 12) == 0)
			return sscanf_s(line + 12, "%f", &f) == 1 && f > 0.5f && f < 200.0f && ((c.depth = f), true);
		if (strncmp(line, "vmfov_max=", 10) == 0)
			return sscanf_s(line + 10, "%f", &f) == 1 && f >= 0.0f && f <= 30.0f && ((c.maxShift = f), true);
		if (strncmp(line, "vmfov_debug=", 12) == 0)
			return sscanf_s(line + 12, "%d", &v) == 1 && ((c.debug = v != 0), true);
		return false;  // vmfov_<unknown>
	}

	// Load-time parse (weapon_tech.h ParseWeaponTechText), one line at a time.
	bool ParseVmFovLine(const char *line, bool &known) { return ParseVmFovInto(line, g_vm, known); }

	// Live reload (bo3_additive.h ApplyTuningText): every vmfov line again from the whole text; a removed line is its
	// default again (so a removed vmfov= is off).
	void VmFovReloadText(const char *text)
	{
		VmFovConfig c;
		char line[256];
		int lines = 0;
		for (const char *p = text; (p = NextCfgLine(p, line, sizeof(line))) != nullptr;)
		{
			line[strcspn(line, "\r\n")] = 0;
			bool known;
			if (!ParseVmFovInto(line, c, known))
				Log("vmfov: live reload: bad line '%s'", line);
			lines += known;
		}
		if (c.mode == g_vm.mode && c.fov == g_vm.fov && c.hold == g_vm.hold && c.depth == g_vm.depth &&
		    c.maxShift == g_vm.maxShift && c.debug == g_vm.debug)
			return;
		char buf[96];
		if (!g_vmHooked)
		{
			if (c.mode != kVmOff)
				Log("vmfov: live: %s asked for, but vmfov was off at load, so the hook isn't in; restart the game", VmModeText(c, buf, sizeof(buf)));
			g_vm = c;
			return;
		}
		g_vm = c;
		Log("vmfov: live: %s, ads %s, depth %.1f, max %.1f, debug %d (%d line(s))", VmModeText(c, buf, sizeof(buf)),
		    c.hold ? "hold" : "fade", c.depth, c.maxShift, c.debug, lines);
	}

	inline float VmClamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

	// The view-space forward offset for this frame (units, negative = toward the eye), BO3's own pull taken out.
	float VmFovShift(const VmFovConfig &c, float hipFov, float ads)
	{
		// BO3's pull, as CG_CalculateWeaponPosition adds it (it reads the cg_fov dvar; the hip refdef FOV stands in for
		// it here: the dvar's value is encrypted and its getter is control-flow obfuscated, so neither is touched)
		const float bo3 = -2.0f * VmClamp((hipFov - 65.0f) * 0.05f, 0.0f, 1.0f) * (1.0f - ads);
		float want;
		if (c.mode == kVmMw)
			want = -6.0f * VmClamp((hipFov - 70.0f) / 15.0f, 0.0f, 1.0f);
		else
		{
			const float tw = tanf(hipFov * 0.00872664626f), tv = tanf(c.fov * 0.00872664626f);
			want = tw > 1e-4f ? (tv / tw - 1.0f) * c.depth : 0.0f;
		}
		want = VmClamp(want, -c.maxShift, c.maxShift);
		if (!c.hold)
			want *= 1.0f - ads;
		return want - bo3;
	}

	void VmFovHook(void *cg, float *placement, float *ang)
	{
		static const void *s_cgChecked;
		static bool s_cgOk;
		static float s_hipFov;  // the world FOV last seen at hip
		static double s_nextLog;
		const VmFovConfig c = g_vm;
		if (c.mode != kVmOff && cg)
		{
			const uint8_t *p = static_cast<const uint8_t *>(cg);
			if (cg != s_cgChecked)  // a new cg: check the two fields once
			{
				s_cgChecked = cg;
				s_cgOk = FastReadable(p + kVmCgRefdefFov, 4) && FastReadable(p + kVmCgAdsFrac, 4);
				if (!s_cgOk)
					Log("vmfov: cg %p: refdef / playerState not readable; no shift for this cg", cg);
			}
			if (s_cgOk)
			{
				const float fov = *reinterpret_cast<const float *>(p + kVmCgRefdefFov);
				const float ads = VmClamp(*reinterpret_cast<const float *>(p + kVmCgAdsFrac), 0.0f, 1.0f);
				if (fov > 1.0f && fov < 179.0f && ads <= 0.0f)
					s_hipFov = fov;
				if (s_hipFov > 0.0f)
				{
					const float shift = VmFovShift(c, s_hipFov, ads);
					placement[kVmPlacementForward] += shift;
					if (c.debug)
					{
						const double now = NowSeconds();
						if (now >= s_nextLog)
						{
							s_nextLog = now + 1.0;
							Log("vmfov: world fov %.2f (hip %.2f) ads %.2f -> forward %+.2f (BO3's own pull taken out)", fov, s_hipFov,
							    ads, shift);
						}
					}
				}
			}
		}
		g_vmNext(cg, placement, ang);
	}

	// Redirects the CG_CalculateWeaponPosition call in CG_AddViewWeapon. It may already go to bo3_additive.h's gun hook
	// (its jmp [rip] stub to CalcWeaponPosHook): then this chains in front of it (both only add view-space offsets, so
	// the order doesn't matter). Anything else at the site: not installed.
	void InstallVmFov()
	{
		if (WtDebugSkip("vmfov"))
			return;
		char buf[96];
		if (g_vmHooked)
			return;
		if (g_vm.mode == kVmOff)
		{
			Log("vmfov: off (vmfov=off or no vmfov line); not installed");
			return;
		}
		if (!WtExeSupported())
		{
			Log("vmfov: unknown exe; not installed");
			return;
		}
		uint8_t *site = At<uint8_t>(kVmCallCalcWeaponPos);
		if (!FastReadable(site, 5) || site[0] != 0xE8)
		{
			Log("vmfov: +%zx isn't a call; not installed", kVmCallCalcWeaponPos);
			return;
		}
		int32_t rel;
		memcpy(&rel, site + 1, 4);
		uint8_t *target = site + 5 + rel;
		bool ours = false;  // bo3_additive.h's stub: FF 25 00000000 <&CalcWeaponPosHook>
		if (target != At<uint8_t>(kVmCalcWeaponPos))
		{
			void *dest = nullptr;
			if (FastReadable(target, 14) && target[0] == 0xFF && target[1] == 0x25 && !memcmp(target + 2, "\0\0\0\0", 4))
				memcpy(&dest, target + 6, 8);
			ours = dest == reinterpret_cast<void *>(&CalcWeaponPosHook);
			if (!ours)
			{
				Log("vmfov: +%zx calls +%zx, neither CG_CalculateWeaponPosition nor the gun hook; not installed",
				    kVmCallCalcWeaponPos, static_cast<size_t>(target - g_base));
				return;
			}
		}
		auto *stub = static_cast<uint8_t *>(AllocNear(reinterpret_cast<uintptr_t>(site), 0x1000));
		if (!stub)
		{
			Log("vmfov: no memory near the exe; not installed");
			return;
		}
		void *fn = reinterpret_cast<void *>(&VmFovHook);
		stub[0] = 0xFF;  // jmp [rip]
		stub[1] = 0x25;
		memset(stub + 2, 0, 4);
		memcpy(stub + 6, &fn, 8);
		FlushInstructionCache(GetCurrentProcess(), stub, 14);
		g_vmNext = reinterpret_cast<VmCalcWeaponPosFn>(target);
		const int32_t newRel = static_cast<int32_t>(stub - (site + 5));
		DWORD old;
		VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &old);
		memcpy(site + 1, &newRel, 4);
		VirtualProtect(site + 1, 4, old, &old);
		FlushInstructionCache(GetCurrentProcess(), site, 5);
		g_vmHooked = true;
		Log("vmfov: hooked (%s); %s, ads %s, depth %.1f, max %.1f, debug %d", ours ? "in front of the gun hook" : "own call redirect",
		    VmModeText(g_vm, buf, sizeof(buf)), g_vm.hold ? "hold" : "fade", g_vm.depth, g_vm.maxShift, g_vm.debug);
	}
}
