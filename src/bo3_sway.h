// MW19 (IW8) first-person weapon sway for BO3 weapons (included at the end of bo3_additive.h, after bo3_locomotion.h).
// Research and formulas: xpakcap\additives\sway\REPORT_sway.md; reference evaluator: sway_sim.py next to it (this is a
// straight port of its AdvancedSway, Idle, Base stance pivots and AdsGunBob). IW8 sources: OpenIW8 cg_view_motion.cpp
// (AdvancedSwayState / Deadzone / GunDir / Springs), bg_weapons_view.cpp (_IdleAngles, _MovementTiltAngles, _Base),
// cg_weapons.cpp (CG_CalculateWeaponMovement_CalcAngles pivots), KisakCOD com_math.cpp (LinearTrack, graphs).
//
// What runs where:
//   CalcWeaponPosHook (CG_CalculateWeaponPosition, once per rendered frame): update, then add to the gun's view-space
//     angles (pitch, yaw, roll) and origin (forward, left, up) before BO3 places it.
//     - advanced hip sway: the arms (torso) lag the camera by a deadzone that opens with turn speed, on an implicit
//       mass-spring; the gun points into the turn on a second spring, rotating about gunPivotPoint, roll = yaw x
//       yawToRoll; both weighted by (1 - ADS). Firing narrows the deadzone and hands the gun to the torso.
//     - advanced idle: two sine settings (pitch/yaw/roll at 0.001/0.0007/0.0005 rad per idle ms), a forward push and a
//       rotation about a point <rot> ahead, magnitudes and speed lerped hip -> ADS; float idle time (IW8 truncates to
//       whole ms per frame, which runs slow at high frame rates); stance factor tracked at 0.5/s.
//     - stance: vStandOfs, vStandOfsRot about its pivot, vDuckedOfsRot about its pivot smoothed at fDuckedOfsRotRate;
//       all x (1 - ADS). (Moves, strafe, ducked/prone offsets and rates are BO3's own GDT fields: phase 1.)
//     - ADS gun bob + movement tilt, from BO3's bob cycle (ps +0xC) and speed ratio, tilt about (tiltOffset, 0, 0). The
//       cycle is followed across wraps and across the engine's restart from 0 after a stop (the phase carries on, so
//       the bob doesn't pop at stops and quick reversals); the crouch factor blends at the bob's transition rate.
//   ViewAxisHook (refdef view axis, visual only): the idle's camera part (ViewMagnitudeX/Y x ADS), its idle clocks run
//     on to the hook's own time (it runs before the frame's gun update).
//   FireRecoilHook: the fire timestamp for the fire fraction.
// Timing: one step per game frame. dt is the game's frame time from cg time (cg+0x11A88C, whole ms: the usercmd clock,
// so the interval the view delta covers; IW8 likewise uses its input frame time) while it agrees with the wall clock,
// else QueryPerformanceCounter. A second call in the same frame (same cg time and view angles) re-applies the last
// result. Springs are implicit, filters dt/duration, and frames longer than 1/240 s are substepped with interpolated
// inputs, so 30..240 fps give the same motion (sway harness). Jitter against a continuous reference: jitter.cpp in
// additives\sway\harness (1000 Hz mouse, jittery frame times, double calls, BO3's bob truncation).
//
// Config (stub_boot.cfg), one weapon per line; <w> may be * on sway_graph (all weapons). A weapon without lines of its
// own takes those of its wop_alias / sway_alias source (so PaPs follow their base unless they have their own).
//   sway_adv=<w>,torsoSmooth,fireTorsoSmooth,torsoViewSmoothMs,dzAdjP,dzAdjY,dzSpeedP,dzSpeedY,dzMaxP,dzMaxY,
//            tMassP,tMassY,tSpringP,tSpringY,tDampP,tDampY           (swaySettings.adv torso part; turns advanced sway on)
//   sway_advgun=<w>,gunViewSmoothMs,speedP,speedY,offP,offY,gMassP,gMassY,gSpringP,gSpringY,gDampP,gDampY,
//            pivotF,pivotL,pivotU,yawToRoll                           (gun direction part)
//   sway_advfire=<w>,fireMs,startBlendMs,finishBlendMs,fireDeadzoneScale,fireTorsoToGunScale
//   sway_graph=<w|*>,<0 deadzone|1 gun>,x0,y0,x1,y1,...               (RumbleGraph knots, up to 16; default linear)
//   sway_idle=<w>,<1|2>,hipSpeed,adsSpeed,hipP,hipY,hipR,hipF,hipRot,adsP,adsY,adsR,adsF,adsRot,hipViewP,hipViewY,
//            adsViewP,adsViewY                                         (AdvancedIdleSettings, one line per setting)
//   sway_idlemisc=<w>,crouchFactor,proneFactor,breathGasp             (fIdleCrouch/ProneFactor; gasp is logged only)
//   sway_stance=<w>,standOfsF,L,U,standRotP,Y,R,standPivotF,L,U,duckRotP,Y,R,duckPivotF,L,U,duckRotRate
//   sway_adsbob=<w>,pitch,yaw,tiltP,tiltY,tiltR,tiltOffset,crouchFactor   (fAdsGunBob*)
//   sway_alias=<w>,<source>
// Globals (live: edit the cfg while the game runs; per-weapon lines are live too):
//   sway_enable=<0|1>                    everything on / off (default 1)
//   sway_parts=<adv>,<idle>,<stance>,<adsbob>,<camera idle>    0/1 each (default 1,1,1,1,1)
//   sway_scale=<torso>,<gun>,<idle>,<adsbob>                   multipliers (default 1,1,1,1)
//   sway_smoothing=<0|1>[,<clamp deg>]   AngularSmoothing of the view before the deadzone (IW8: gamepads; default 0: with a
//                                        mouse it adds the raw frame steps back into the torso goal, measured no smoother)
//   sway_advparts=<torso spring>,<gun dir>,<gun spring>        advancedSway*Enabled dvars (default 1,1,1)
//   sway_gunsign=<1|-1>                  gun direction leads (1) or lags (-1) the turn (sign unconfirmed; default 1)
//   sway_gunbobmax=<f>                   bg_gunBobMax (IW8 default unknown; 1)
//   sway_gunbobtrans=<in s>,<out s>      bg_gunBobTransIn/OutTime (unknown; 0.25,0.25)
//   sway_boblag=<f>                      bg_weaponBobLag (IW4 0.25)
//   sway_idlefwd=<time scale>,<mag scale>  AdvancedIdleForwardMotion constants (unknown; 0.001,0.01)
//   sway_idleview=<pitch>,<yaw>          camera idle time scales (classic path's 0.001,0.0007)
//   sway_substep=<Hz>                    frames longer than 1/Hz are integrated in substeps (default 240; 0 = once per
//                                        frame, as IW8 does)
//   sway_clock=<game|qpc>                dt from cg time (default) or the wall clock (as before 2026-09-30)
//   sway_bobfilter=<Hz>                  tracking filter on the bob cycle for the ADS bob (default 0 = the cycle as is;
//                                        4 measured no better: BO3's per-frame steps are even enough at 60-240 fps)
//   sway_debug=<0|1>                     2 Hz trace of every part in the log
#pragma once

