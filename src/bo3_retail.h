// The retail exe's addresses (BlackOps3.exe CL 13892626, SizeOfImage 0x1D74B000, TimeDateStamp 0x693D731E).
//
// Mapped 2026-10-03 from a runtime dump of the Arxan-decrypted image against the
// Enhanced IDB; per-item evidence and register contracts are in the author's mapping notes.
// Each header keeps its Enhanced values as the default; ApplyRetailAddresses() overwrites them once, before anything is
// installed. Everything here is still checked against the code at install time (the same shape checks as on Enhanced,
// with retail's bytes where the compile differs), so a wrong value fails closed with a log line.
#pragma once

namespace
{
	void ApplyRetailAddresses()
	{
		// ---- core (bo3_additive.h) --------------------------------------------------------------------------------------
		kUpdateViewWeaponAnim = 0x45D480;  // obfuscated state machine at the top, as on Enhanced: never wrap it
		kAfterUpdateCall = 0x44B460;       // call at 0x44B45B in CG_UpdateViewModelAnims (0x449B40)
		kSetGoalWeight = 0x22CCBC0;
		kGetInfoIndex = 0x22C9340;         // (tree, node) wrapper over the (node, root info) worker 0x22C9360
		kXAnimInfo = 0x17DF81E0;
		kSyncTime = 0x22CD5B0;
		kWeaponVariants = 0x19BF4310;
		kVariantCount = 0x19BF5314;        // NOT table-0x10 on retail: table+0x1004
		kSLTable = 0x505D2D0;
		kClipSlotWeapon = 0x266E6F0;
		kClipEmpty = 0x266F2F0;
		kMeleeOwnGun = 0x2677E37;          // in BG_GetMeleeWeapon 0x2677DE0
		{
			static const uint8_t ctx[] = {0x83, 0xB8, 0xCC, 0x15, 0x00, 0x00, 0x00};  // cmp dword [rax+15CCh], 0
			memcpy(kMeleeCtx, ctx, sizeof(ctx));
			kMeleeJzRel = 0xC5;
		}
		kEmptyRaiseCall = 0x2666520;
		kEmptyDropCall = 0x265BA60;
		kDObjGetBoneIndex = 0x22C4900;
		kSLGetString = 0x12D7B40;          // (text, user, type): SlString passes type 3
		kXAnimInfoRef = 0x22CCC44;         // lea r9, XAnimInfo (in XAnimSetGoalWeight)
		kSLTableRef = 0x12D7184;           // mov rax, [SL table]
		kVariantCountRef = 0x267185A;      // mov edx, [variant count] in the registration block
		kGetViewmodelWeapon = 0x2657E30;
		kGetViewmodelWeaponCall = 0x45EE32;

		// ---- view / gun / fire / kick return (bo3_additive.h) -----------------------------------------------------------
		kCallCalcWeaponPos = 0x117C290;    // in CG_AddViewWeapon 0x117A710; (cg, placement, ang) as on Enhanced
		kCalcWeaponPos = 0x1199C80;
		kCallViewAxis[0] = 0x10AE259;      // retail has ONE AnglesToAxis call in CG_CalcViewValues (both paths reach it)
		kCallViewAxis[1] = 0;
		kCallViewAxisCount = 1;
		kCallFireRecoil = 0x11BBBD1;       // in CG_FireWeapon 0x11B9F40
		kFireRecoil = 0x265A070;
		kAnglesToAxis = 0x224ECE0;
		kAxisToAngles = 0x2248E40;
		kKickCopy = 0x10C170B;             // call setter 0x132DED0 (getter + the three kickAngles stores; returns clientActive)
		kKickSetter = 0x132DED0;
		kClientActiveGetter = 0x71BD0;     // obfuscated: only checked (inside the setter), never called or wrapped
		kCallAfterKickCopy = 0x10C1729;    // -> 0x11D7520 (cg, ps, ..); rax = clientActive, rcx = cg
		kAfterKickCopyTarget = 0x11D7520;
		kKickAdsScaleCheck = 0x2659EA9;    // in helper 0x2659EA0(ps)
		kGetWeaponDef = 0x2671510;
		kFinishMoveAngles = 0x134075F;     // the same bytes as Enhanced
		kTwinGetterCall = 0x612FDD;        // the twin kickAngles copy in 0x6114A0: call setter ... call 0x11D7520
		kTwinCopyCall = 0x612FFF;
		kTwinCopyTarget = 0x11D7520;

		// ---- perf: registration + tree build (bo3_perf.h) ----------------------------------------------------------------
		kRegisterBlock = 0x267185A;
		kRegisterCall = 0x2671872;
		kSetupVariant = 0x26726A0;
		kTreeBuild = 0x444C80;
		kTreeBuildInner = 0x444BB0;
		kTreeBuildCalls[0] = 0x119BF71;    // weapon change
		kTreeBuildCalls[1] = 0x1281623;    // respawn
		kGetVariantDef = 0x26719A0;

		// ---- slots (bo3_slots.h) -----------------------------------------------------------------------------------------
		kSlotGroupTable = 0x2F289E0;
		kSlotGroupLoopEnd = 0x444B7B;      // in 0x444AB0 (retail split the group loop out of the inner build)
		kSlotGroupLoopEndField = 0xC;
		kSlotBaseLoop = 0x444C17;
		kSlotRootInit = 0x444BED;
		kSlotRootInitFn = 0x22C66D0;
		kSlotCreateStores = 0x22C7E0B;     // in XAnimCreate 0x22C7DE0
		kSlotEnsureInfo = 0x22C63AB;

		// ---- inspect + akimbo + HUD (bo3_inspect.h, bo3_inspect_hud.h) ---------------------------------------------------
		kInsSite = 0x44B446;               // 21 bytes, then the call at 0x44B45B
		kInsCallUpdate = 0x44B45B;
		kInsKeyCatchers = 0x5359BC4;
		kInsKeyCatchersRef = 0x133D333;
		kInsIsDualWield = 0x267B1B0;
		kInsDualWieldWeapon = 0x2676960;
		kInsAkLastAnim = 0x460A71;
		kInsAkCmpAnim = 0x460A78;
		kInsAkSetNode = 0x460AE9;
		kInsAkIdle = 0x459C86;             // in the left idle function 0x459C30 (+0x12: the 0A8h store)
		kInsAkLeftState = 0x2658C91;
		kInsAkIsDualCall = 0x460A90;
		kInsAkDualCall = 0x459C54;
		kInsAkClipCall = 0x459C5F;         // -> clip-ammo helper 0x266DCA0, which calls the clip weapon fn at +0xC
		kInsAkClipAmmo = 0x266DCA0;
		kInsHudRoots = 0x1626C03C;
		kInsHudRootsAccessor = 0x200D5A0;
		kInsHudCreateModel = 0x200CF00;    // (parent, path): the two extra arguments are ignored
		kInsHudSetInt = 0x200DC50;

		// ---- interrupts + interrupt_empty_melee (bo3_interrupt.h) ---------------------------------------------------------
		kIntPmWeapon = 0x265FDE0;
		kIntPmWeaponSites[0] = 0x265FD6E;  // in 0x265FD50: hand 0
		kIntPmWeaponSites[1] = 0x265FD7F;  //                hand 1
		kIntAnimToNode = 0x267BA50;        // takes a 5th (byte) argument, passed as 0
		kIntNodeTimes = 0x325F550;         // all 197 entries identical to Enhanced's
		kIntNode41TimeFn = 0x2678CB0;
		kIntIsSegmented = 0x267B6B0;
		kIntPostMeleeEmptyCall = 0x2664935;  // in PM_Weapon_CheckForReload 0x26648A0

		// ---- segmented reloads (bo3_segreload.h) ---------------------------------------------------------------------------
		kSrBeginReload = 0x265BD80;
		kSrLoopBegin = 0x265D820;          // reads its first argument (pm) on retail
		kSrLoopDone = 0x2665C80;
		kSrStartDone = 0x2665F80;
		kSrReloadEmptyTime = 0x2678CB0;
		kSrIsSegmented = 0x267B6B0;
		kSrBeginSites[0] = {0x2664B98, kSrBeginReload};
		kSrBeginSites[1] = {0x266279F, kSrBeginReload};
		kSrLoopSites[0] = {0x265C0A2, kSrLoopBegin};
		kSrLoopSites[1] = {0x2666043, kSrLoopBegin};
		kSrLoopSites[2] = {0x2665EE2, kSrLoopBegin};
		kSrLoopDoneSites[0] = {0x2660284, kSrLoopDone};
		kSrLoopDoneSites[1] = {0x266102D, kSrLoopDone};
		kSrStartDoneSites[0] = {0x2660271, kSrStartDone};

		// ---- IK (bo3_ik.h; its DObj anchors are a retail table in InstallIk) ------------------------------------------------
		kDObjCalcSkel = 0x22C0B40;
		kCallSkelCalcBone = 0x1ABB5C;
		kCallSkelCalcPose = 0x99DA09;

		// ---- slide (bo3_slide.h) ------------------------------------------------------------------------------------------
		kSlImageSize = kRetailImageSize;
		kSlUpdate = 0x1692C0;
		kSlEndCheck = 0x168F50;
		kSlCanSlide = 0x1683A0;
		kSlCheckJump = 0x25FD4D0;
		kSlFriction = 0x2613C00;
		kSlAccelerate = 0x260F390;
		kSlCmdScale = 0x2611A90;
		kSlSprintOk = 0x2616520;
		kSlStepSlide = 0x26210E0;
		kSlMoveScale = 0x26734A0;
		const uintptr_t slideCalls[] = {0x261FB37, 0x169300, 0x169342, 0x1692F1, 0x261AA72, 0x2615031,
		                                0x261AAED, 0x261AD90, 0x2611D4F, 0x2618447, 0x261AE33};
		const uintptr_t slideTargets[] = {kSlUpdate, kSlEndCheck, kSlCanSlide, kSlCheckJump, kSlCheckJump, kSlCheckJump,
		                                  kSlFriction, kSlAccelerate, kSlCmdScale, kSlSprintOk, kSlStepSlide};
		static_assert(std::size(slideCalls) == std::size(kSlSites), "one retail call site per Enhanced one");
		for (size_t i = 0; i < std::size(kSlSites); i++)
		{
			kSlSites[i].call = slideCalls[i];
			kSlSites[i].target = slideTargets[i];
		}
	}
}
