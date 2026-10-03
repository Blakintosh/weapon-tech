// MW2019 (IW8) slide for BO3 (both supported exes). OPT-IN: nothing is installed or changed unless slide_enable=1 is in the
// cfg when the DLL starts. Research: the author's research notes (IW8 SuitDef / dvars, BO3 slide functions), the IW8
// decompile bg_slide.cpp / bg_pmove.cpp and the IW8 PDB build (PM_WalkMove's slide-in acceleration = suit inAcceleration).
//
// ---- Movement (BG: server Pmove and client prediction both run it) ---------------------------------------------------
// Every hook is a call-site rel32 redirect through one stub page; every callee was checked in IDA (work.i64): plain code,
// no control-flow flattening, no pointer-decryption state, no return-address reads (the author's research notes). The original
// is always called from our hook with the caller's own arguments; our code runs before and after it. Only the ps being
// processed (and pm's usercmd copy, which IW8 itself edits) is written, and only from the ps, the usercmd and the cfg, so
// the server's snapshot matches the client's prediction. Every ps field used is networked (BO3 ps netfields 0x143370CF0:
// slide flags 0x840 5 bits, slideTime 0x844, outDuration 0x848, subsequentCount 0x84C 4 bits, type 0x850, offsets 0x834).
//
//   site (RVA)   caller -> callee                       what we do
//   0x2770ACD    PmoveSingle -> PM_Slide_Update 0x1F92D0  pre/post: undo BO3's instant 625/550/450 push on the start
//                                                       frame; IW8 subsequent count (1000 ms window, 15 max); IW8 view
//                                                       angle offsets (ps+0x834) blended 300/300 ms (slide_view=1); camera
//                                                       pitch 15 deg over 1100 ms (slide_camera=1); in-air friction ramp
//                                                       after a slide that ended by losing the ground; the debug trace
//   0x1F9508     PM_Slide_Update -> EndCheck 0x1F8A20     IW8 length (max_time_ms, via the start time BO3's 750/500 test
//                                                       sees), min continue speed 50 (BO3: 150), ADS end on/off
//   0x1F953C     PM_Slide_Update -> CanSlide 0x1F8730     min start speed 140 (BO3: 100), subsequentCount < 15
//   0x1F94FC     PM_Slide_Update -> PM_CheckJump 0x274C280  slide-jump: velocity x jump_speed_scale (IW8
//   0x276243B    PM_WalkMove     -> PM_CheckJump            Slide_ScaleVelocityBeforeJump), also within lateJumpGraceMs
//   0x27628D6    0x142762810     -> PM_CheckJump            of the slide's end
//   0x2762490    PM_WalkMove -> PM_Friction 0x275F030      slide friction 0.26 / 0.10 / 0.55 / 0.05 (BO3: 0.15 / 0.001 /
//                                                       0.45 / 0) and the out-friction ramp 0.36 -> 1 over 500 ms
//   0x2762736    PM_WalkMove -> PM_Accelerate 0x275F7E0    while sliding: IW8's wish (Slide_UpdateMovement + PM_CmdScale_Walk):
//                                                       forward 127 for inTimeMs at speed x sprint x crouch x gun x
//                                                       inMaxSpeedScale x (1 - 0.15)^n with accel inAcceleration, then no
//                                                       forward; strafe x strafe_speed_scale
//   0x2760470    PM_CmdScale_Walk -> PM_CmdScale 0x27600B0  BO3's post-slide 0.01 -> 1 speed ramp removed (ps+0x848 reads 0
//                                                       for the call; IW8's outSpeedScale is 1 -> 1)
//   0x275EEDE    PM_UpdateSprint -> sprint-start check 0x275EB70   sprint locked for sprintDelayMs after a slide
//                                                       (slide_sprint_lock=1)
//   0x27627D0    PM_WalkMove -> PM_StepSlideMove 0x2775BE0   after it, while sliding: +0.5 per horizontal axis so BO3's
//                                                       velocity truncation rounds instead (slide_snap_round=1)
//   called:      0x27D2640 (ps, weapon) -> the gun's move speed scale (what PM_CmdScale_Walk uses)
//
// BO3 slide state: pm_flags (ps+0x10) bit 0x80 = sliding. ps+0x844 = the start time while sliding, the end time after
// (BO3's End writes it and sets ps+0x840 = 8). EndCheck's own rules (ground loss, stance, water, juke, 750 ms overrun
// under geometry, 675 with sprint) are kept; IW8 has the same overrun (friction_duration_ms 750, x0.9 with sprint).
//
// ---- Viewmodel: the MW slide gesture as an additive layer (client only, visual) -----------------------------------
// The gesture xanims (in / loop / out, frame 0 = the gun's idle as the additive reference, frames 1..N the gesture) sit
// in the juke slots under additive root 184: jukeForwardAnim (187) = in, jukeBackwardAnim (188) = loop,
// jukeForwardADSAnim (191) = out (GDT; ZM never jukes and the engine forces 184..192 to 0 every frame, so they are inert
// until this layer drives them). A gun takes part when those leaves hold real anims (not the idle a blank slot gets).
// SlideViewmodelFrame() runs in the viewmodel hook after CG_UpdateViewWeaponAnim (bo3_additive.h AfterViewWeaponAnim),
// like bo3_locomotion.h: in plays once from the slide's start, then the loop, and the out from the slide's end; leaves
// crossfade, the root fades in / out, stands down for reload / raise / drop / melee / offhand, ADS fades it.
// BO3's own slide anims would play under it: for guns with the layer, slots whose name ends in slide_in / slide_loop /
// slide_out / slide_air_in / slide_in_air are pointed at the gun's idle name (runtime, only with slide_enable=1; takes
// effect from the next tree build, i.e. the next raise; the log lists every slot changed).
//
// ---- Config (weapon_tech.cfg) -------------------------------------------------------------------------------------
//   slide_enable=0|1          master switch, read at start only (default 0: nothing installed, nothing changes)
//   slide_suit=<preset>       iw8_defaultsuit_mp (default; every MP / operator suit) or iw8_defaultsuit (SP: inAcceleration
//                             2.8, frictionScaleBlocked 0)
//   slide_suit=<key>=<v>[,<key>=<v>...]   SuitDef overrides (names as in the SuitDef, without "slide_"):
//                             inTimeMs inMaxSpeedScale inAcceleration max_time_ms frictionScaleNormal frictionScaleDownhill
//                             frictionScaleUphill frictionScaleBlocked outTimeMs outFrictionScaleStart outFrictionScaleFinish
//                             inAirTimeMs inAirFrictionScaleStart inAirFrictionScaleFinish strafe_speed_scale
//                             jump_speed_scale sprintDelayMs viewBlendInTimeMs viewBlendOutTimeMs
//                             player_sprintSpeedScale player_crouchSpeedScale
//   slide_dvars=<key>=<v>[,...]   the exe-only IW8 dvars (PDB defaults): min_required_velocity 140 min_continue_velocity 50
//                             subsequentSlideTime 1000 subsequentSlideScale 0.15 lateJumpGraceMs 300 cameraPitchOffset 15
//                             cameraRotateTimeMs 1100 cameraAlignmentEaseMode -1 (-1 = BO3's own ease, 2) view_angles -3 0 -1
//                             (three numbers, space separated) viewInterpType 1 stopspeed 100 friction 5.5
//   slide_ads_ends=1          ADS ends the slide (BO3's EndCheck already does on the ADS flag; 0 = it doesn't)
//   slide_sprint_lock=1       no sprint for sprintDelayMs after a slide
//   slide_camera=1            IW8 camera: pitch eased to cameraPitchOffset over cameraRotateTimeMs (0 = BO3's 10 / 500)
//   slide_view=1              IW8 view angles blended viewBlendIn / Out (0 = BO3's -8 / -7 and roll bounce)
//   slide_snap_round=1        while sliding, BO3's per-step velocity truncation becomes round-to-nearest (see SlStepHook;
//                             0 = BO3's truncation, which drains ~150-300 u/s^2 at high frame rates)
//   slide_gesture=1           the viewmodel gesture layer (only with slide_enable=1)
//   slide_gesture_nodes=187,188,191   in, loop, out leaves (under root 184)
//   slide_gesture_blend=0.1,0.15,0.12  root fade in, root fade out, leaf crossfade (s)
//   slide_gesture_weight=1    root weight
//   slide_gesture_fps=30      the gesture xanims' frame rate (frame count = length x fps)
//   slide_gesture_ref=1       frame 0 is the additive reference (played frames start at 1)
//   slide_gesture_off=<weapon>   no gesture layer (and no native-anim swap) for that weapon (repeatable)
//   slide_native_anims=swap|keep   swap (default): BO3's slide anims -> the idle for guns with the layer
//   slide_debug=0|1|2|3       1: start / end / jump lines; 2: also a per-frame speed trace while sliding and 600 ms after;
//                             3: also velocity probes around PM_Accelerate / PM_StepSlideMove and between frames
// Everything except slide_enable and the hook set is live when the cfg is the loose file (own watcher, 0.5 s).
#pragma once
#include <atomic>
#include <cmath>