namespace
{
	struct SwayGraph
	{
		int n = 0;  // knots; 0 = linear
		float x[16], y[16];
	};
	struct SwayIdleSet
	{
		bool on = false;
		float hipSpeed = 0, adsSpeed = 0;
		float hip[5] = {}, ads[5] = {};  // pitch, yaw, roll magnitudes (x0.01 deg), forward push, rotation offset (in)
		float hipView[2] = {}, adsView[2] = {};
	};
	struct SwayWeapon
	{
		char weapon[64];
		bool adv = false, gun = false, fire = false, stance = false, adsBob = false;
		// sway_adv
		float torsoSmooth = 60, fireTorsoSmooth = 70, torsoViewSmoothMs = 20;
		float dzAdj[2] = {2000, 2000}, dzSpeed[2] = {250, 250}, dzMax[2] = {}, tMass[2] = {1, 1}, tSpring[2] = {100, 100},
		      tDamp[2] = {10, 10};
		// sway_advgun
		float gunViewSmoothMs = 20, gSpeed[2] = {250, 250}, gOff[2] = {}, gMass[2] = {1, 1}, gSpring[2] = {100, 100},
		      gDamp[2] = {10, 10}, pivot[3] = {}, yawToRoll = 0;
		// sway_advfire
		float fireMs = 300, fireStartMs = 200, fireFinishMs = 200, fireDzScale = 0.5f, fireToGun = 0.5f;
		SwayGraph graph[2];
		bool hasGraph[2] = {};
		SwayIdleSet idle[2];
		float crouchFactor = 1, proneFactor = 1, gasp = 0;
		float standOfs[3] = {}, standRot[3] = {}, standPivot[3] = {}, duckRot[3] = {}, duckPivot[3] = {}, duckRotRate = 0;
		float bobPitch = 0, bobYaw = 0, tilt[3] = {}, tiltOffset = 0, bobCrouch = 1;
		int variant = -1;
	};
	constexpr int kMaxSways = 160;
	SwayWeapon g_sways[kMaxSways];
	int g_swayCount;
	struct SwayAlias
	{
		char weapon[64], source[64];
	};
	SwayAlias g_swayAliases[kMaxSways];
	int g_swayAliasCount;

	struct SwayGlobals
	{
		bool enable = true;
		bool parts[5] = {true, true, true, true, true};  // adv, idle, stance, adsbob, camera idle
		float scale[4] = {1, 1, 1, 1};                    // torso, gun, idle, adsbob
		bool smoothing = false;
		float smoothClamp = 0;
		bool torsoSpring = true, gunDir = true, gunSpring = true;
		float gunSign = 1;
		float gunBobMax = 1, gunBobIn = 0.25f, gunBobOut = 0.25f, bobLag = 0.25f;
		float idleFwdTime = 0.001f, idleFwdMag = 0.01f;
		float idleViewTime[2] = {0.001f, 0.0007f};
		SwayGraph graph[2];
		bool hasGraph[2] = {};
		float substepHz = 240;
		bool debug = false;
		bool gameClock = true;  // sway_clock=game: dt from cg time, the usercmd clock the view angles move on (qpc: wall clock)
		float bobHz = 0;        // sway_bobfilter=<Hz>: tracking filter on BO3's 8-bit bob cycle for the ADS gun bob (0 = raw)
		bool frameId = true;    // a second call in the same frame (same cg time and view) re-applies instead of stepping
		bool camLead = true;    // the camera idle is evaluated at the view hook's own time, not the last gun update's
	} g_swayCfg;

	SwayWeapon *FindSway(const char *weapon, bool create)
	{
		for (int i = 0; i < g_swayCount; i++)
			if (strcmp(g_sways[i].weapon, weapon) == 0)
				return &g_sways[i];
		if (!create || g_swayCount >= kMaxSways)
			return nullptr;
		SwayWeapon &w = g_sways[g_swayCount++];
		w = SwayWeapon{};
		strcpy_s(w.weapon, weapon);
		return &w;
	}

	void RecordSwayAlias(const char *weapon, const char *source)
	{
		if (g_swayAliasCount >= kMaxSways || !weapon[0] || !source[0])
			return;
		SwayAlias &a = g_swayAliases[g_swayAliasCount++];
		strcpy_s(a.weapon, weapon);
		strcpy_s(a.source, source);
	}

	bool SwayFloats(char f[][96], int first, int count, float *out)
	{
		for (int i = 0; i < count; i++)
			if (!ParseFloat(f[first + i], out[i]) || !std::isfinite(out[i]))
				return false;
		return true;
	}

	bool SwayParseGraph(char f[][96], int n, SwayGraph &g)
	{
		int knots = (n - 2) / 2;
		if ((n - 2) % 2 || knots < 2 || knots > 16)
			return false;
		SwayGraph t;
		t.n = knots;
		for (int k = 0; k < knots; k++)
			if (!ParseFloat(f[2 + 2 * k], t.x[k]) || !ParseFloat(f[3 + 2 * k], t.y[k]) || (k && t.x[k] < t.x[k - 1]))
				return false;
		g = t;
		return true;
	}

