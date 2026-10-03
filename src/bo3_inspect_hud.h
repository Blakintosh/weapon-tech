// Inspect -> LUI: publishes the inspect state as UI models so the map's HUD can react (bo3_inspect.h calls this).
//
//   hudItems.weaponTech.inspecting      int, 1 while an inspect runs on this local client, else 0
//   hudItems.weaponTech.inspectHideHud  int, the cfg's inspect_hidehud (0/1)
//
// Karelia: WeaponReticle hides the crosshair while inspecting is 1; the HUD fades out while both are 1.
//
// Thread: InspectBeforeAnim runs inside CG_UpdateViewModelAnims 0x1404F1FA0. T7Overcharged's reticle_models writes
// its hudItems.reticle models from CG_ShouldDrawCrosshair inside CG_DrawCrosshair 0x14077AE70. Callers (IDA xrefs):
// CG_DrawCrosshair <- 0x1406E63C0 <- 0x1411C01A0; CG_UpdateViewModelAnims <- 0x1411C01A0 directly (and <- 0x1411D4950,
// the other CG_DrawActiveFrame); 0x1411C01A0 and 0x1411D4950 are both called by 0x141485BF0, which goes up to WinMain.
// A call can't change threads, so the models are written on the thread T7Overcharged writes its own on. The first write logs its thread next to the thread init() was called on
// from Lua (the LUI thread), as a runtime check.
//
// Writes happen only when a value changes, or when the controller's root model changes (map load / UI reset), when
// both are written again. Enhanced only (addresses from the Enhanced IDB, same as reticle_models.cpp).
#pragma once

namespace
{
	// Enhanced values; bo3_retail.h sets retail's (there the table is only reached through a tiny accessor function).
	uintptr_t kInsHudRoots = 0x186CD218;       // u16 root model per controller (VA 0x1586CD218 - 0x140000000)
	uintptr_t kInsHudCreateModel = 0x21348E0;  // CreateModelFromPath(parent, path, 1, 0) -> u16 (retail's takes 2 args)
	uintptr_t kInsHudSetInt = 0x21353A0;       // SetInt(u16 model, int)
	uintptr_t kInsHudRootsRef[2] = {0x1C455F, 0x1FB6AE};  // Enhanced: movzx r32, word [base+idx*2+rva]
	uintptr_t kInsHudRootsAccessor = 0;        // retail: movsxd rax,ecx; lea rcx,[table]; movzx eax,word [rcx+rax*2]; ret

	struct InspectHudModels
	{
		uint16_t root, inspecting, hideHud;
		int lastInspecting = -1, lastHideHud = -1;
	} g_insHud[4];
	DWORD g_insHudLuaThread;  // the thread init() ran on (Lua)
	bool g_insHudThreadLogged;

	void InspectHudNoteLuaThread() { g_insHudLuaThread = GetCurrentThreadId(); }

	// Cheap enough for every frame: one u16 read, two compares. lc indexes the controller roots (lc == controller
	// outside splitscreen, as in reticle_models, which uses controller 0).
	void InspectHudSync(int lc, bool inspecting, bool hideHud)
	{
		if (lc < 0 || lc >= 4)
			return;
		static int s_ok = -1;  // a wrong table address faults here on every frame: check it once
		if (s_ok < 0)
		{
			// Readable isn't enough (any .data address passes; a VA mistaken for an RVA got through that way): the constant
			// must also be the one the engine's own `movzx r32, word [base+idx*2+rva]` at 0x1401C455F / 0x1401FB6AE use.
			const bool readable = FastReadable(At<uint8_t>(kInsHudRoots), 8);
			bool tied;
			if (IsRetailExe())
			{
				static const uint8_t kHead[] = {0x48, 0x63, 0xC1, 0x48, 0x8D, 0x0D}, kTail[] = {0x0F, 0xB7, 0x04, 0x41, 0xC3};
				const uint8_t *a = At<uint8_t>(kInsHudRootsAccessor);
				tied = kInsHudRootsAccessor && FastReadable(a, 16) && memcmp(a, kHead, sizeof(kHead)) == 0 &&
				       CodeRefers(kInsHudRootsAccessor + 3, 7, kInsHudRoots) && memcmp(a + 10, kTail, sizeof(kTail)) == 0;
			}
			else
				tied = CodeRefers(kInsHudRootsRef[0], 9, kInsHudRoots) && CodeRefers(kInsHudRootsRef[1], 9, kInsHudRoots);
			s_ok = readable && tied ? 1 : 0;
			if (!s_ok)
				Log("inspect hud: the controller root table at +%zx %s; HUD/crosshair hiding OFF", kInsHudRoots,
				    readable ? "doesn't match the code" : "isn't readable");
			else
				Log("inspect hud: controller root table +%zx matches the code", kInsHudRoots);
		}
		if (!s_ok)
			return;
		const uint16_t root = At<uint16_t>(kInsHudRoots)[lc];
		if (!root)
			return;
		InspectHudModels &m = g_insHud[lc];
		using create_t = uint16_t(__fastcall *)(uint16_t, const char *, bool, int);
		using set_int_t = bool(__fastcall *)(uint16_t, int);
		const auto setInt = reinterpret_cast<set_int_t>(At<uint8_t>(kInsHudSetInt));
		if (root != m.root)
		{
			const auto create = reinterpret_cast<create_t>(At<uint8_t>(kInsHudCreateModel));
			m.root = root;
			m.inspecting = create(root, "hudItems.weaponTech.inspecting", true, 0);
			m.hideHud = create(root, "hudItems.weaponTech.inspectHideHud", true, 0);
			m.lastInspecting = m.lastHideHud = -1;
			if (!g_insHudThreadLogged)
			{
				g_insHudThreadLogged = true;
				const DWORD tid = GetCurrentThreadId();
				Log("inspect hud: models under root %u on thread %lu; Lua init() thread %lu%s", root, tid,
				    g_insHudLuaThread, tid == g_insHudLuaThread ? " (same thread)" : " (DIFFERENT thread)");
			}
			else
				HotLog("inspect hud: root model changed to %u; models rebuilt", root);
		}
		if (!m.inspecting || !m.hideHud)
			return;
		if (m.lastHideHud != static_cast<int>(hideHud))
		{
			m.lastHideHud = hideHud;
			setInt(m.hideHud, hideHud ? 1 : 0);
		}
		if (m.lastInspecting != static_cast<int>(inspecting))
		{
			m.lastInspecting = inspecting;
			setInt(m.inspecting, inspecting ? 1 : 0);
			HotLog("inspect hud: inspecting = %d", inspecting ? 1 : 0);
		}
	}
}
