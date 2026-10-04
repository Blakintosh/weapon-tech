// The retail exe's addresses (BlackOps3.exe Steam build 24784313, SizeOfImage 0x1D75BC00, TimeDateStamp 0x6A7B6355).
//
// Mapped 2026-10-03 from a runtime dump of the Arxan-decrypted image against the Enhanced IDB (for the previous retail
// exe, 0x1D74B000 / 0x693D731E); per-item evidence and register contracts are in the author's mapping notes. Ported
// 2026-10-04 to the current exe by diffing the two decrypted retail dumps function by function: 97% of functions are
// identical apart from relocated operands, and everything here moved by +0 or -0x6C0 (each one checked in the dump).
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
		kSetGoalWeight = 0x22CC500;
		kGetInfoIndex = 0x22C8C80;         // (tree, node) wrapper over the (node, root info) worker 0x22C8CA0
		kXAnimInfo = 0x17DF81E0;
		kSyncTime = 0x22CCEF0;
		kWeaponVariants = 0x19BF4310;
		kVariantCount = 0x19BF5314;        // NOT table-0x10 on retail: table+0x1004
		kSLTable = 0x505D2D0;
		kClipSlotWeapon = 0x266E030;
		kClipEmpty = 0x266EC30;
		kMeleeOwnGun = 0x2677777;          // in BG_GetMeleeWeapon 0x2677720
		{
			static const uint8_t ctx[] = {0x83, 0xB8, 0xCC, 0x15, 0x00, 0x00, 0x00};  // cmp dword [rax+15CCh], 0
			memcpy(kMeleeCtx, ctx, sizeof(ctx));
			kMeleeJzRel = 0xC5;
		}
		kEmptyRaiseCall = 0x2665E60;
		kEmptyDropCall = 0x265B3A0;
		kDObjGetBoneIndex = 0x22C4240;
		kSLGetString = 0x12D7B40;          // (text, user, type): SlString passes type 3
		kXAnimInfoRef = 0x22CC584;         // lea r9, XAnimInfo (in XAnimSetGoalWeight)
		kSLTableRef = 0x12D7184;           // mov rax, [SL table]
		kVariantCountRef = 0x267119A;      // mov edx, [variant count] in the registration block
		kGetViewmodelWeapon = 0x2657770;
		kGetViewmodelWeaponCall = 0x45EE32;

		// ---- view / gun / fire / kick return (bo3_additive.h) -----------------------------------------------------------
		kCallCalcWeaponPos = 0x117C290;    // in CG_AddViewWeapon 0x117A710; (cg, placement, ang) as on Enhanced
		kCalcWeaponPos = 0x1199C80;
		kCallViewAxis[0] = 0x10AE259;      // retail has ONE AnglesToAxis call in CG_CalcViewValues (both paths reach it)
		kCallViewAxis[1] = 0;
		kCallViewAxisCount = 1;
		kCallFireRecoil = 0x11BBBD1;       // in CG_FireWeapon 0x11B9F40
		kFireRecoil = 0x26599B0;
		kAnglesToAxis = 0x224E620;
		kAxisToAngles = 0x2248780;
		kKickCopy = 0x10C170B;             // call setter 0x132DED0 (getter + the three kickAngles stores; returns clientActive)
		kKickSetter = 0x132DED0;
		kClientActiveGetter = 0x71BD0;     // obfuscated: only checked (inside the setter), never called or wrapped
		kCallAfterKickCopy = 0x10C1729;    // -> 0x11D7520 (cg, ps, ..); rax = clientActive, rcx = cg
		kAfterKickCopyTarget = 0x11D7520;
		kKickAdsScaleCheck = 0x26597E9;    // in helper 0x26597E0(ps)
		kGetWeaponDef = 0x2670E50;
		kFinishMoveAngles = 0x134075F;     // the same bytes as Enhanced
		kTwinGetterCall = 0x612FDD;        // the twin kickAngles copy in 0x6114A0: call setter ... call 0x11D7520
		kTwinCopyCall = 0x612FFF;
		kTwinCopyTarget = 0x11D7520;

		// ---- perf: registration + tree build (bo3_perf.h) ----------------------------------------------------------------
		kRegisterBlock = 0x267119A;
		kRegisterCall = 0x26711B2;
		kSetupVariant = 0x2671FE0;
		kTreeBuild = 0x444C80;
		kTreeBuildInner = 0x444BB0;
		kTreeBuildCalls[0] = 0x119BF71;    // weapon change
		kTreeBuildCalls[1] = 0x1281623;    // respawn
		kGetVariantDef = 0x26712E0;

		// ---- slots (bo3_slots.h) -----------------------------------------------------------------------------------------
		kSlotGroupTable = 0x2F289E0;
		kSlotGroupLoopEnd = 0x444B7B;      // in 0x444AB0 (retail split the group loop out of the inner build)
		kSlotGroupLoopEndField = 0xC;
		kSlotBaseLoop = 0x444C17;
		kSlotRootInit = 0x444BED;
		kSlotRootInitFn = 0x22C6010;
		kSlotCreateStores = 0x22C774B;     // in XAnimCreate 0x22C7720
		kSlotEnsureInfo = 0x22C5CEB;

		// ---- inspect + akimbo + HUD (bo3_inspect.h, bo3_inspect_hud.h) ---------------------------------------------------
		kInsSite = 0x44B446;               // 21 bytes, then the call at 0x44B45B
		kInsCallUpdate = 0x44B45B;
		kInsKeyCatchers = 0x5359BC4;
		kInsKeyCatchersRef = 0x133D333;
		kInsIsDualWield = 0x267AAF0;
		kInsDualWieldWeapon = 0x26762A0;
		kInsAkLastAnim = 0x460A71;
		kInsAkCmpAnim = 0x460A78;
		kInsAkSetNode = 0x460AE9;
		kInsAkIdle = 0x459C86;             // in the left idle function 0x459C30 (+0x12: the 0A8h store)
		kInsAkLeftState = 0x26585D1;
		kInsAkIsDualCall = 0x460A90;
		kInsAkDualCall = 0x459C54;
		kInsAkClipCall = 0x459C5F;         // -> clip-ammo helper 0x266D5E0, which calls the clip weapon fn at +0xC
		kInsAkClipAmmo = 0x266D5E0;
		kInsHudRoots = 0x1626C03C;
		kInsHudRootsAccessor = 0x200CEE0;
		kInsHudCreateModel = 0x200C840;    // (parent, path): the two extra arguments are ignored
		kInsHudSetInt = 0x200D590;

		// ---- interrupts + interrupt_empty_melee (bo3_interrupt.h) ---------------------------------------------------------
		kIntPmWeapon = 0x265F720;
		kIntPmWeaponSites[0] = 0x265F6AE;  // in 0x265F690: hand 0
		kIntPmWeaponSites[1] = 0x265F6BF;  //                hand 1
		kIntAnimToNode = 0x267B390;        // takes a 5th (byte) argument, passed as 0
		kIntNodeTimes = 0x325F550;         // all 197 entries identical to Enhanced's
		kIntNode41TimeFn = 0x26785F0;
		kIntIsSegmented = 0x267AFF0;
		kIntPostMeleeEmptyCall = 0x2664275;  // in PM_Weapon_CheckForReload 0x26641E0

		// ---- segmented reloads (bo3_segreload.h) ---------------------------------------------------------------------------
		kSrBeginReload = 0x265B6C0;
		kSrLoopBegin = 0x265D160;          // reads its first argument (pm) on retail
		kSrLoopDone = 0x26655C0;
		kSrStartDone = 0x26658C0;
		kSrReloadEmptyTime = 0x26785F0;
		kSrIsSegmented = 0x267AFF0;
		kSrBeginSites[0] = {0x26644D8, kSrBeginReload};
		kSrBeginSites[1] = {0x26620DF, kSrBeginReload};
		kSrLoopSites[0] = {0x265B9E2, kSrLoopBegin};
		kSrLoopSites[1] = {0x2665983, kSrLoopBegin};
		kSrLoopSites[2] = {0x2665822, kSrLoopBegin};
		kSrLoopDoneSites[0] = {0x265FBC4, kSrLoopDone};
		kSrLoopDoneSites[1] = {0x266096D, kSrLoopDone};
		kSrStartDoneSites[0] = {0x265FBB1, kSrStartDone};

		// ---- IK (bo3_ik.h; its DObj anchors are a retail table in InstallIk) ------------------------------------------------
		kDObjCalcSkel = 0x22C0480;
		kCallSkelCalcBone = 0x1ABB5C;
		kCallSkelCalcPose = 0x99DA09;

		// ---- slide (bo3_slide.h) ------------------------------------------------------------------------------------------
		kSlImageSize = kRetailImageSize;
		kSlUpdate = 0x1692C0;
		kSlEndCheck = 0x168F50;
		kSlCanSlide = 0x1683A0;
		kSlCheckJump = 0x25FCE10;
		kSlFriction = 0x2613540;
		kSlAccelerate = 0x260ECD0;
		kSlCmdScale = 0x26113D0;
		kSlSprintOk = 0x2615E60;
		kSlStepSlide = 0x2620A20;
		kSlMoveScale = 0x2672DE0;
		const uintptr_t slideCalls[] = {0x261F477, 0x169300, 0x169342, 0x1692F1, 0x261A3B2, 0x2614971,
		                                0x261A42D, 0x261A6D0, 0x261168F, 0x2617D87, 0x261A773};
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