	// sway_* lines and globals. `known` is false for other keys.
	bool ParseSwayLine(const char *line, bool &known)
	{
		known = true;
		char f[24][96];
		auto value = [&](const char *key) -> const char * {
			size_t n = strlen(key);
			return strncmp(line, key, n) == 0 && line[n] == '=' ? line + n + 1 : nullptr;
		};
		const char *v;
		// ---- globals
		if ((v = value("sway_enable")))
		{
			int on;
			return ParseInt(v, on) && ((g_swayCfg.enable = on != 0), true);
		}
		if ((v = value("sway_debug")))
		{
			int on;
			return ParseInt(v, on) && ((g_swayCfg.debug = on != 0), true);
		}
		if ((v = value("sway_parts")))
		{
			float p[5];
			if (SplitFields(v, f, 6) != 5 || !SwayFloats(f, 0, 5, p))
				return false;
			for (int i = 0; i < 5; i++)
				g_swayCfg.parts[i] = p[i] != 0;
			return true;
		}
		if ((v = value("sway_scale")))
			return SplitFields(v, f, 5) == 4 && SwayFloats(f, 0, 4, g_swayCfg.scale);
		if ((v = value("sway_smoothing")))
		{
			int n = SplitFields(v, f, 3), on;
			float clampDeg = 0;
			if (n < 1 || n > 2 || !ParseInt(f[0], on) || (n == 2 && (!ParseFloat(f[1], clampDeg) || clampDeg < 0)))
				return false;
			g_swayCfg.smoothing = on != 0;
			g_swayCfg.smoothClamp = clampDeg;
			return true;
		}
		if ((v = value("sway_advparts")))
		{
			float p[3];
			if (SplitFields(v, f, 4) != 3 || !SwayFloats(f, 0, 3, p))
				return false;
			g_swayCfg.torsoSpring = p[0] != 0;
			g_swayCfg.gunDir = p[1] != 0;
			g_swayCfg.gunSpring = p[2] != 0;
			return true;
		}
		if ((v = value("sway_gunsign")))
		{
			float s;
			return ParseFloat(v, s) && (s == 1 || s == -1) && ((g_swayCfg.gunSign = s), true);
		}
		if ((v = value("sway_gunbobmax")))
			return ParseFloat(v, g_swayCfg.gunBobMax) && g_swayCfg.gunBobMax >= 0;
		if ((v = value("sway_gunbobtrans")))
		{
			float t[2];
			if (SplitFields(v, f, 3) != 2 || !SwayFloats(f, 0, 2, t) || t[0] < 0 || t[1] < 0)
				return false;
			g_swayCfg.gunBobIn = t[0];
			g_swayCfg.gunBobOut = t[1];
			return true;
		}
		if ((v = value("sway_boblag")))
			return ParseFloat(v, g_swayCfg.bobLag);
		if ((v = value("sway_idlefwd")))
		{
			float t[2];
			if (SplitFields(v, f, 3) != 2 || !SwayFloats(f, 0, 2, t))
				return false;
			g_swayCfg.idleFwdTime = t[0];
			g_swayCfg.idleFwdMag = t[1];
			return true;
		}
		if ((v = value("sway_idleview")))
			return SplitFields(v, f, 3) == 2 && SwayFloats(f, 0, 2, g_swayCfg.idleViewTime);
		if ((v = value("sway_substep")))
			return ParseFloat(v, g_swayCfg.substepHz) && g_swayCfg.substepHz >= 0 && g_swayCfg.substepHz <= 2000;
		if ((v = value("sway_clock")))
		{
			if (SplitFields(v, f, 2) != 1 || (strcmp(f[0], "game") != 0 && strcmp(f[0], "qpc") != 0))
				return false;
			g_swayCfg.gameClock = strcmp(f[0], "game") == 0;
			return true;
		}
		if ((v = value("sway_bobfilter")))
			return ParseFloat(v, g_swayCfg.bobHz) && g_swayCfg.bobHz >= 0 && g_swayCfg.bobHz <= 30;

		// ---- per weapon
		static const char *const kKeys[] = {"sway_adv", "sway_advgun", "sway_advfire", "sway_graph", "sway_idle",
		                                    "sway_idlemisc", "sway_stance", "sway_adsbob", "sway_alias"};
		int key = -1;
		for (int i = 0; i < 9 && key < 0; i++)
			if ((v = value(kKeys[i])))
				key = i;
		if (key < 0)
		{
			known = strncmp(line, "sway_", 5) == 0;  // an unknown sway_ key is a bad line, not someone else's
			return !known;
		}
		int n = SplitFields(v, f, 24);
		if (n < 2 || !f[0][0] || strlen(f[0]) >= 64)
			return false;
		if (key == 8)  // sway_alias
		{
			if (n != 2 || !f[1][0])
				return false;
			RecordSwayAlias(f[0], f[1]);
			return true;
		}
		if (key == 3)  // sway_graph
		{
			int which;
			SwayGraph g;
			if (!ParseInt(f[1], which) || which < 0 || which > 1 || !SwayParseGraph(f, n, g))
				return false;
			if (strcmp(f[0], "*") == 0)
			{
				g_swayCfg.graph[which] = g;
				g_swayCfg.hasGraph[which] = true;
				return true;
			}
			SwayWeapon *w = FindSway(f[0], true);
			if (!w)
				return false;
			w->graph[which] = g;
			w->hasGraph[which] = true;
			return true;
		}
		static const int kCounts[] = {15, 15, 5, 0, 17, 3, 16, 7};
		if (n != 1 + kCounts[key])
			return false;
		float x[17];
		if (!SwayFloats(f, 1, kCounts[key], x))
			return false;
		if ((key == 4 && x[0] != 1 && x[0] != 2) || (key == 0 && !(x[9] > 0 && x[10] > 0)) || (key == 1 && !(x[5] > 0 && x[6] > 0)))
			return false;  // setting 1 or 2; masses > 0 (they divide)
		SwayWeapon *w = FindSway(f[0], true);
		if (!w)
			return false;
		auto pair = [](float *dst, const float *src) { dst[0] = src[0], dst[1] = src[1]; };
		auto vec3 = [](float *dst, const float *src) { dst[0] = src[0], dst[1] = src[1], dst[2] = src[2]; };
		switch (key)
		{
		case 0:
			w->adv = true;
			w->torsoSmooth = x[0], w->fireTorsoSmooth = x[1], w->torsoViewSmoothMs = x[2];
			pair(w->dzAdj, x + 3), pair(w->dzSpeed, x + 5), pair(w->dzMax, x + 7);
			pair(w->tMass, x + 9), pair(w->tSpring, x + 11), pair(w->tDamp, x + 13);
			return true;
		case 1:
			w->gun = true;
			w->gunViewSmoothMs = x[0];
			pair(w->gSpeed, x + 1), pair(w->gOff, x + 3), pair(w->gMass, x + 5), pair(w->gSpring, x + 7), pair(w->gDamp, x + 9);
			vec3(w->pivot, x + 11);
			w->yawToRoll = x[14];
			return true;
		case 2:
			w->fire = true;
			w->fireMs = x[0], w->fireStartMs = x[1], w->fireFinishMs = x[2], w->fireDzScale = x[3], w->fireToGun = x[4];
			return true;
		case 4:
		{
			SwayIdleSet &s = w->idle[static_cast<int>(x[0]) - 1];
			s.on = true;
			s.hipSpeed = x[1], s.adsSpeed = x[2];
			memcpy(s.hip, x + 3, sizeof(s.hip));
			memcpy(s.ads, x + 8, sizeof(s.ads));
			pair(s.hipView, x + 13), pair(s.adsView, x + 15);
			return true;
		}
		case 5:
			w->crouchFactor = x[0], w->proneFactor = x[1], w->gasp = x[2];
			return true;
		case 6:
			w->stance = true;
			vec3(w->standOfs, x), vec3(w->standRot, x + 3), vec3(w->standPivot, x + 6), vec3(w->duckRot, x + 9),
			    vec3(w->duckPivot, x + 12);
			w->duckRotRate = x[15];
			return true;
		case 7:
			w->adsBob = true;
			w->bobPitch = x[0], w->bobYaw = x[1];
			vec3(w->tilt, x + 2);
			w->tiltOffset = x[5], w->bobCrouch = x[6];
			return true;
		}
		return false;
	}