namespace
{
	// ---- Engine addresses (RVAs = VA - 0x140000000): Enhanced values; bo3_retail.h overwrites them on retail ------------
	uintptr_t kSlImageSize = 0x1A53F000;
	uintptr_t kSlUpdate = 0x1F92D0, kSlEndCheck = 0x1F8A20, kSlCanSlide = 0x1F8730, kSlCheckJump = 0x274C280;
	uintptr_t kSlFriction = 0x275F030, kSlAccelerate = 0x275F7E0, kSlCmdScale = 0x27600B0, kSlSprintOk = 0x275EB70;
	uintptr_t kSlStepSlide = 0x2775BE0;  // PM_StepSlideMove(pm, pml, int)
	uintptr_t kSlMoveScale = 0x27D2640;  // (ps, weapon) -> float, lerp(hip, ADS) move speed scale
	struct SlSite
	{
		uintptr_t call, target;
		const char *what;
	};
	enum SlHook { kShUpdate, kShEndCheck, kShCanSlide, kShJump, kShFriction, kShAccel, kShCmdScale, kShSprint, kShStep, kShCount };
	SlSite kSlSites[] = {  // retail: bo3_retail.h rewrites call and target, same order
	    {0x2770ACD, kSlUpdate, "PmoveSingle -> PM_Slide_Update"},       // kShUpdate
	    {0x1F9508, kSlEndCheck, "PM_Slide_Update -> EndCheck"},         // kShEndCheck
	    {0x1F953C, kSlCanSlide, "PM_Slide_Update -> CanSlide"},         // kShCanSlide
	    {0x1F94FC, kSlCheckJump, "PM_Slide_Update -> PM_CheckJump"},    // kShJump
	    {0x276243B, kSlCheckJump, "PM_WalkMove -> PM_CheckJump"},       // kShJump
	    {0x27628D6, kSlCheckJump, "0x142762810 -> PM_CheckJump"},       // kShJump
	    {0x2762490, kSlFriction, "PM_WalkMove -> PM_Friction"},         // kShFriction
	    {0x2762736, kSlAccelerate, "PM_WalkMove -> PM_Accelerate"},     // kShAccel
	    {0x2760470, kSlCmdScale, "PM_CmdScale_Walk -> PM_CmdScale"},    // kShCmdScale
	    {0x275EEDE, kSlSprintOk, "PM_UpdateSprint -> sprint start check"},  // kShSprint
	    {0x27627D0, kSlStepSlide, "PM_WalkMove -> PM_StepSlideMove"},   // kShStep (velocity snap rounding + probe)
	};
	constexpr SlHook kSlSiteHook[] = {kShUpdate, kShEndCheck, kShCanSlide, kShJump, kShJump, kShJump, kShFriction, kShAccel,
	                                  kShCmdScale, kShSprint, kShStep};

	// pmove_t
	constexpr size_t kSlPmPs = 0x0, kSlPmTime = 0x8, kSlPmForward = 0x40, kSlPmRight = 0x41, kSlPmSide = 0x2AC;
	// pml_t
	constexpr size_t kSlPmlForward = 0x0, kSlPmlRight = 0xC, kSlPmlFrametime = 0x24, kSlPmlWalking = 0x2C,
	                 kSlPmlGroundPlane = 0x30, kSlPmlNormal = 0x40, kSlPmlSurface = 0x54;
	// playerState_t
	constexpr size_t kSlPsClient = 0x0, kSlPsPmType = 0x8, kSlPsFlags = 0x10, kSlPsOther = 0x20, kSlPsVel = 0x3C,
	                 kSlPsSpeed = 0xCC, kSlPsGroundEnt = 0x100, kSlPsJumpTime = 0x110, kSlPsViewTrans = 0xDC,
	                 kSlPsWeapon = 0x2C0, kSlPsViewPitch = 0x318, kSlPsMoveScale = 0x7D4, kSlPsWater = 0xBB0;
	constexpr size_t kSlPsAngles = 0x834, kSlPsSlideFlags = 0x840, kSlPsSlideTime = 0x844, kSlPsOutDur = 0x848,
	                 kSlPsCount = 0x84C, kSlPsType = 0x850;
	constexpr uint64_t kPmfSliding = 0x80, kPmfAds = 0x400000, kPmfJuke = 0x40, kPmfAirSlide = 0x400000000000ull;
	constexpr uint64_t kPmfFrictionOwn = 0x80000000ull | 0x8000000000000ull | 0x2000000ull |
	                                     0x400000000ull;  // PM_Friction branches that skip or rescale friction: left to BO3
	constexpr int kSlEnded = 8;  // ps+0x840 after BO3's End

	// ---- settings (immutable snapshots, swapped whole on a live reload) -----------------------------------------------
	struct SlideSettings
	{
		// SuitDef (iw8_defaultsuit_mp)
		int inTimeMs = 200;
		float inMaxSpeedScale = 1.8f, inAcceleration = 1.8f;
		int maxTimeMs = 800;
		float fricNormal = 0.26f, fricDownhill = 0.10f, fricUphill = 0.55f, fricBlocked = 0.05f;
		int outTimeMs = 500;
		float outFricStart = 0.36f, outFricFinish = 1.0f;
		int inAirTimeMs = 100;
		float inAirFricStart = 0.3f, inAirFricFinish = 1.0f;
		float strafeScale = 0.25f, jumpScale = 0.55f;
		int sprintDelayMs = 300, viewBlendInMs = 300, viewBlendOutMs = 300;
		float sprintSpeedScale = 1.5075f, crouchSpeedScale = 0.55f;
		// dvars (PDB defaults)
		float minStartSpeed = 140, minContinueSpeed = 50;
		int subsequentTime = 1000;
		float subsequentScale = 0.15f;
		int lateJumpGraceMs = 300;
		float cameraPitch = 15;
		int cameraTimeMs = 1100, cameraEase = -1;
		float viewAngles[3] = {-3, 0, -1};
		int viewInterp = 1;
		float stopspeed = 100, friction = 5.5f;
		// switches
		bool adsEnds = true, sprintLock = false, camera = false, view = false, gesture = true, snapRound = true;
		int debug = 0;
		// gesture layer
		uint32_t nodes[3] = {187, 188, 191};
		float blendIn = 0.1f, blendOut = 0.15f, blendCross = 0.12f, gestureWeight = 1, gestureFps = 30;
		bool gestureRef = true, swapNative = true;
		char suitName[32] = "iw8_defaultsuit_mp";
	};
	constexpr uint32_t kSlRoot = 184;

	bool g_slEnable, g_slHooked, g_slSeenEnable;
	void SlSwapNativeSlots();
	void SlideViewmodelFrame(uint8_t *ps, uint8_t *vm, double now);
	SlideSettings g_slParse;  // filled while the cfg is read (start), then published
	std::atomic<const SlideSettings *> g_sl{nullptr};
	char g_slOff[32][64];  // slide_gesture_off=
	int g_slOffCount;
	FILETIME g_slCfgTime;
	char g_slCfgPath[MAX_PATH];

	const SlideSettings &Sl()
	{
		static const SlideSettings s_default;
		const SlideSettings *s = g_sl.load(std::memory_order_acquire);
		return s ? *s : s_default;
	}

	void SlApplyPreset(SlideSettings &s, const char *name)
	{
		SlideSettings d;  // iw8_defaultsuit_mp
		auto keep = s;
		s = d;
		// keep everything that is not a SuitDef value
		memcpy(s.viewAngles, keep.viewAngles, sizeof(s.viewAngles));
		s.minStartSpeed = keep.minStartSpeed, s.minContinueSpeed = keep.minContinueSpeed, s.subsequentTime = keep.subsequentTime;
		s.subsequentScale = keep.subsequentScale, s.lateJumpGraceMs = keep.lateJumpGraceMs, s.cameraPitch = keep.cameraPitch;
		s.cameraTimeMs = keep.cameraTimeMs, s.cameraEase = keep.cameraEase, s.viewInterp = keep.viewInterp;
		s.stopspeed = keep.stopspeed, s.friction = keep.friction, s.adsEnds = keep.adsEnds, s.sprintLock = keep.sprintLock;
		s.camera = keep.camera, s.view = keep.view, s.gesture = keep.gesture, s.debug = keep.debug, s.snapRound = keep.snapRound;
		memcpy(s.nodes, keep.nodes, sizeof(s.nodes));
		s.blendIn = keep.blendIn, s.blendOut = keep.blendOut, s.blendCross = keep.blendCross, s.gestureWeight = keep.gestureWeight;
		s.gestureFps = keep.gestureFps, s.gestureRef = keep.gestureRef, s.swapNative = keep.swapNative;
		if (!_stricmp(name, "iw8_defaultsuit"))  // SP
		{
			s.inAcceleration = 2.8f;
			s.fricBlocked = 0.0f;
		}
		strcpy_s(s.suitName, name);
	}