	// After the whole cfg: aliases take their source's lines (chains resolve over a few passes).
	void FinishSwayConfig()
	{
		for (int pass = 0; pass < 4; pass++)
		{
			bool added = false;
			for (int a = 0; a < g_swayAliasCount; a++)
			{
				const SwayAlias &al = g_swayAliases[a];
				const SwayWeapon *src = FindSway(al.source, false);
				if (!src || FindSway(al.weapon, false))
					continue;
				SwayWeapon copy = *src;
				SwayWeapon *w = FindSway(al.weapon, true);
				if (!w)
					break;
				*w = copy;
				strcpy_s(w->weapon, al.weapon);
				w->variant = -1;
				added = true;
			}
			if (!added)
				break;
		}
	}

	bool SwayConfigured() { return g_swayCount > 0; }
	bool SwayEnabled() { return g_swayCfg.enable; }

	void LogSwayConfig(const char *when)
	{
		int adv = 0, idle = 0, stance = 0, bob = 0;
		for (int i = 0; i < g_swayCount; i++)
		{
			adv += g_sways[i].adv;
			idle += g_sways[i].idle[0].on || g_sways[i].idle[1].on;
			stance += g_sways[i].stance;
			bob += g_sways[i].adsBob;
		}
		const SwayGlobals &g = g_swayCfg;
		Log("sway: %s%d weapon(s): %d advanced sway, %d idle, %d stance, %d ADS gun bob; %s, parts %d%d%d%d%d, scale %.2f/%.2f/%.2f/%.2f, "
		    "graphs %s/%s, smoothing %d, gun sign %+.0f",
		    when, g_swayCount, adv, idle, stance, bob, g.enable ? "ON" : "OFF (sway_enable=0)", g.parts[0], g.parts[1], g.parts[2],
		    g.parts[3], g.parts[4], g.scale[0], g.scale[1], g.scale[2], g.scale[3], g.hasGraph[0] ? "knots" : "linear",
		    g.hasGraph[1] ? "knots" : "linear", g.smoothing, g.gunSign);
	}

	// ---- math --------------------------------------------------------------------------------------------------------
	float SwayWrap180(float a) { return a - 360.0f * floorf(a / 360.0f + 0.5f); }
	float SwayClamp(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
	float SwayLinearTrack(float tgt, float cur, float rate, float dt)  // LinearTrack (KisakCOD com_math.cpp)
	{
		float err = tgt - cur, step = err > 0 ? rate * dt : -rate * dt;
		if (fabsf(err) <= 0.001f || fabsf(step) > fabsf(err))
			return tgt;
		return cur + step;
	}
	float SwaySmoothFilter(float cur, float target, float durationMs, float dt)  // cur += (target - cur) * dt / duration
	{
		float k = durationMs * 0.001f / dt;
		return k <= 1.0f ? target : cur + (target - cur) / k;
	}
	float SwayGraphValue(const SwayGraph &g, float frac)  // GraphGetValueFromFraction; no knots = linear
	{
		frac = SwayClamp(frac, 0.0f, 1.0f);
		if (g.n < 2)
			return frac;
		if (frac <= g.x[0])
			return g.y[0];
		for (int k = 1; k < g.n; k++)
			if (g.x[k] >= frac)
			{
				float span = g.x[k] - g.x[k - 1];
				float u = span > 1e-6f ? (frac - g.x[k - 1]) / span : 1.0f;
				return g.y[k - 1] + (g.y[k] - g.y[k - 1]) * u;
			}
		return g.y[g.n - 1];
	}
	// dst = P + R(ang) (dst - P): RotatePointAroundPoint with AnglesToAxis rows (forward, left, up).
	void SwayRotateAbout(float dst[3], const float pivot[3], const float ang[3])
	{
		if (fabsf(ang[0]) < 1e-6f && fabsf(ang[1]) < 1e-6f && fabsf(ang[2]) < 1e-6f)
			return;
		float axis[3][3];
		WopAnglesToAxis(ang, axis);
		float d[3] = {dst[0] - pivot[0], dst[1] - pivot[1], dst[2] - pivot[2]};
		for (int j = 0; j < 3; j++)
			dst[j] = pivot[j] + d[0] * axis[0][j] + d[1] * axis[1][j] + d[2] * axis[2][j];
	}

	// ---- state ---------------------------------------------------------------------------------------------------------
	struct SwaySpring  // AdvancedSwaySprings::Update: implicit damped spring toward a moving goal, pitch/yaw
	{
		bool init = false;
		float x[3] = {}, v[3] = {}, g[3] = {};
	};
	void SwaySpringUpdate(SwaySpring &s, const float m[2], const float k[2], const float c[2], const float goal[3], float dt,
	                      bool enabled)
	{
		if (!s.init)
		{
			memcpy(s.x, goal, sizeof(s.x));
			memcpy(s.g, goal, sizeof(s.g));
			memset(s.v, 0, sizeof(s.v));
			s.init = true;
		}
		float gv[3];
		for (int i = 0; i < 3; i++)
			gv[i] = SwayWrap180(goal[i] - s.g[i]) / dt;
		for (int i = 0; i < 2; i++)
		{
			if (!enabled)
			{
				s.x[i] = goal[i];
				s.v[i] = gv[i];
				continue;
			}
			float kk = k[i] / m[i], cc = c[i] / m[i];
			s.v[i] = (s.v[i] + dt * cc * gv[i] - dt * kk * (s.x[i] - goal[i])) / (1 + dt * cc + dt * dt * kk);
			s.x[i] += dt * s.v[i];
		}
		s.x[2] = goal[2];
		s.v[2] = gv[2];
		memcpy(s.g, goal, sizeof(s.g));
	}

	struct SwayInput
	{
		float view[3];     // ps viewangles (pitch, yaw, roll)
		float ads;         // 0..1
		float sinceFire;   // seconds since the last shot
		int stance;        // 0 stand, 1 crouch, 2 prone
		float xySpeed, maxSpeed;
		double bobCycle;   // radians, continuous (2 pi per bob wrap)
	};

	struct SwayState
	{
		// advanced sway
		bool advInit = false;
		float smoothed[3], prevSm[3], prevView[3], offset[3], velAbs[2], deadzone[2], worldVel[3], fire = 0;
		SwaySpring torso, gun;
		// idle
		double idleT[2] = {};
		float idleFactor = 1;
		// stance, ADS bob
		float crouchRot[3] = {};
		float bobRatio = 0;
		float bobCrouchF = 1;  // fAdsGunBobCrouchFactor, blended (IW8 switches it with the stance: a pop while ADS-walking)
		// outputs of the last update (applied again by a second call in the same frame, e.g. the other hand)
		float ang[3] = {}, org[3] = {}, view[3] = {};
		float torsoOut[3] = {}, gunOut[3] = {}, idleAng[3] = {}, stanceAng[3] = {}, bobAng[3] = {};
		// substeps: the last frame's inputs
		SwayInput lastIn = {};
		bool haveLast = false;
		// game glue
		const uint8_t *ps = nullptr;
		const SwayWeapon *w = nullptr;  // the weapon of the last update (the view hook's camera idle)
		double last = -1, nextTrace = 0;
		int variant = -1, prevBob = -1, gen = 0;
		double bobPhase = 0;            // BO3's bob cycle in cycles, continuous across wraps and restarts
		double bobOffset = 0;           // added to the engine's cycle since the last restart from a stop
		bool bobHeld = false;           // stopped: the engine's cycle is 0 and the phase is held
		double bobSm = 0, bobRate = 0;  // its tracking filter (cycles, cycles / s)
		bool bobInit = false;
		int lastCgTime = 0;             // cg time (ms) of the last step
	} g_sway;
	int g_swayClockOk, g_swayClockBad;  // steps timed by cg time / by the wall clock because cg time disagreed

	const SwayGraph &SwayGraphFor(const SwayWeapon &w, int which)
	{
		return w.hasGraph[which] ? w.graph[which] : g_swayCfg.graph[which];  // g_swayCfg's n = 0 is linear
	}

	// cg_view_motion.cpp AdvancedSwayState::Update (sway_sim.py AdvancedSway.update).
	void SwayAdvUpdate(SwayState &s, const SwayWeapon &w, const SwayInput &in, float dt)
	{
		const SwayGlobals &g = g_swayCfg;
		if (!s.advInit)
		{
			memcpy(s.smoothed, in.view, sizeof(s.smoothed));
			memcpy(s.prevSm, in.view, sizeof(s.prevSm));
			memcpy(s.prevView, in.view, sizeof(s.prevView));
			memset(s.offset, 0, sizeof(s.offset));
			memset(s.velAbs, 0, sizeof(s.velAbs));
			memset(s.deadzone, 0, sizeof(s.deadzone));
			memset(s.worldVel, 0, sizeof(s.worldVel));
			s.torso = SwaySpring{};
			s.gun = SwaySpring{};
			s.fire = 0;
			s.advInit = true;
			return;
		}
		if (dt <= 0)
			return;
		// fire fraction: 1 for fireMs after a shot, blended in / out over the start / finish durations
		float target = in.sinceFire * 1000.0f < w.fireMs ? 1.0f : 0.0f;
		float k = (target > s.fire ? w.fireStartMs : w.fireFinishMs) * 0.001f / dt;
		s.fire = k > 1 ? s.fire - s.fire / k + target / k : target;
		const float f = s.fire;
		// AngularSmoothing: the smoothed view closes on the raw one at lerp(torsoSmooth, fireTorsoSmooth, fire) /s
		if (g.smoothing)
		{
			float rate = (1 - f) * w.torsoSmooth + f * w.fireTorsoSmooth;
			float a = 1.0f - expf(-rate * dt);
			for (int i = 0; i < 3; i++)
			{
				s.smoothed[i] = s.smoothed[i] + SwayWrap180(in.view[i] - s.smoothed[i]) * a;
				if (g.smoothClamp > 0)
				{
					float lag = SwayWrap180(in.view[i] - s.smoothed[i]);
					if (fabsf(lag) > g.smoothClamp)
						s.smoothed[i] = in.view[i] - (lag > 0 ? g.smoothClamp : -g.smoothClamp);
				}
			}
		}
		else
			memcpy(s.smoothed, in.view, sizeof(s.smoothed));
		float vel[3];
		for (int i = 0; i < 3; i++)
			vel[i] = SwayWrap180(s.smoothed[i] - s.prevSm[i]) / dt;
		memcpy(s.prevSm, s.smoothed, sizeof(s.prevSm));
		// torso goal (AdvancedSwayDeadzone): accumulate the -view delta, clamped to a deadzone that opens with view speed
		for (int i = 0; i < 2; i++)
			s.velAbs[i] = SwaySmoothFilter(s.velAbs[i], fabsf(vel[i]), w.torsoViewSmoothMs, dt);
		for (int i = 0; i < 3; i++)
			s.offset[i] = SwayWrap180(s.offset[i] - SwayWrap180(s.smoothed[i] - s.prevView[i]));
		for (int i = 0; i < 2; i++)
		{
			float goal = SwayGraphValue(SwayGraphFor(w, 0), w.dzSpeed[i] > 0 ? s.velAbs[i] / w.dzSpeed[i] : 1.0f) * w.dzMax[i] *
			             (f * w.fireDzScale + 1 - f);
			s.deadzone[i] = SwayLinearTrack(goal, s.deadzone[i], w.dzAdj[i], dt) * (1 - in.ads);
			s.offset[i] = SwayClamp(s.offset[i], -s.deadzone[i], s.deadzone[i]);
		}
		memcpy(s.prevView, s.smoothed, sizeof(s.prevView));
		float goal[3];
		for (int i = 0; i < 3; i++)
			goal[i] = s.offset[i] + SwayWrap180(s.smoothed[i] - in.view[i]);
		SwaySpringUpdate(s.torso, w.tMass, w.tSpring, w.tDamp, goal, dt, g.torsoSpring);
		// gun direction (AdvancedSwayGunDir): graph(|v| / viewspeed) x offset, signed by the smoothed view velocity
		if (!w.gun)
			return;
		for (int i = 0; i < 3; i++)
			s.worldVel[i] = SwaySmoothFilter(s.worldVel[i], vel[i], w.gunViewSmoothMs, dt);
		float gg[3] = {0, 0, 0};
		for (int i = 0; i < 2; i++)
		{
			float mag = SwayGraphValue(SwayGraphFor(w, 1), w.gSpeed[i] > 0 ? fabsf(s.worldVel[i]) / w.gSpeed[i] : 1.0f) * w.gOff[i];
			gg[i] = (s.worldVel[i] < 0 ? -mag : mag) * g.gunSign;
		}
		if (f > 0)
		{
			float sc = w.fireToGun - 1.0f;
			for (int i = 0; i < 3; i++)
				gg[i] = gg[i] + (sc * s.torso.x[i] - gg[i]) * f;
		}
		SwaySpringUpdate(s.gun, w.gMass, w.gSpring, w.gDamp, gg, dt, g.gunSpring);
	}

	// One update: every part's state advances by dt (0 on the first frame: nothing moves yet).
	void SwayStepOnce(SwayState &s, const SwayWeapon &w, const SwayInput &in, float dt)
	{
		const SwayGlobals &g = g_swayCfg;
		if (w.adv && g.parts[0])
			SwayAdvUpdate(s, w, in, dt);
		// idle time and stance factor (PM_Weapon_IncrementMovementIdleTime_AdvancedIdle, _IdleFactor)
		float tgt = in.stance == 1 ? w.crouchFactor : in.stance == 2 ? w.proneFactor : 1.0f;
		s.idleFactor = SwayLinearTrack(tgt, s.idleFactor, 0.5f, dt);
		for (int k = 0; k < 2; k++)
		{
			const SwayIdleSet &set = w.idle[k];
			if (set.on)
				s.idleT[k] += dt * 1000.0 * (set.hipSpeed + (set.adsSpeed - set.hipSpeed) * in.ads);
		}
		// crouch cant, smoothed at fDuckedOfsRotRate
		float cr = dt * w.duckRotRate < 1 ? dt * w.duckRotRate : 1.0f;
		for (int i = 0; i < 3; i++)
			s.crouchRot[i] += ((in.stance == 1 ? w.duckRot[i] : 0.0f) - s.crouchRot[i]) * cr;
		// ADS gun bob amplitude ratio (bg_gunBobTransIn/OutTime)
		float want = in.maxSpeed > 0 ? (in.xySpeed / in.maxSpeed < 1 ? in.xySpeed / in.maxSpeed : 1.0f) : 0.0f;
		float trans = want > s.bobRatio ? g.gunBobIn : g.gunBobOut;
		s.bobRatio = trans > 0 ? SwayLinearTrack(want, s.bobRatio, 1.0f / trans, dt) : want;
		// the crouch factor moves at the same rate (IW8 applies it at once; the bob is then up to ~0.15 deg off in a frame)
		const float crouchF = in.stance == 1 ? w.bobCrouch : 1.0f, crouchT = crouchF > s.bobCrouchF ? g.gunBobIn : g.gunBobOut;
		s.bobCrouchF = crouchT > 0 ? SwayLinearTrack(crouchF, s.bobCrouchF, 1.0f / crouchT, dt) : crouchF;
	}

	// A frame longer than 1 / sway_substep (default 240 Hz) is integrated in equal substeps, the inputs interpolated
	// from the last frame's (the view along the shorter arc, a shot placed at its time), so 30 and 60 fps follow the
	// same curve as 240 fps. IW8 itself steps once per frame (sway_substep=0), which lags and damps more at low fps.
	void SwayStep(SwayState &s, const SwayWeapon &w, const SwayInput &in, float dt)
	{
		const float h = g_swayCfg.substepHz > 0 ? 1.0f / g_swayCfg.substepHz : 0.0f;
		int n = h > 0 && dt > h && s.haveLast ? static_cast<int>(ceilf(dt / h - 1e-3f)) : 1;
		n = n < 1 ? 1 : n > 16 ? 16 : n;
		if (n == 1)
			SwayStepOnce(s, w, in, dt);
		else
		{
			const SwayInput &a = s.lastIn;
			for (int k = 1; k <= n; k++)
			{
				const float u = static_cast<float>(k) / n, before = dt * (1 - u);  // this substep ends `before` s ahead of the frame
				SwayInput x = in;
				for (int i = 0; i < 3; i++)
					x.view[i] = in.view[i] - SwayWrap180(in.view[i] - a.view[i]) * (1 - u);
				x.ads = a.ads + (in.ads - a.ads) * u;
				x.xySpeed = a.xySpeed + (in.xySpeed - a.xySpeed) * u;
				x.bobCycle = a.bobCycle + (in.bobCycle - a.bobCycle) * u;
				x.sinceFire = before <= in.sinceFire ? in.sinceFire - before : a.sinceFire + (dt - before);
				SwayStepOnce(s, w, x, dt / n);
			}
		}
		s.lastIn = in;
		s.haveLast = true;
	}

	// The idle's camera part (BG_CalculateViewMovement_Angles_Idle), visual only, x ADS, added to out[0..1]. `leadSec`
	// runs the idle clocks on from the last update (the view hook runs before this frame's gun update).
	void SwayCameraIdle(const SwayState &s, const SwayWeapon &w, float ads, double leadSec, float out[2])
	{
		const SwayGlobals &g = g_swayCfg;
		if (!g.parts[4])
			return;
		for (int k = 0; k < 2; k++)
		{
			const SwayIdleSet &set = w.idle[k];
			if (!set.on)
				continue;
			const double t = s.idleT[k] + leadSec * 1000.0 * (set.hipSpeed + (set.adsSpeed - set.hipSpeed) * ads);
			const float S = s.idleFactor * g.scale[2];
			float vx = set.hipView[0] + (set.adsView[0] - set.hipView[0]) * ads;
			float vy = set.hipView[1] + (set.adsView[1] - set.hipView[1]) * ads;
			out[0] += static_cast<float>(sin(t * g.idleViewTime[0])) * vx * ads * S * 0.01f;
			out[1] += static_cast<float>(sin(t * g.idleViewTime[1])) * vy * ads * S * 0.01f;
		}
	}

	// The additions for this frame: gun angles, gun origin (view space forward, left, up) and camera angles.
	void SwayCompose(SwayState &s, const SwayWeapon &w, const SwayInput &in)
	{
		const SwayGlobals &g = g_swayCfg;
		const float hip = SwayClamp(1 - in.ads, 0, 1), ads = SwayClamp(in.ads, 0, 1);
		float ang[3] = {}, org[3] = {}, view[3] = {}, dst[3] = {};
		for (float *v : {s.torsoOut, s.gunOut, s.idleAng, s.stanceAng, s.bobAng})
			v[0] = v[1] = v[2] = 0;
		// stance: stand offset and rotation (constant) and the crouch cant, x (1 - ADS)
		if (w.stance && g.parts[2])
		{
			float stand[3], crouch[3];
			for (int i = 0; i < 3; i++)
			{
				stand[i] = hip * w.standRot[i];
				crouch[i] = hip * s.crouchRot[i];
				org[i] += hip * w.standOfs[i];
				s.stanceAng[i] = stand[i] + crouch[i];
				ang[i] += s.stanceAng[i];
			}
			SwayRotateAbout(dst, w.standPivot, stand);
			SwayRotateAbout(dst, w.duckPivot, crouch);
		}
		// idle: per setting, sines at 0.001 / 0.0007 / 0.0005 per idle ms, forward push, rotation about (rot, 0, 0)
		if (g.parts[1])
			for (int k = 0; k < 2; k++)
			{
				const SwayIdleSet &set = w.idle[k];
				if (!set.on)
					continue;
				const double t = s.idleT[k];
				const float S = s.idleFactor * g.scale[2];
				float m[5];
				for (int i = 0; i < 5; i++)
					m[i] = set.hip[i] + (set.ads[i] - set.hip[i]) * ads;
				float ia[3] = {static_cast<float>(sin(t * 0.001)) * m[0] * S * 0.01f, static_cast<float>(sin(t * 0.0007)) * m[1] * S * 0.01f,
				               static_cast<float>(sin(t * 0.0005)) * m[2] * S * 0.01f};
				for (int i = 0; i < 3; i++)
				{
					ang[i] += ia[i];
					s.idleAng[i] += ia[i];
				}
				org[0] += static_cast<float>(sin(t * g.idleFwdTime)) * m[3] * S * g.idleFwdMag;
				static const float kEye[3] = {};
				const float P[3] = {m[4], 0, 0};
				float r[3] = {P[0], 0, 0};
				SwayRotateAbout(r, kEye, ia);  // R * P
				for (int i = 0; i < 3; i++)
					org[i] += P[i] - r[i];
			}
		if (g.parts[1])
			SwayCameraIdle(s, w, ads, 0.0, view);
		// ADS gun bob and movement tilt (ADS only)
		if (w.adsBob && g.parts[3] && ads > 0)
		{
			const float amp = s.bobRatio * g.gunBobMax * s.bobCrouchF * g.scale[3];
			auto bob = [amp](double c, float &v, float &h) {
				v = static_cast<float>((sin(c * 4 + 1.5707963267948966) * 0.15 + sin(c * 2) * 0.75) * amp);
				h = static_cast<float>(sin(c) * amp);
			};
			const double c0 = g.bobLag * 3.141592653589793 + 2 * 3.141592653589793 + in.bobCycle;
			float v, h;
			bob(c0, v, h);
			float ba[3] = {ads * v * w.bobPitch, ads * h * w.bobYaw, 0};
			bob(c0 - 0.47123894, v, h);
			float tilt[3] = {ads * v * -w.tilt[0], ads * h * w.tilt[1], ads * h * w.tilt[2]};
			for (int i = 0; i < 3; i++)
			{
				s.bobAng[i] = ba[i] + tilt[i];
				ang[i] += s.bobAng[i];
			}
			const float pivot[3] = {w.tiltOffset, 0, 0};
			SwayRotateAbout(dst, pivot, tilt);
		}
		// advanced sway: torso about the eye, gun about gunPivotPoint, x (1 - ADS)
		if (w.adv && g.parts[0] && s.torso.init)
		{
			s.torsoOut[0] = hip * s.torso.x[0] * g.scale[0];
			s.torsoOut[1] = hip * s.torso.x[1] * g.scale[0];
			if (w.gun && g.gunDir && s.gun.init)
			{
				s.gunOut[0] = hip * s.gun.x[0] * g.scale[1];
				s.gunOut[1] = hip * s.gun.x[1] * g.scale[1];
				s.gunOut[2] = w.yawToRoll * s.gunOut[1];
			}
			for (int i = 0; i < 3; i++)
				ang[i] += s.torsoOut[i] + s.gunOut[i];
			SwayRotateAbout(dst, w.pivot, s.gunOut);
		}
		for (int i = 0; i < 3; i++)
		{
			s.ang[i] = ang[i];
			s.org[i] = org[i] + dst[i];
			s.view[i] = view[i];
		}
	}

	// ---- game glue -----------------------------------------------------------------------------------------------------
	constexpr size_t kSwayPsWeapon = 0x2C0, kSwayPsAds = 0x2FC, kSwayPsViewAngles = 0x318, kSwayPsPmFlags = 0x10,
	                 kSwayPsVelocity = 0x3C, kSwayPsSpeed = 0xCC, kSwayPsMoveSpeedScale = 0x7D4, kSwayPsBobCycle = 0x0C;
	const void *g_swayFirePs;
	double g_swayFireTime = -1e9;
	double g_swayViewTime = -1;  // when g_sway.view was last computed
	double (*g_swayClock)();     // the offline harness's clock; the game uses GameNow() (holds while paused)
	double SwayNow() { return g_swayClock ? g_swayClock() : GameNow(); }

	// FireRecoilHook: a shot by this playerState.
	void SwayOnFire(const void *ps)
	{
		g_swayFirePs = ps;
		g_swayFireTime = SwayNow();
	}

	// Keeps each weapon's variant index (indices change between maps): one pass over the variants, names looked up in a
	// hash (bo3_perf.h, which keeps them current after that from the registration hook).
	void ResolveSwayVariants()
	{
		if (g_swayCount)
			ResolveKinds(1u << kSubSway);
	}

	SwayWeapon *SwayForVariant(int variant)
	{
		for (int i = 0; i < g_swayCount; i++)
			if (g_sways[i].variant == variant)
				return &g_sways[i];
		return nullptr;
	}

	// BO3's bob cycle moves in whole 1/256 steps and each pmove drops the fraction, so it advances unevenly from frame to
	// frame (e.g. 1, 2, 2, 1 at 144 fps). The ADS gun bob follows a type-2 tracking filter on it instead (critically
	// damped at sway_bobfilter Hz; no lag at a steady pace), so the bob moves at an even speed. Stepped in <= 2 ms.
	void SwayBobTrack(SwayState &s, float dt)
	{
		const double w = 2 * 3.141592653589793 * g_swayCfg.bobHz;
		if (!s.bobInit || fabs(s.bobPhase - s.bobSm) > 0.5)
		{
			s.bobSm = s.bobPhase;
			s.bobRate = 0;
			s.bobInit = true;
			return;
		}
		int n = static_cast<int>(ceilf(dt / 0.002f));
		n = n < 1 ? 1 : n > 64 ? 64 : n;
		const double h = static_cast<double>(dt) / n;
		for (int i = 0; i < n; i++)
		{
			const double e = s.bobPhase - s.bobSm;
			s.bobRate += w * w * e * h;
			s.bobSm += (s.bobRate + 2 * w * e) * h;
		}
	}

	// CalcWeaponPosHook: update once per frame, then add the angles and the origin to what BO3 is about to place the gun
	// with. A second call in the same frame (same cg time and view angles, or within 0.2 ms) re-applies the same result.
	// dt is the game's own frame time (cg time, the clock the usercmd and so the view angles move on) while it agrees with
	// the wall clock (sway_clock=qpc: always the wall clock, as before).
	constexpr size_t kSwayCgFrametime = 0x11A888, kSwayCgTime = 0x11A88C;  // cg_t ints (ms), NOTES_view.txt
	void SwayApply(const uint8_t *ps, const uint8_t *cg, float *placement, float *ang)
	{
		SwayState &s = g_sway;
		const SwayGlobals &g = g_swayCfg;
		if (!g_swayCount || !g.enable)
		{
			s.last = -1;
			return;
		}
		const int variant = static_cast<int>(*reinterpret_cast<const uint64_t *>(ps + kSwayPsWeapon) & 0x1FF);
		const SwayWeapon *w = SwayForVariant(variant);
		if (!w)
		{
			s.last = -1;
			s.variant = -1;
			return;
		}
		const double now = SwayNow();
		const int cgTime = cg ? *reinterpret_cast<const int32_t *>(cg + kSwayCgTime) : 0;
		SwayInput in;
		memcpy(in.view, ps + kSwayPsViewAngles, sizeof(in.view));
		if (!std::isfinite(in.view[0]) || !std::isfinite(in.view[1]) || !std::isfinite(in.view[2]))
			return;
		in.ads = SwayClamp(*reinterpret_cast<const float *>(ps + kSwayPsAds), 0, 1);
		in.sinceFire = g_swayFirePs == ps ? static_cast<float>(now - g_swayFireTime) : 1e9f;
		const uint64_t pmFlags = *reinterpret_cast<const uint64_t *>(ps + kSwayPsPmFlags);
		in.stance = (pmFlags & 1) ? 2 : (pmFlags & 2) ? 1 : 0;
		const float *vel = reinterpret_cast<const float *>(ps + kSwayPsVelocity);
		in.xySpeed = sqrtf(vel[0] * vel[0] + vel[1] * vel[1]);
		in.maxSpeed = *reinterpret_cast<const int32_t *>(ps + kSwayPsSpeed) * *reinterpret_cast<const float *>(ps + kSwayPsMoveSpeedScale);
		if (!(in.maxSpeed > 1.0f))
			in.maxSpeed = 190.0f;
		// bob cycle with generations (continuous across wraps; held while the engine zeroes it on a stop)
		const int bob = *reinterpret_cast<const int32_t *>(ps + kSwayPsBobCycle) & 0xFF;
		if (s.prevBob >= 0)
		{
			int d = bob - s.prevBob;
			if (d < -128)
				s.gen++;
			else if (d > 128)
				s.gen--;
			s.gen &= (1 << 20) - 1;
		}
		s.prevBob = bob;
		if (bob == 0 && in.xySpeed < 2.0f)
			s.bobHeld = true;  // the engine zeroes the cycle below 1 u/s: hold the phase
		else
		{
			const double raw = s.gen + bob / 256.0;
			if (s.bobHeld)
			{
				// moving again: the engine restarts the cycle from 0, which would jump the ADS bob by up to half a cycle
				// (a pop at every stop or quick reversal). Carry on from the held phase instead (only the fraction matters).
				const double off = s.bobPhase - raw;
				s.bobOffset = off - floor(off);
				s.bobHeld = false;
			}
			s.bobPhase = raw + s.bobOffset;
		}
		in.bobCycle = s.bobPhase * 2 * 3.141592653589793;

		// A gap (pause, menu, death), another client's ps or a weapon from none: start over.
		const float gap = s.last < 0 ? 1e9f : static_cast<float>(now - s.last);
		if (gap > 0.25f || s.ps != ps || s.variant < 0)
		{
			const int prevBob = s.prevBob, gen = s.gen;
			const double phase = s.bobPhase, bobSm = s.bobSm, bobRate = s.bobRate, bobOffset = s.bobOffset;
			const bool bobInit = s.bobInit, bobHeld = s.bobHeld;
			s = SwayState{};
			s.prevBob = prevBob, s.gen = gen, s.bobPhase = phase, s.bobOffset = bobOffset, s.bobHeld = bobHeld;
			s.bobSm = bobSm, s.bobRate = bobRate, s.bobInit = bobInit;
			s.ps = ps;
			SwayStep(s, *w, in, 0.0f);
			SwayBobTrack(s, 0.0f);
			s.last = now;
			s.lastCgTime = cgTime;
		}
		else
		{
			const bool sameFrame = gap < 0.0002f || (g.frameId && cg && gap < 0.0025f && cgTime == s.lastCgTime &&
			                                         memcmp(in.view, s.lastIn.view, sizeof(in.view)) == 0);
			if (!sameFrame)
			{
				float dt = gap;
				if (g.gameClock && cg)
				{
					// cg time moved by whole ms since the last step: that is the input interval the view delta covers. Taken
					// while it agrees with the wall clock (a frozen, jumping or misread cg time falls back to it).
					const float gdt = (cgTime - s.lastCgTime) * 0.001f;
					if (gdt > 0 && gdt < 1.5f * gap + 0.003f && gdt > 0.5f * gap - 0.003f)
					{
						dt = gdt;
						g_swayClockOk++;
					}
					else
						g_swayClockBad++;
				}
				dt = dt > 0.1f ? 0.1f : dt;
				SwayStep(s, *w, in, dt);
				SwayBobTrack(s, dt);
				s.last = now;
				s.lastCgTime = cgTime;
			}
		}
		if (g.bobHz > 0)
			in.bobCycle = s.bobSm * 2 * 3.141592653589793;
		s.w = w;
		if (variant != s.variant)
		{
			static int s_logs;
			if (s_logs++ < 100)
				Log("sway: holding %s (variant %d): adv %s, idle %d+%d, stance %s, ADS bob %s", w->weapon, variant, w->adv ? "on" : "off",
				    w->idle[0].on, w->idle[1].on, w->stance ? "on" : "off", w->adsBob ? "on" : "off");
			s.variant = variant;
		}
		SwayCompose(s, *w, in);
		g_swayViewTime = now;
		for (int i = 0; i < 3; i++)
		{
			ang[i] += s.ang[i];
			placement[4 + i] += s.org[i];
		}
		if (g.debug && now >= s.nextTrace)
		{
			s.nextTrace = now + 0.5;
			Log("sway: ads %.2f stance %d speed %.0f/%.0f fire %.2f | torso %.2f %.2f | gun %.2f %.2f %.2f | idle %.2f %.2f %.2f | stance %.2f "
			    "%.2f %.2f | adsbob %.2f %.2f %.2f | org %.2f %.2f %.2f | cam %.3f %.3f | clock %s (%d/%d steps on cg time)",
			    in.ads, in.stance, in.xySpeed, in.maxSpeed, s.fire, s.torsoOut[0], s.torsoOut[1], s.gunOut[0], s.gunOut[1], s.gunOut[2],
			    s.idleAng[0], s.idleAng[1], s.idleAng[2], s.stanceAng[0], s.stanceAng[1], s.stanceAng[2], s.bobAng[0], s.bobAng[1],
			    s.bobAng[2], s.org[0], s.org[1], s.org[2], s.view[0], s.view[1], g.gameClock ? "game" : "qpc", g_swayClockOk,
			    g_swayClockOk + g_swayClockBad);
		}
	}

	// ViewAxisHook: the idle's camera part (visual only; the aim is the usercmd's). This runs before the frame's gun update,
	// so the idle clocks are run on to now from the last one (camLead, always on: there is no cfg key for it).
	void SwayViewAngles(float *angles)
	{
		const SwayState &s = g_sway;
		if (!g_swayCount || !g_swayCfg.enable || s.variant < 0 || s.last < 0)
			return;
		const double now = SwayNow();
		if (now - g_swayViewTime > 0.25)
			return;
		if (g_swayCfg.camLead && s.w && s.haveLast)
		{
			float v[2] = {0, 0};
			const double lead = now - s.last;
			SwayCameraIdle(s, *s.w, SwayClamp(s.lastIn.ads, 0, 1), lead < 0 ? 0.0 : lead > 0.05 ? 0.05 : lead, v);
			angles[0] += v[0];
			angles[1] += v[1];
			return;
		}
		angles[0] += s.view[0];
		angles[1] += s.view[1];
	}
	// Live tuning: the whole sway config is read again when the cfg changes (bo3_additive.h ReloadAdditiveTuning).
	void BeginSwayReload()
	{
		g_namesDirty = true;  // the name index points into g_sways
		g_swayCount = 0;
		g_swayAliasCount = 0;
		g_swayCfg = SwayGlobals{};
	}
	void EndSwayReload()
	{
		FinishSwayConfig();
		ResolveSwayVariants();
		g_sway.variant = -1;  // re-log the held weapon's parts
		LogSwayConfig("live reload: ");
	}
}