	// key=value pairs, comma separated. Returns false on an unknown key or a bad number.
	bool SlParsePairs(SlideSettings &s, const char *text, bool suit)
	{
		char buf[512];
		strncpy_s(buf, text, _TRUNCATE);
		if (char *hash = strchr(buf, '#'))
			*hash = 0;
		struct F
		{
			const char *key;
			float *f;
			int *i;
		};
		const F suitKeys[] = {
		    {"inTimeMs", nullptr, &s.inTimeMs}, {"inMaxSpeedScale", &s.inMaxSpeedScale, nullptr},
		    {"inAcceleration", &s.inAcceleration, nullptr}, {"max_time_ms", nullptr, &s.maxTimeMs},
		    {"frictionScaleNormal", &s.fricNormal, nullptr}, {"frictionScaleDownhill", &s.fricDownhill, nullptr},
		    {"frictionScaleUphill", &s.fricUphill, nullptr}, {"frictionScaleBlocked", &s.fricBlocked, nullptr},
		    {"outTimeMs", nullptr, &s.outTimeMs}, {"outFrictionScaleStart", &s.outFricStart, nullptr},
		    {"outFrictionScaleFinish", &s.outFricFinish, nullptr}, {"inAirTimeMs", nullptr, &s.inAirTimeMs},
		    {"inAirFrictionScaleStart", &s.inAirFricStart, nullptr}, {"inAirFrictionScaleFinish", &s.inAirFricFinish, nullptr},
		    {"strafe_speed_scale", &s.strafeScale, nullptr}, {"jump_speed_scale", &s.jumpScale, nullptr},
		    {"sprintDelayMs", nullptr, &s.sprintDelayMs}, {"viewBlendInTimeMs", nullptr, &s.viewBlendInMs},
		    {"viewBlendOutTimeMs", nullptr, &s.viewBlendOutMs}, {"player_sprintSpeedScale", &s.sprintSpeedScale, nullptr},
		    {"player_crouchSpeedScale", &s.crouchSpeedScale, nullptr}};
		const F dvarKeys[] = {
		    {"min_required_velocity", &s.minStartSpeed, nullptr}, {"min_continue_velocity", &s.minContinueSpeed, nullptr},
		    {"subsequentSlideTime", nullptr, &s.subsequentTime}, {"subsequentSlideScale", &s.subsequentScale, nullptr},
		    {"lateJumpGraceMs", nullptr, &s.lateJumpGraceMs}, {"cameraPitchOffset", &s.cameraPitch, nullptr},
		    {"cameraRotateTimeMs", nullptr, &s.cameraTimeMs}, {"cameraAlignmentEaseMode", nullptr, &s.cameraEase},
		    {"viewInterpType", nullptr, &s.viewInterp}, {"stopspeed", &s.stopspeed, nullptr}, {"friction", &s.friction, nullptr}};
		for (char *ctx = nullptr, *tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(nullptr, ",", &ctx))
		{
			while (*tok == ' ' || *tok == '\t')
				tok++;
			if (!*tok)
				continue;
			char *eq = strchr(tok, '=');
			if (!eq)
				return false;
			*eq = 0;
			char *key = tok, *val = eq + 1;
			for (char *e = eq - 1; e >= key && (*e == ' ' || *e == '\t'); e--)
				*e = 0;
			if (!suit && !strcmp(key, "view_angles"))
			{
				float a[3];
				if (sscanf_s(val, "%f %f %f", &a[0], &a[1], &a[2]) != 3)
					return false;
				memcpy(s.viewAngles, a, sizeof(a));
				continue;
			}
			bool found = false;
			const F *keys = suit ? suitKeys : dvarKeys;
			const size_t nkeys = suit ? _countof(suitKeys) : _countof(dvarKeys);
			for (size_t k = 0; k < nkeys; k++)
				if (const F &f = keys[k]; !strcmp(f.key, key))
				{
					found = true;
					if (f.f ? sscanf_s(val, "%f", f.f) != 1 : sscanf_s(val, "%d", f.i) != 1)
						return false;
				}
			if (!found)
				return false;
		}
		return true;
	}

	bool SlParseBool(const char *v, bool &out)
	{
		int i;
		if (sscanf_s(v, "%d", &i) != 1)
			return false;
		out = i != 0;
		return true;
	}

	// Every slide_ key except slide_enable / slide_gesture_off, into s. known = false: not ours.
	bool SlParseSetting(SlideSettings &s, const char *line, bool &known)
	{
		known = true;
		auto is = [&](const char *k) { return strncmp(line, k, strlen(k)) == 0; };
		auto val = [&](const char *k) { return line + strlen(k); };
		if (is("slide_suit="))
		{
			const char *v = val("slide_suit=");
			if (!strchr(v, '='))
			{
				char name[32] = {};
				if (sscanf_s(v, "%31[^ \t#]", name, static_cast<unsigned>(sizeof(name))) != 1 ||
				    (_stricmp(name, "iw8_defaultsuit_mp") && _stricmp(name, "iw8_defaultsuit")))
					return false;
				SlApplyPreset(s, name);
				return true;
			}
			return SlParsePairs(s, v, true);
		}
		if (is("slide_dvars="))
			return SlParsePairs(s, val("slide_dvars="), false);
		if (is("slide_ads_ends="))
			return SlParseBool(val("slide_ads_ends="), s.adsEnds);
		if (is("slide_sprint_lock="))
			return SlParseBool(val("slide_sprint_lock="), s.sprintLock);
		if (is("slide_camera="))
			return SlParseBool(val("slide_camera="), s.camera);
		if (is("slide_view="))
			return SlParseBool(val("slide_view="), s.view);
		if (is("slide_snap_round="))
			return SlParseBool(val("slide_snap_round="), s.snapRound);
		if (is("slide_gesture="))
			return SlParseBool(val("slide_gesture="), s.gesture);
		if (is("slide_debug="))
			return sscanf_s(val("slide_debug="), "%d", &s.debug) == 1;
		if (is("slide_gesture_nodes="))
		{
			unsigned a, b, c;
			if (sscanf_s(val("slide_gesture_nodes="), "%u,%u,%u", &a, &b, &c) != 3)
				return false;
			for (unsigned n : {a, b, c})
				if (n < 185 || n > 196 || n == 193 || n == 195)
					return false;
			s.nodes[0] = a, s.nodes[1] = b, s.nodes[2] = c;
			return true;
		}
		if (is("slide_gesture_blend="))
			return sscanf_s(val("slide_gesture_blend="), "%f,%f,%f", &s.blendIn, &s.blendOut, &s.blendCross) == 3 &&
			       s.blendIn > 0 && s.blendOut > 0 && s.blendCross > 0;
		if (is("slide_gesture_weight="))
			return sscanf_s(val("slide_gesture_weight="), "%f", &s.gestureWeight) == 1;
		if (is("slide_gesture_fps="))
			return sscanf_s(val("slide_gesture_fps="), "%f", &s.gestureFps) == 1 && s.gestureFps > 0;
		if (is("slide_gesture_ref="))
			return SlParseBool(val("slide_gesture_ref="), s.gestureRef);
		if (is("slide_native_anims="))
		{
			const char *v = val("slide_native_anims=");
			if (!strncmp(v, "swap", 4))
				return s.swapNative = true, true;
			if (!strncmp(v, "keep", 4))
				return s.swapNative = false, true;
			return false;
		}
		known = false;
		return true;
	}

	// Called with every cfg line (weapon_tech.h). known = true for slide_ keys.
	bool ParseSlideLine(const char *line, bool &known)
	{
		if (strncmp(line, "slide_", 6) != 0)
			return known = false, true;
		known = true;
		if (!strncmp(line, "slide_enable=", 13))
		{
			g_slSeenEnable = true;
			return SlParseBool(line + 13, g_slEnable);
		}
		if (!strncmp(line, "slide_gesture_off=", 18))
		{
			char w[64] = {};
			if (g_slOffCount >= 32 || sscanf_s(line + 18, "%63[^, \t#]", w, static_cast<unsigned>(sizeof(w))) != 1)
				return false;
			strcpy_s(g_slOff[g_slOffCount++], w);
			return true;
		}
		bool k;
		bool ok = SlParseSetting(g_slParse, line, k);
		if (!k)
			return false;  // a slide_ key we don't know
		return ok;
	}

	// ---- small math --------------------------------------------------------------------------------------------------
	template <typename T>
	T &SlAt(uint8_t *p, size_t off) { return *reinterpret_cast<T *>(p + off); }
	float SlClamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
	float SlLen3(const float *v) { return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
	float SlLen2(const float *v) { return sqrtf(v[0] * v[0] + v[1] * v[1]); }

	// IW8 Slide_GetViewAngleOffsets' slide_viewInterpType curves.
	float SlEase(float f, int type)
	{
		f = SlClamp01(f);
		switch (type)
		{
		case 1: return (sinf((f - 0.5f) * 3.1415927f) + 1.0f) * 0.5f;
		case 2: return ((f * 6.0f - 15.0f) * f + 10.0f) * f * f * f;
		case 3: return f * f * f;
		case 4: return (f - 1) * (f - 1) * (f - 1) + 1;
		case 5: return f * f * f * f;
		case 6: return 1 - (f - 1) * (f - 1) * (f - 1) * (f - 1);
		case 7: return powf(2.0f, (f - 1) * 10.0f);
		case 8: return 1 - powf(2.0f, f * -10.0f);
		default: return f;
		}
	}
	// The fraction whose eased value is e (all curves are monotonic): bisection.
	float SlEaseInverse(float e, int type)
	{
		if (e <= SlEase(0, type))
			return 0;
		if (e >= SlEase(1, type))
			return 1;
		float lo = 0, hi = 1;
		for (int i = 0; i < 24; i++)
		{
			float m = (lo + hi) * 0.5f;
			(SlEase(m, type) < e ? lo : hi) = m;
		}
		return (lo + hi) * 0.5f;
	}

	// ---- per-call context (each thread runs its own Pmove: server and client) ------------------------------------------
	struct SlCtx
	{
		uint8_t *walkPm, *walkPml;  // set by the friction hook, read by the accelerate hook (same PM_WalkMove)
		int count;                  // IW8's subsequentCount for this PM_Slide_Update call (CanSlide reads it)
		bool inUpdate;
		bool airPath;               // CanSlide is on BO3's air path (pm_flags 0x400000000000)
		float probeXy;              // slide_debug=3: xy after the last PM_StepSlideMove (the leak probe)
		int probeTime;
	};
	thread_local SlCtx t_sl;

	const char *SlSide(const uint8_t *pm) { return pm[kSlPmSide] == 1 ? "cl" : "sv"; }

	using SlUpdateFn = uint64_t (*)(uint8_t *pm, uint8_t *pml);
	using SlPmFn = uint64_t (*)(uint8_t *pm);
	using SlFrictionFn = void (*)(uint8_t *pml, uint8_t *pm);
	using SlAccelFn = void (*)(uint8_t *ps, uint8_t *pml, float *wishdir, float wishspeed, float accel);
	using SlCmdScaleFn = float (*)(uint8_t *pm);
	using SlSprintFn = uint64_t (*)(uint8_t *pm, uint8_t *a2, uint8_t *ps);
	using SlMoveScaleFn = float (*)(uint8_t *ps, uint64_t weapon);
	SlUpdateFn g_slUpdate, g_slCanSlide, g_slJump;
	SlPmFn g_slEndCheck;
	SlFrictionFn g_slFriction;
	SlAccelFn g_slAccel;
	SlCmdScaleFn g_slCmdScale;
	SlSprintFn g_slSprint;
	using SlStepFn = void (*)(uint8_t *pm, uint8_t *pml, int a3);
	SlStepFn g_slStep;
	SlMoveScaleFn g_slMoveScale;

	bool SlInWindow(int now, int from, int ms) { return ms > 0 && now >= from && now - from <= ms; }
	bool SlEndedRecently(uint8_t *ps, int now, int ms)
	{
		return !(SlAt<uint64_t>(ps, kSlPsFlags) & kPmfSliding) && (SlAt<int32_t>(ps, kSlPsSlideFlags) & kSlEnded) &&
		       SlInWindow(now, SlAt<int32_t>(ps, kSlPsSlideTime), ms);
	}

	// IW8 slide-in: forward input forced to 127 for inTimeMs from the start (Slide_IsInSlideInState).
	bool SlInPhase(uint8_t *ps, int now, const SlideSettings &s)
	{
		int dt = now - SlAt<int32_t>(ps, kSlPsSlideTime);
		return s.inTimeMs > 0 && dt >= 0 && dt <= s.inTimeMs;
	}

	// ---- the per-frame trace (slide_debug=2) ---------------------------------------------------------------------------
	void SlTrace(uint8_t *pm, uint8_t *ps, uint8_t *pml, const char *tag)
	{
		const SlideSettings &s = Sl();
		const int now = SlAt<int32_t>(pm, kSlPmTime);
		const bool sliding = (SlAt<uint64_t>(ps, kSlPsFlags) & kPmfSliding) != 0;
		const int since = now - SlAt<int32_t>(ps, kSlPsSlideTime);
		if (s.debug < 2 || (!sliding && !SlEndedRecently(ps, now, 600)))
			return;
		const float *v = &SlAt<float>(ps, kSlPsVel);
		const float *a = &SlAt<float>(ps, kSlPsAngles);
		Log("slide: trace %s c%d t=%d %s %+5d ms | xy %6.1f z %+6.1f | n %d type %d | ground %d walk %d | ang %.2f %.2f %.2f%s",
		    SlSide(pm), SlAt<int32_t>(ps, kSlPsClient), now, sliding ? (SlInPhase(ps, now, s) ? "IN   " : "SLIDE") : "after", since,
		    SlLen2(v), v[2], SlAt<int32_t>(ps, kSlPsCount), SlAt<int32_t>(ps, kSlPsType), SlAt<int32_t>(ps, kSlPsGroundEnt),
		    pml ? SlAt<int32_t>(pml, kSlPmlWalking) : -1, a[0], a[1], a[2], tag);
	}

	// ---- hooks -------------------------------------------------------------------------------------------------------
	uint64_t SlUpdateHook(uint8_t *pm, uint8_t *pml)
	{
		uint8_t *ps = SlAt<uint8_t *>(pm, kSlPmPs);
		const SlideSettings &s = Sl();
		const int now = SlAt<int32_t>(pm, kSlPmTime);
		const uint64_t flags0 = SlAt<uint64_t>(ps, kSlPsFlags);
		const bool sliding0 = (flags0 & kPmfSliding) != 0;
		float vel0[3];
		memcpy(vel0, &SlAt<float>(ps, kSlPsVel), sizeof(vel0));
		const int slideFlags0 = SlAt<int32_t>(ps, kSlPsSlideFlags), slideTime0 = SlAt<int32_t>(ps, kSlPsSlideTime);
		const bool hadEnd = !sliding0 && (slideFlags0 & kSlEnded);
		float ang0[3];
		memcpy(ang0, &SlAt<float>(ps, kSlPsAngles), sizeof(ang0));
		const uint32_t other0 = SlAt<uint32_t>(ps, kSlPsOther);
		uint8_t trans0[0x20];
		memcpy(trans0, ps + kSlPsViewTrans, sizeof(trans0));
		const int ground0 = SlAt<int32_t>(ps, kSlPsGroundEnt);
		if (s.debug >= 3 && sliding0 && t_sl.probeTime)
			Log("slide: probe %s c%d t=%d entry xy %.2f (after the last step-slide %.2f at t=%d: %+.2f between frames)", SlSide(pm),
				SlAt<int32_t>(ps, kSlPsClient), now, SlLen2(vel0), t_sl.probeXy, t_sl.probeTime, SlLen2(vel0) - t_sl.probeXy);

		// IW8 Slide_Update: the count resets once the last slide ended more than subsequentSlideTime ago.
		int count = SlAt<int32_t>(ps, kSlPsCount);
		if (!sliding0 && (!hadEnd || slideTime0 + s.subsequentTime < now))
			count = 0;
		t_sl.count = count;
		t_sl.inUpdate = true;
		t_sl.airPath = (flags0 & kPmfAirSlide) != 0;
		const uint64_t ret = g_slUpdate(pm, pml);
		t_sl.inUpdate = false;

		const uint64_t flags1 = SlAt<uint64_t>(ps, kSlPsFlags);
		const bool sliding1 = (flags1 & kPmfSliding) != 0;
		float *vel = &SlAt<float>(ps, kSlPsVel);
		if (!sliding0 && sliding1)
		{
			// The start: BO3 set |v| to 625 / 550 / 450 x 0.85^n x movementMultiplierSlide (and v.z = 0). IW8 has no push:
			// the slide-in acceleration does it (the PM_Accelerate hook). Back to the speed the player had.
			const float pushed = SlLen2(vel);
			vel[0] = vel0[0];
			vel[1] = vel0[1];
			const bool subsequent = hadEnd && slideTime0 + s.subsequentTime > now;
			count = count + (subsequent ? 1 : 0);
			SlAt<int32_t>(ps, kSlPsCount) = count > 15 ? 15 : count;
			if (s.camera)
			{
				// IW8 Slide_StartViewAngleTransition: only when looking further down than cameraPitchOffset.
				const float pitch = SlAt<float>(ps, kSlPsViewPitch);
				if (s.cameraPitch < pitch)
				{
					SlAt<uint32_t>(ps, kSlPsOther) |= 0x20;
					SlAt<int32_t>(ps, 0xDC) = 9;
					SlAt<int32_t>(ps, 0xE0) = now;
					SlAt<int32_t>(ps, 0xE4) = now;
					SlAt<int32_t>(ps, 0xE8) = s.cameraTimeMs;
					SlAt<int32_t>(ps, 0xEC) = s.cameraEase >= 0 ? s.cameraEase : 2;  // BO3's own start uses 2
					SlAt<float>(ps, 0xF0) = pitch;
					SlAt<float>(ps, 0xF8) = s.cameraPitch;
				}
				else if (!(other0 & 0x20) && (SlAt<uint32_t>(ps, kSlPsOther) & 0x20))
				{
					// BO3 started its 10 deg transition and IW8 wouldn't: undo it.
					SlAt<uint32_t>(ps, kSlPsOther) = (SlAt<uint32_t>(ps, kSlPsOther) & ~0x20u) | (other0 & 0x20u);
					memcpy(ps + kSlPsViewTrans, trans0, sizeof(trans0));
				}
			}
			if (s.debug)
				Log("slide: START %s c%d t=%d speed %.1f (BO3 push was %.1f, undone) n %d type %d pitch %.1f%s", SlSide(pm),
				    SlAt<int32_t>(ps, kSlPsClient), now, SlLen2(vel0), pushed, SlAt<int32_t>(ps, kSlPsCount),
				    SlAt<int32_t>(ps, kSlPsType), SlAt<float>(ps, kSlPsViewPitch), t_sl.airPath ? " (air path)" : "");
		}
		else if (!sliding0 && !sliding1)
			SlAt<int32_t>(ps, kSlPsCount) = count;  // BO3 zeroes it every frame; IW8 keeps it for subsequentSlideTime
		else if (sliding0 && !sliding1 && s.debug)
		{
			const bool jumped = SlAt<int32_t>(ps, kSlPsJumpTime) == now;
			Log("slide: END %s c%d t=%d after %d ms, speed %.1f%s%s", SlSide(pm), SlAt<int32_t>(ps, kSlPsClient), now,
			    now - slideTime0, SlLen2(vel), jumped ? ", jump" : "", ground0 == 1023 ? ", in the air" : "");
		}

		// IW8's in-air friction ramp: a slide that ended by losing the ground (not a slide-jump) keeps friction in the air
		// for inAirTimeMs (PM_Friction: Slide_SlideOutInAirFrictionScale; BO3 has no air friction).
		if (!sliding1 && s.inAirTimeMs > 0 && SlEndedRecently(ps, now, s.inAirTimeMs) &&
		    SlAt<int32_t>(ps, kSlPsGroundEnt) == 1023 && SlAt<int32_t>(ps, kSlPsJumpTime) < SlAt<int32_t>(ps, kSlPsSlideTime))
		{
			const float f = SlClamp01(static_cast<float>(now - SlAt<int32_t>(ps, kSlPsSlideTime)) / s.inAirTimeMs);
			const float scale = (1 - f) * s.inAirFricStart + f * s.inAirFricFinish;
			const float speed = SlLen3(vel), dt = SlAt<float>(pml, kSlPmlFrametime);
			if (speed >= 1.0f && dt > 0)
			{
				const float drop = (speed > s.stopspeed ? speed : s.stopspeed) * scale * s.friction * dt;
				const float k = (speed - drop > 0 ? speed - drop : 0) / speed;
				for (int i = 0; i < 3; i++)
					vel[i] *= k;
			}
		}

		// IW8 view angles (Slide_GetViewAngleOffsets): fraction up over viewBlendIn while sliding, down over viewBlendOut
		// after. The fraction lives in ps+0x834 itself (inverted from last frame's value), so a slide that starts during the
		// blend-out continues from there, as IW8's slideFractionOnStateChange does.
		if (s.view)
		{
			int axis = 0;
			for (int i = 1; i < 3; i++)
				if (fabsf(s.viewAngles[i]) > fabsf(s.viewAngles[axis]))
					axis = i;
			float *ang = &SlAt<float>(ps, kSlPsAngles);
			if (fabsf(s.viewAngles[axis]) < 1e-4f)
				ang[0] = ang[1] = ang[2] = 0;
			else
			{
				float f = SlEaseInverse(SlClamp01(ang0[axis] / s.viewAngles[axis]), s.viewInterp);
				const float dt = SlAt<float>(pml, kSlPmlFrametime) * 1000.0f;
				if (sliding1)
					f = s.viewBlendInMs > 0 ? f + dt / s.viewBlendInMs : 1;
				else
					f = s.viewBlendOutMs > 0 ? f - dt / s.viewBlendOutMs : 0;
				const float e = SlEase(SlClamp01(f), s.viewInterp);
				for (int i = 0; i < 3; i++)
					ang[i] = s.viewAngles[i] * e;
			}
		}
		SlTrace(pm, ps, pml, "");
		return ret;
	}

	// EndCheck: IW8's length and minimum speed through BO3's own test, ADS optional. True = end the slide.
	uint64_t SlEndCheckHook(uint8_t *pm)
	{
		uint8_t *ps = SlAt<uint8_t *>(pm, kSlPmPs);
		const SlideSettings &s = Sl();
		float *vel = &SlAt<float>(ps, kSlPsVel);
		const float speed = SlLen3(vel);
		if (speed < s.minContinueSpeed)
			return 1;  // IW8 slide_min_continue_velocity
		int32_t &slideTime = SlAt<int32_t>(ps, kSlPsSlideTime);
		uint64_t &flags = SlAt<uint64_t>(ps, kSlPsFlags);
		const int32_t start = slideTime;
		const int type = SlAt<int32_t>(ps, kSlPsType);
		const int bo3Len = (type == 1 || type == 2) ? 500 : 750;
		float saved[3];
		memcpy(saved, vel, sizeof(saved));
		const uint64_t flagsSaved = flags;
		// BO3 ends below 150 u/s before anything else: present at least that for the call, so its other rules still run.
		const bool scaled = speed < 150.5f;
		if (scaled)
			for (int i = 0; i < 3; i++)
				vel[i] *= 150.5f / speed;
		slideTime = start + s.maxTimeMs - bo3Len;  // BO3: start + 750 (500) > now; IW8: start + max_time_ms > now
		if (!s.adsEnds)
			flags &= ~kPmfAds;
		const uint64_t ret = g_slEndCheck(pm);
		slideTime = start;
		if (scaled)
			memcpy(vel, saved, sizeof(saved));
		if (!s.adsEnds)
			flags = (flags & ~kPmfAds) | (flagsSaved & kPmfAds);
		return ret;
	}

	// CanSlide: IW8's 140 u/s minimum (BO3: 100) and subsequentCount < 15; a refusal does what BO3's own refusal does.
	uint64_t SlCanSlideHook(uint8_t *pm, uint8_t *pml)
	{
		uint8_t *ps = SlAt<uint8_t *>(pm, kSlPmPs);
		const SlideSettings &s = Sl();
		const bool airPath = (SlAt<uint64_t>(ps, kSlPsFlags) & kPmfAirSlide) != 0;
		const uint64_t ret = g_slCanSlide(pm, pml);
		if (!static_cast<uint32_t>(ret) || airPath)
			return ret;
		const float speed = SlLen3(&SlAt<float>(ps, kSlPsVel));
		const int count = t_sl.inUpdate ? t_sl.count : 0;
		if (speed >= s.minStartSpeed && count < 15)
			return ret;
		uint64_t &flags = SlAt<uint64_t>(ps, kSlPsFlags);
		flags = (flags & 0xFFFFC1FFFFFFFFFFull) | 0x240000000000ull;
		if (s.debug)
			Log("slide: no start %s c%d: speed %.1f (min %.0f), subsequent count %d", SlSide(pm), SlAt<int32_t>(ps, kSlPsClient),
			    speed, s.minStartSpeed, count);
		return 0;
	}

	// PM_CheckJump: IW8 Slide_ScaleVelocityBeforeJump while sliding and within lateJumpGraceMs of the end.
	uint64_t SlJumpHook(uint8_t *pm, uint8_t *pml)
	{
		uint8_t *ps = SlAt<uint8_t *>(pm, kSlPmPs);
		const SlideSettings &s = Sl();
		const int now = SlAt<int32_t>(pm, kSlPmTime);
		const bool sliding = (SlAt<uint64_t>(ps, kSlPsFlags) & kPmfSliding) != 0;
		if (s.jumpScale == 1.0f || !(sliding || SlEndedRecently(ps, now, s.lateJumpGraceMs - 1)))
			return g_slJump(pm, pml);
		float *vel = &SlAt<float>(ps, kSlPsVel);
		float saved[3], scaled[3];
		memcpy(saved, vel, sizeof(saved));
		for (int i = 0; i < 3; i++)
			vel[i] = scaled[i] = saved[i] * s.jumpScale;
		const uint64_t ret = g_slJump(pm, pml);
		if (!static_cast<uint8_t>(ret))
		{
			if (!memcmp(vel, scaled, sizeof(scaled)))
				memcpy(vel, saved, sizeof(saved));  // no jump: as it was
			return ret;
		}
		if (s.debug)
			Log("slide: JUMP %s c%d t=%d %s: xy %.1f -> %.1f (x%.2f), z %.1f", SlSide(pm), SlAt<int32_t>(ps, kSlPsClient), now,
			    sliding ? "out of the slide" : "late", SlLen2(saved), SlLen2(vel), s.jumpScale, vel[2]);
		return ret;
	}

	// PM_Friction (walking): the slide's friction scales and the out-friction ramp.
	void SlFrictionHook(uint8_t *pml, uint8_t *pm)
	{
		t_sl.walkPm = pm;
		t_sl.walkPml = pml;
		uint8_t *ps = SlAt<uint8_t *>(pm, kSlPmPs);
		const SlideSettings &s = Sl();
		float *vel = &SlAt<float>(ps, kSlPsVel);
		float v0[3];
		memcpy(v0, vel, sizeof(v0));
		g_slFriction(pml, pm);
		const uint64_t flags = SlAt<uint64_t>(ps, kSlPsFlags);
		if (!SlAt<int32_t>(pml, kSlPmlWalking) || (SlAt<uint32_t>(pml, kSlPmlSurface) & 2) || SlAt<int32_t>(ps, kSlPsPmType) == 5 ||
		    (flags & kPmfFrictionOwn) || SlAt<int32_t>(ps, kSlPsWater) > 1)
			return;
		const float speed = SlLen2(v0);  // BO3 measures without z while walking, then scales all three
		const float dt = SlAt<float>(pml, kSlPmlFrametime);
		if (speed < 1.0f || dt <= 0)
			return;
		const int now = SlAt<int32_t>(pm, kSlPmTime);
		if (flags & kPmfSliding)
		{
			// BO3: x0 when under something (its trace), else by slope x0.15 / x0.001 / x0.45 (velocity direction . ground
			// normal, +-0.2), times its own landing / 0x1000000 factors. Its result says which: no change at all = blocked;
			// the slope class we work out as it does. IW8's drop = BO3's drop x (IW8 scale / BO3 scale), which keeps BO3's
			// other factors; blocked (BO3 0) is worked out directly.
			const float after = SlLen2(vel);
			float iw8, bo3 = 0;
			const char *cls;
			if (!memcmp(vel, v0, sizeof(v0)))
				iw8 = s.fricBlocked, cls = "blocked";
			else
			{
				iw8 = s.fricNormal, bo3 = 0.15f, cls = "flat";
				if (SlAt<int32_t>(pml, kSlPmlGroundPlane))
				{
					const float *n = &SlAt<float>(pml, kSlPmlNormal);
					const float d = (v0[0] * n[0] + v0[1] * n[1]) / speed;
					if (d > 0.2f)
						iw8 = s.fricDownhill, bo3 = 0.001f, cls = "downhill";
					else if (d < -0.2f)
						iw8 = s.fricUphill, bo3 = 0.45f, cls = "uphill";
				}
			}
			const float drop = bo3 > 0 ? (speed - after) * (iw8 / bo3) : (speed > s.stopspeed ? speed : s.stopspeed) * iw8 * s.friction * dt;
			const float k = (speed - drop > 0 ? speed - drop : 0) / speed;
			for (int i = 0; i < 3; i++)
				vel[i] = v0[i] * k;
			if (s.debug >= 2)
				Log("slide: friction %s c%d t=%d %s x%.2f: %.1f -> %.1f (BO3 %.1f)", SlSide(pm), SlAt<int32_t>(ps, kSlPsClient), now,
				    cls, iw8, speed, SlLen2(vel), after);
			return;
		}
		if (s.outTimeMs > 0 && SlEndedRecently(ps, now, s.outTimeMs))
		{
			// IW8 Slide_SlideOutFrictionScale: BO3's whole friction drop x lerp(start, finish) over outTimeMs.
			const float f = SlClamp01(static_cast<float>(now - SlAt<int32_t>(ps, kSlPsSlideTime)) / s.outTimeMs);
			const float scale = (1 - f) * s.outFricStart + f * s.outFricFinish;
			const float after = SlLen2(vel);
			if (after >= speed)
				return;
			const float drop = (speed - after) * scale;
			const float k = (speed - drop > 0 ? speed - drop : 0) / speed;
			for (int i = 0; i < 3; i++)
				vel[i] = v0[i] * k;
		}
	}

	// BO3's PM_ProjectVelocity onto the ground plane (0x14275DA80), for a unit wish direction.
	void SlProject(float *v, const float *n)
	{
		const float h = v[0] * v[0] + v[1] * v[1];
		if (fabsf(n[2]) < 0.001f || h == 0)
			return;
		const float z = -(v[0] * n[0] + v[1] * n[1]) / n[2];
		const float k = sqrtf((v[2] * v[2] + h) / (z * z + h));
		if (k < 1.0f || z < 0.0f || v[2] > 0.0f)
		{
			v[0] *= k;
			v[1] *= k;
			v[2] = z * k;
		}
	}

	// PM_Accelerate (walking): while sliding, IW8's wish replaces BO3's (Slide_UpdateMovement: forward 127 in the slide-in,
	// 0 after; PM_CmdScale_Walk: side x slide_strafe_speed_scale, the slide-in speed scale; PM_WalkMove: the slide-in's
	// acceleration is the suit's inAcceleration).
	void SlAccelHook(uint8_t *ps, uint8_t *pml, float *wishdir, float wishspeed, float accel)
	{
		uint8_t *pm = t_sl.walkPm;
		if (pml != t_sl.walkPml || !pm || SlAt<uint8_t *>(pm, kSlPmPs) != ps || !(SlAt<uint64_t>(ps, kSlPsFlags) & kPmfSliding))
			return g_slAccel(ps, pml, wishdir, wishspeed, accel);
		const SlideSettings &s = Sl();
		const int now = SlAt<int32_t>(pm, kSlPmTime);
		const bool in = SlInPhase(ps, now, s);
		const int fm = in ? 127 : 0, rm = static_cast<int8_t>(pm[kSlPmRight]);
		if (!fm && !rm)
		{
			const float before = SlLen2(&SlAt<float>(ps, kSlPsVel));
			g_slAccel(ps, pml, wishdir, wishspeed, accel);
			if (s.debug >= 3)
				Log("slide: probe accel (BO3's wish %.2f, dir %.2f %.2f %.2f, accel %.2f): xy %.2f -> %.2f", wishspeed, wishdir[0],
					wishdir[1], wishdir[2], accel, before, SlLen2(&SlAt<float>(ps, kSlPsVel)));
			return;
		}
		const float len = sqrtf(static_cast<float>(fm * fm + rm * rm));
		const int arm = rm < 0 ? -rm : rm;
		const float k = static_cast<float>(fm > arm ? fm : arm) / len;
		float gun = 1.0f;
		const uint64_t weapon = SlAt<uint64_t>(ps, kSlPsWeapon);
		if (weapon & 0x1FF)
		{
			gun = g_slMoveScale(ps, weapon);
			if (!(gun > 0.0f && gun < 10.0f))
				gun = 1.0f;
		}
		float moveScale = SlAt<float>(ps, kSlPsMoveScale);
		if (!(moveScale > 0.0f && moveScale < 10.0f))
			moveScale = 1.0f;
		float base = SlAt<int32_t>(ps, kSlPsSpeed) * k / 127.0f * s.sprintSpeedScale * s.crouchSpeedScale * gun * moveScale;
		if (in)
			base *= powf(1.0f - s.subsequentScale, static_cast<float>(SlAt<int32_t>(ps, kSlPsCount))) * s.inMaxSpeedScale;
		const float *f = &SlAt<float>(pml, kSlPmlForward), *r = &SlAt<float>(pml, kSlPmlRight);
		float vec[3];
		for (int i = 0; i < 3; i++)
			vec[i] = f[i] * (fm * base) + r[i] * (rm * base * s.strafeScale);
		vec[2] = 0;
		const float ws = SlLen3(vec);
		if (ws < 1e-4f)
			return g_slAccel(ps, pml, wishdir, wishspeed, accel);
		float dir[3] = {vec[0] / ws, vec[1] / ws, 0};
		SlProject(dir, &SlAt<float>(pml, kSlPmlNormal));
		const float a = in && s.inAcceleration > 0 ? s.inAcceleration : accel;
		if (s.debug >= 2)
			Log("slide: accel %s c%d t=%d %s fm %d rm %d wish %.1f (BO3 %.1f) accel %.2f (BO3 %.2f) gun %.3f n %d xy %.1f",
			    SlSide(pm), SlAt<int32_t>(ps, kSlPsClient), now, in ? "IN" : "strafe", fm, rm, ws, wishspeed, a, accel, gun,
			    SlAt<int32_t>(ps, kSlPsCount), SlLen2(&SlAt<float>(ps, kSlPsVel)));
		// BO3's PM_Accelerate clamps |v| to the wish speed while the sprint flag (0x20) is set on the ground (a dvar-gated
		// rule); IW8 has no such clamp in a slide, and a strafe wish (~40 u/s) would stop it dead. Not sprinting for the call.
		uint64_t &flags = SlAt<uint64_t>(ps, kSlPsFlags);
		const uint64_t sprint = flags & 0x20;
		flags &= ~0x20ull;
		g_slAccel(ps, pml, dir, ws, a);
		flags |= sprint;
		if (s.debug >= 3)
			Log("slide: probe accel (ours) -> xy %.2f", SlLen2(&SlAt<float>(ps, kSlPsVel)));
	}

	// After PM_WalkMove's PM_StepSlideMove (the last velocity change of the walk move).
	// BO3 truncates ps->velocity to whole units (toward zero, per axis) after every Pmove step: in the slide probe run the
	// next frame always starts from integer components (82.41 -> 82, 77.55 -> 77, 190.28 -> 190). That costs ~0.5 u/s per
	// axis per Pmove step, i.e. ~0.7 u/s per step on a diagonal; at 2-5 ms steps that is 150-300 u/s^2, more than the IW8
	// slide friction itself (the coast ended at ~50 u/s instead of ~120). BO3's own slide hides it (a 625 push, friction
	// 0.15); IW8 keeps full precision. slide_snap_round=1: while sliding, +0.5 toward the sign of each horizontal component
	// here, so the engine's truncation becomes round-to-nearest (no bias, the same snapped values on server and client).
	void SlStepHook(uint8_t *pm, uint8_t *pml, int a3)
	{
		uint8_t *ps = SlAt<uint8_t *>(pm, kSlPmPs);
		const SlideSettings &s = Sl();
		const bool sliding = (SlAt<uint64_t>(ps, kSlPsFlags) & kPmfSliding) != 0;
		float v0[3];
		memcpy(v0, &SlAt<float>(ps, kSlPsVel), sizeof(v0));
		g_slStep(pm, pml, a3);
		float *vel = &SlAt<float>(ps, kSlPsVel);
		if (sliding && s.snapRound)
			for (int i = 0; i < 2; i++)
				if (vel[i] != 0.0f)
					vel[i] += vel[i] > 0 ? 0.5f : -0.5f;
		if (s.debug < 3 || !sliding)
			return;
		const float *v = vel;
		t_sl.probeXy = SlLen2(v);
		t_sl.probeTime = SlAt<int32_t>(pm, kSlPmTime);
		Log("slide: probe step-slide %s c%d t=%d: v %.2f %.2f %.2f -> %.2f %.2f %.2f (xy %.2f -> %.2f) ground %d normal z %.4f",
			SlSide(pm), SlAt<int32_t>(ps, kSlPsClient), t_sl.probeTime, v0[0], v0[1], v0[2], v[0], v[1], v[2], SlLen2(v0), t_sl.probeXy,
			SlAt<int32_t>(ps, kSlPsGroundEnt), SlAt<float>(pml, kSlPmlNormal + 8));
	}

	// PM_CmdScale: BO3's post-slide 0.01 -> 1 speed ramp (ps+0x848 = slideOutDuration) reads 0 for the call.
	float SlCmdScaleHook(uint8_t *pm)
	{
		uint8_t *ps = SlAt<uint8_t *>(pm, kSlPmPs);
		int32_t &dur = SlAt<int32_t>(ps, kSlPsOutDur);
		const int32_t saved = dur;
		dur = 0;
		const float r = g_slCmdScale(pm);
		dur = saved;
		return r;
	}

	// The sprint start check: no sprint within sprintDelayMs of a slide's end (IW8 Slide_IsInSprintDelay).
	uint64_t SlSprintHook(uint8_t *pm, uint8_t *a2, uint8_t *ps)
	{
		const SlideSettings &s = Sl();
		if (s.sprintLock && ps && SlEndedRecently(ps, SlAt<int32_t>(pm, kSlPmTime), s.sprintDelayMs))
			return 0;
		return g_slSprint(pm, a2, ps);
	}

	// ---- live tuning (own watcher; the cfg is re-read for slide_ keys only) --------------------------------------------
	void SlPublish(const SlideSettings &s)
	{
		g_sl.store(new SlideSettings(s), std::memory_order_release);  // old snapshots are leaked on purpose (tiny, rare)
	}

	void SlReload(const char *why)
	{
		char *text = ReadCfgText(g_slCfgPath);
		if (!text)
			return;
		SlideSettings s;
		char line[512];
		int bad = 0;
		for (const char *p = text; (p = NextCfgLine(p, line, sizeof(line))) != nullptr;)
		{
			line[strcspn(line, "\r\n")] = 0;
			if (strncmp(line, "slide_", 6) || !strncmp(line, "slide_enable=", 13) || !strncmp(line, "slide_gesture_off=", 18))
				continue;
			bool known;
			if (!SlParseSetting(s, line, known) || !known)
			{
				bad++;
				Log("slide: live reload: bad line '%s' (ignored)", line);
			}
		}
		free(text);
		SlPublish(s);
		Log("slide: %s: suit %s, max %d ms, in %d ms x%.2f accel %.2f, friction %.2f/%.2f/%.2f/%.2f, out %d ms %.2f->%.2f, "
		    "air %d ms %.2f->%.2f, strafe %.2f, jump %.2f (grace %d ms), speeds %.0f/%.0f, subsequent %d ms %.2f, sprint lock %s "
		    "%d ms, ads ends %s, camera %s (%.0f deg / %d ms), view %s (%.1f %.1f %.1f, %d/%d ms, interp %d), gesture %s, debug %d%s",
		    why, s.suitName, s.maxTimeMs, s.inTimeMs, s.inMaxSpeedScale, s.inAcceleration, s.fricNormal, s.fricDownhill, s.fricUphill,
		    s.fricBlocked, s.outTimeMs, s.outFricStart, s.outFricFinish, s.inAirTimeMs, s.inAirFricStart, s.inAirFricFinish,
		    s.strafeScale, s.jumpScale, s.lateJumpGraceMs, s.minStartSpeed, s.minContinueSpeed, s.subsequentTime, s.subsequentScale,
		    s.sprintLock ? "on" : "off", s.sprintDelayMs, s.adsEnds ? "on" : "off", s.camera ? "on" : "off", s.cameraPitch,
		    s.cameraTimeMs, s.view ? "on" : "off", s.viewAngles[0], s.viewAngles[1], s.viewAngles[2], s.viewBlendInMs,
		    s.viewBlendOutMs, s.viewInterp, s.gesture ? "on" : "off", s.debug, bad ? " (bad lines ignored)" : "");
	}

	DWORD WINAPI SlWatchThread(void *)
	{
		SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
		for (;;)
		{
			Sleep(500);
			WIN32_FILE_ATTRIBUTE_DATA a;
			if (!GetFileAttributesExA(g_slCfgPath, GetFileExInfoStandard, &a) || CompareFileTime(&a.ftLastWriteTime, &g_slCfgTime) == 0)
				continue;
			g_slCfgTime = a.ftLastWriteTime;
			SlReload("live reload");
		}
	}

	// ---- install -----------------------------------------------------------------------------------------------------
	bool SlSiteOk(const SlSite &site)
	{
		if (site.call + 5 > kSlImageSize || site.target >= kSlImageSize)
			return false;
		uint8_t *p = At<uint8_t>(site.call);
		if (!FastReadable(p, 5) || p[0] != 0xE8)
			return false;
		int32_t rel;
		memcpy(&rel, p + 1, 4);
		return p + 5 + rel == At<uint8_t>(site.target);
	}

	void InstallSlide()
	{
		if (WtDebugSkip("slide"))
			return;
		if (g_slHooked)
			return;
		if (!g_slEnable)
		{
			Log("slide: slide_enable=%s: MW slide not installed (BO3's slide unchanged)", g_slSeenEnable ? "0" : "missing");
			return;
		}
		if (!WtExeSupported())
		{
			Log("slide: unknown exe; not installed");
			return;
		}
		auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(g_base);
		auto *nt = reinterpret_cast<IMAGE_NT_HEADERS *>(g_base + dos->e_lfanew);
		if (nt->OptionalHeader.SizeOfImage != kSlImageSize)
		{
			Log("slide: image size 0x%x, expected 0x%zx (another build); not installed", nt->OptionalHeader.SizeOfImage,
			    kSlImageSize);
			return;
		}
		for (const SlSite &site : kSlSites)
			if (!SlSiteOk(site))
			{
				Log("slide: +%zx (%s) isn't a call to +%zx (another build, or already hooked); not installed", site.call, site.what,
				    site.target);
				return;
			}
		if (!FastReadable(At<uint8_t>(kSlMoveScale), 16))
		{
			Log("slide: +%zx not readable; not installed", kSlMoveScale);
			return;
		}
		auto *stub = static_cast<uint8_t *>(AllocNear(reinterpret_cast<uintptr_t>(At<uint8_t>(kSlUpdate)), 0x1000));
		if (!stub)
		{
			Log("slide: no memory near the exe; not installed");
			return;
		}
		SlPublish(g_slParse);
		g_slUpdate = reinterpret_cast<SlUpdateFn>(At<uint8_t>(kSlUpdate));
		g_slEndCheck = reinterpret_cast<SlPmFn>(At<uint8_t>(kSlEndCheck));
		g_slCanSlide = reinterpret_cast<SlUpdateFn>(At<uint8_t>(kSlCanSlide));
		g_slJump = reinterpret_cast<SlUpdateFn>(At<uint8_t>(kSlCheckJump));
		g_slFriction = reinterpret_cast<SlFrictionFn>(At<uint8_t>(kSlFriction));
		g_slAccel = reinterpret_cast<SlAccelFn>(At<uint8_t>(kSlAccelerate));
		g_slCmdScale = reinterpret_cast<SlCmdScaleFn>(At<uint8_t>(kSlCmdScale));
		g_slSprint = reinterpret_cast<SlSprintFn>(At<uint8_t>(kSlSprintOk));
		g_slStep = reinterpret_cast<SlStepFn>(At<uint8_t>(kSlStepSlide));
		g_slMoveScale = reinterpret_cast<SlMoveScaleFn>(At<uint8_t>(kSlMoveScale));
		void *hooks[kShCount] = {reinterpret_cast<void *>(&SlUpdateHook),   reinterpret_cast<void *>(&SlEndCheckHook),
		                         reinterpret_cast<void *>(&SlCanSlideHook), reinterpret_cast<void *>(&SlJumpHook),
		                         reinterpret_cast<void *>(&SlFrictionHook), reinterpret_cast<void *>(&SlAccelHook),
		                         reinterpret_cast<void *>(&SlCmdScaleHook), reinterpret_cast<void *>(&SlSprintHook),
	                         reinterpret_cast<void *>(&SlStepHook)};
		for (int h = 0; h < kShCount; h++)
		{
			memcpy(stub + 16 * h, "\xFF\x25\x00\x00\x00\x00", 6);  // jmp [rip] -> hook
			memcpy(stub + 16 * h + 6, &hooks[h], 8);
		}
		FlushInstructionCache(GetCurrentProcess(), stub, 16 * kShCount);
		for (size_t i = 0; i < _countof(kSlSites); i++)
		{
			uint8_t *site = At<uint8_t>(kSlSites[i].call);
			int32_t newRel = static_cast<int32_t>(stub + 16 * kSlSiteHook[i] - (site + 5));
			DWORD old;
			VirtualProtect(site + 1, 4, PAGE_EXECUTE_READWRITE, &old);
			memcpy(site + 1, &newRel, 4);
			VirtualProtect(site + 1, 4, old, &old);
			FlushInstructionCache(GetCurrentProcess(), site, 5);
		}
		g_slHooked = true;
		g_slideVmFrame = &SlideViewmodelFrame;
		Log("slide: MW2019 slide installed: %zu call sites hooked; %d slide_gesture_off line(s)", _countof(kSlSites), g_slOffCount);
		for (const SlSite &site : kSlSites)
			Log("slide:   +%zx %s", site.call, site.what);
		if (g_cfgPath[0])
		{
			strcpy_s(g_slCfgPath, g_cfgPath);
			WIN32_FILE_ATTRIBUTE_DATA a;
			if (GetFileAttributesExA(g_slCfgPath, GetFileExInfoStandard, &a))
				g_slCfgTime = a.ftLastWriteTime;
			SlReload("settings");
			if (HANDLE t = CreateThread(nullptr, 0, SlWatchThread, nullptr, 0, nullptr))
				CloseHandle(t);
		}
		else
		{
			const SlideSettings &s = Sl();
			Log("slide: settings from the baked cfg (no live tuning): suit %s, max %d ms, debug %d", s.suitName, s.maxTimeMs, s.debug);
		}
		SlSwapNativeSlots();  // the variants registered so far (the rest: from the viewmodel hook, every 120 frames)
	}

	// ---- viewmodel gesture layer ---------------------------------------------------------------------------------------
	enum SlPhase { kSpIdle, kSpIn, kSpLoop, kSpOut };
	const char *const kSlPhaseNames[] = {"idle", "in", "loop", "out"};

	struct SlVm
	{
		int variant = -1;
		void *dobj = nullptr, *tree = nullptr;
		unsigned builds = 0;
		bool ok = false;
		const char *why = "";
		int frames[3] = {};  // intervals per leaf (frames - 1), from the leaf's length and slide_gesture_fps
		SlPhase phase = kSpIdle;
		double phaseT = 0, last = -1;
		float rootW = 0, leafW[3] = {}, gate = 0;
		double leafT[3] = {};  // each leaf's own clock (a leaf fading out keeps running: the loop keeps looping)
		bool wasSliding = false, writing = false;
		int recheck = 0;
	} g_slVm;

	// Patched variants (by pointer): BO3's own slide anims -> the idle, for guns with the gesture layer.
	const uint8_t *g_slVariantSeen[512];

	bool SlWeaponOff(const char *name)
	{
		for (int i = 0; i < g_slOffCount; i++)
			if (!strcmp(g_slOff[i], name))
				return true;
		return false;
	}

	bool SlEndsWith(const char *s, const char *suffix)
	{
		size_t a = strlen(s), b = strlen(suffix);
		return a >= b && !_stricmp(s + a - b, suffix);
	}

	// One pass over the variant table: for each new variant whose gesture slots hold names of their own, point BO3's slide
	// anim slots at the idle (the tree loads slot names when it is built, on a raise).
	void SlSwapNativeSlots()
	{
		const SlideSettings &s = Sl();
		if (!s.gesture || !s.swapNative)
			return;
		uint8_t **variants = At<uint8_t *>(kWeaponVariants);
		ReadCache rc;
		for (int v = 1; v < 512; v++)
		{
			uint8_t *variant = variants[v];
			if (!variant || g_slVariantSeen[v] == variant)
				continue;
			if (!rc.Readable(variant, kVariantAnims + 8))
				continue;
			g_slVariantSeen[v] = variant;
			const char *name = *reinterpret_cast<const char *const *>(variant);
			const char **anims = *reinterpret_cast<const char ***>(variant + kVariantAnims);
			if (!name || !anims || rc.Span(name) < 2 || !rc.Readable(anims, sizeof(char *) * kNumWeapAnims) || SlWeaponOff(name))
				continue;
			const char *idle = anims[1];
			auto own = [&](uint32_t n) {
				const char *a = anims[n];
				return a && rc.Span(a) >= 2 && a[0] && a != idle && !(idle && rc.Span(idle) >= 2 && !strcmp(a, idle));
			};
			if (!idle || !own(s.nodes[0]) || !own(s.nodes[1]) || !own(s.nodes[2]))
				continue;
			int swapped = 0;
			for (int n = 2; n < kNumWeapAnims; n++)
			{
				if (n == static_cast<int>(s.nodes[0]) || n == static_cast<int>(s.nodes[1]) || n == static_cast<int>(s.nodes[2]))
					continue;
				const char *a = anims[n];
				if (!a || a == idle || rc.Span(a) < 2 || !a[0])
					continue;
				if (SlEndsWith(a, "slide_in") || SlEndsWith(a, "slide_loop") || SlEndsWith(a, "slide_out") ||
				    SlEndsWith(a, "slide_air_in") || SlEndsWith(a, "slide_in_air"))
				{
					Log("slide: %s (variant %d): slot %d '%s' -> the idle '%s' (gesture layer on; from the next raise)", name, v, n, a, idle);
					anims[n] = idle;
					swapped++;
				}
			}
			Log("slide: %s (variant %d): gesture slots %u '%s', %u '%s', %u '%s'; %d native slide slot(s) swapped", name, v, s.nodes[0],
			    anims[s.nodes[0]], s.nodes[1], anims[s.nodes[1]], s.nodes[2], anims[s.nodes[2]], swapped);
		}
	}

	bool SlStateBlocks(int state)
	{
		return (state >= 1 && state <= 5) || (state >= 12 && state <= 51);  // raise / drop / reload / melee / offhand
	}

	float SlApproach(float v, float target, float step)
	{
		return v < target ? (v + step > target ? target : v + step) : (v - step < target ? target : v - step);
	}

	void SlLeafTime(void *dobj, uint32_t node, int intervals, double elapsed, bool loop, const SlideSettings &s)
	{
		const int first = s.gestureRef ? 1 : 0;
		const int span = intervals - first;
		if (span <= 0)
			return;
		double frame = elapsed * s.gestureFps;
		frame = loop ? fmod(frame, static_cast<double>(span)) : (frame > span ? span : frame);
		float t = static_cast<float>((first + frame) / intervals);
		if (t > 0.9999f)
			t = 0.9999f;
		LocoSetTime(dobj, node, t);
	}

	// Per frame, from the viewmodel hook (bo3_additive.h AfterViewWeaponAnim), after the engine's own writes.
	void SlideViewmodelFrame(uint8_t *ps, uint8_t *vm, double now)
	{
		if (!g_slHooked)
			return;
		const SlideSettings &s = Sl();
		SlVm &g = g_slVm;
		if (++g.recheck % 120 == 1)
			SlSwapNativeSlots();
		void *dobj = *reinterpret_cast<void **>(vm + kVmDObj);
		void *tree = dobj ? *reinterpret_cast<void **>(dobj) : nullptr;
		if (!tree || !s.gesture)
		{
			if (g.writing && tree && dobj == g.dobj && tree == g.tree)
				Engine().setGoal(dobj, kSlRoot, 0.0f, 0.0f, 1.0f, 0, 0, 0);
			g.writing = false;
			g.variant = -1;
			return;
		}
		float dt = g.last < 0 ? 0.0f : static_cast<float>(now - g.last);
		dt = dt < 0 ? 0 : dt > 0.1f ? 0.1f : dt;
		g.last = now;
		const uint64_t weapon = *reinterpret_cast<uint64_t *>(ps + kPsWeapon);
		const int variant = static_cast<int>(weapon & 0x1FF);
		if (variant != g.variant || dobj != g.dobj || tree != g.tree || g.builds != g_treeBuilds)
		{
			const bool newGun = variant != g.variant;
			g.variant = variant;
			g.dobj = dobj;
			g.tree = tree;
			g.builds = g_treeBuilds;
			g.ok = false;
			g.why = "";
			g.writing = false;
			if (newGun)
			{
				g.phase = kSpIdle;
				g.rootW = g.gate = 0;
				g.leafW[0] = g.leafW[1] = g.leafW[2] = 0;
			}
			uint8_t *vdef = variant ? At<uint8_t *>(kWeaponVariants)[variant] : nullptr;
			const char *name = vdef && FastReadable(vdef, 8) ? *reinterpret_cast<const char *const *>(vdef) : nullptr;
			ReadCache rc;
			if (!variant || !name)
				g.why = "no weapon";
			else if (SlWeaponOff(name))
				g.why = "slide_gesture_off";
			else if (Engine().isDualWield(weapon))
				g.why = "dual wield";
			else
			{
				const LocoWeapon *loco = FindLocoForVariant(variant);
				if (loco && loco->jog)
					g.why = "its locomotion jog uses root 184";
				else if (loco && loco->idle && !loco->idleRefused && loco->idleNode >= 185 && loco->idleNode <= 192)
					g.why = "its idle_active uses root 184";
				else
				{
					bool all = true;
					for (int i = 0; i < 3; i++)
					{
						const float freq = UsableAdditiveLeaf(dobj, s.nodes[i], &rc) ? LeafFrequency(dobj, s.nodes[i], &rc) : 0.0f;
						g.frames[i] = freq > 0 ? static_cast<int>(s.gestureFps / freq + 0.5f) : 0;
						all &= g.frames[i] >= (s.gestureRef ? 2 : 1);
					}
					g.ok = all;
					if (!all)
						g.why = "gesture leaves hold no anims of their own (blank slot = idle)";
				}
			}
			static int s_logs;
			if (name && s_logs++ < 200)
				Log("slide: gesture layer %s for %s (variant %d)%s%s; frames in %d loop %d out %d", g.ok ? "ON" : "off", name, variant,
				    g.ok ? "" : ": ", g.why, g.frames[0], g.frames[1], g.frames[2]);
		}
		if (!g.ok)
			return;

		const uint64_t pmFlags = *reinterpret_cast<uint64_t *>(ps + kPsPmFlags);
		const bool sliding = (pmFlags & kPmfSliding) != 0;
		const int state = *reinterpret_cast<int32_t *>(ps + kPsWeaponState);
		const int pmType = *reinterpret_cast<int32_t *>(ps + kSlPsPmType);
		const float ads = SlClamp01(*reinterpret_cast<float *>(ps + kPsAdsFraction));
		const bool juke = (pmFlags & kPmfJuke) != 0;
		if (juke)
		{
			g.writing = false;  // the engine owns the juke group while juking
			g.phase = kSpIdle;
			g.rootW = 0;
			return;
		}

		// phases: in from the slide's start, then the loop; the out from its end
		const SlPhase before = g.phase;
		if (sliding && !g.wasSliding)
			g.phase = kSpIn, g.phaseT = 0, g.leafT[0] = 0;
		else if (!sliding && g.wasSliding && (g.phase == kSpIn || g.phase == kSpLoop))
			g.phase = kSpOut, g.phaseT = 0, g.leafT[2] = 0;
		g.wasSliding = sliding;
		g.phaseT += dt;
		for (double &t : g.leafT)
			t += dt;
		const int first = s.gestureRef ? 1 : 0;
		auto length = [&](int i) { return (g.frames[i] - first) / s.gestureFps; };
		if (g.phase == kSpIn && g.phaseT >= length(0))
			g.phase = kSpLoop, g.phaseT -= length(0), g.leafT[1] = g.phaseT;
		if (g.phase == kSpOut && g.phaseT >= length(2))
			g.phase = kSpIdle;
		const bool gate = g.phase != kSpIdle && pmType == 0 && !SlStateBlocks(state);
		g.gate = SlApproach(g.gate, gate ? 1.0f : 0.0f, dt / (gate ? s.blendIn : s.blendOut));
		g.rootW = g.gate * (1.0f - ads) * s.gestureWeight;
		const int leaf = g.phase == kSpIn ? 0 : g.phase == kSpLoop ? 1 : 2;
		for (int i = 0; i < 3; i++)
			g.leafW[i] = SlApproach(g.leafW[i], i == leaf ? 1.0f : 0.0f, before == kSpIdle ? 1.0f : dt / s.blendCross);
		if (s.debug && g.phase != before)
			Log("slide: gesture %s -> %s (state %d, ads %.2f, root %.2f)", kSlPhaseNames[before], kSlPhaseNames[g.phase], state, ads,
			    g.rootW);

		if (g.rootW < 0.001f && g.phase == kSpIdle)
		{
			if (g.writing)
				Engine().setGoal(dobj, kSlRoot, 0.0f, 0.0f, 1.0f, 0, 0, 0);
			g.writing = false;
			return;
		}
		g.writing = true;
		Engine().setGoal(dobj, kSlRoot, g.rootW, 0.0f, 1.0f, 0, 0, 0);
		for (int i = 0; i < 3; i++)
		{
			Engine().setGoal(dobj, s.nodes[i], g.leafW[i], 0.0f, 0.0f, 0, 0, 0);
			if (g.leafW[i] < 0.001f)
				continue;
			SlLeafTime(dobj, s.nodes[i], g.frames[i], g.leafT[i], i == 1, s);
		}
	}
}
