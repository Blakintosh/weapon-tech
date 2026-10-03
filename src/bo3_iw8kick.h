// IW8 (MW2019) view kick / gun kick integrator, standalone.
// Ported from the decompiled IW8 code (OpenIW8, %TEMP%\iw8rec\openiw8):
//   bg_weapons_util.cpp  BG_CalculateKickMovement, BG_KickAngles, BG_WeaponFireRecoil, BG_CalculateKickPolar,
//                        BG_GetAngularViewKickSettings / BG_GetAngularGunKickSettings (set selection)
//   bg_weapons_view.cpp  BG_CalculateWeaponMovement_Recoil (gun side)
//   cg_view.cpp          CgViewSystem::UpdateViewKickState (what happens to the kick after firing stops)
//   cg_view_mp.cpp       CG_ViewMP_UpdateKickAngles; client_mp\cl_input_mp.cpp CL_InputMP_FinishMove
// Tags: [C] read from code, [I] inferred / value not recoverable (see "Constants").
//
// ---- Units and signs ------------------------------------------------------------------------------
// angles[0] = pitch, angles[1] = yaw, degrees. vel = deg/s. Accelerations deg/s^2. IW convention:
// negative pitch = up, positive yaw = left. A fired kick is an angular VELOCITY, not an angle:
//   BG_CalculateKickPolar [C]: pitchVel = -S*cos(theta), yawVel = -S*sin(theta), S = strength (deg/s),
//   theta = the set's Dir +- Dev (degrees). Dir 0 = straight up, positive Dir = right, negative = left.
//   The per-shot spread of theta inside Dev is lost in the decompile [I]: Iw8KickPolar uses Dir + (r-0.5)*Dev.
//   PitchScale is fetched with the set but its use is also lost in the decompile [I]; applied to pitch here.
//
// ---- Per shot (BG_WeaponFireRecoil, called from CG_FireWeapon for the local player) [C] -------------
//   The kick velocity REPLACES the current one (cg->kickAVel = out; gun recoilSpeed = out). Nothing is added
//   to the angles. needsToCrossCenter = (angles non-zero) && dot(angles, newVel) < 0, i.e. the new kick heads
//   back toward/through the centre; the integrator then lets the angles pass the centre once instead of
//   snapping to zero there. View kick also gets roll vel = -0.5*yawVel, but with useNewViewKick=1 (every IW8
//   gun checked) the 2D integrator never touches roll, so the roll kick is always 0 [C].
//   Velocity = polar(S) * kickPercent * inputScalar. kickPercent = start% while the starting-kick timer runs
//   (starting bullets * fireTime), end% once the ending timer has run out, 1.0 otherwise (a step, not a lerp).
//   inputScalar = kickAligned/OpposedInputScalar by the sign of the stick/mouse move vs the kick (1.0 on
//   stock weapons). ADS vs hip sets: fWeaponPosFrac >= bg_viewAndGunKickAdsFrac (default not recoverable [I]).
//   Set choice (Iw8KickSelectSet) [C]: start at set 0 (UseSet[0] is ignored), then k = 1.. while
//   UseSet[k] && Bullet[k] <= shotNum. It STOPS at the first unused set: galima gun sets (UseSet 0,0,1,1,..)
//   and sierrax view sets (UseSet 0,0,1,1,0,1) never leave set 0. Hip always uses set 0.
//   shotNum = weaponShotCount - adsRecoilShotCountOffset; the count is bumped in PM_Weapon_FireWeapon before
//   the fire event reaches cgame, so the first shot of a burst is shotNum 1 [I, strong]; the count caps at 31.
//
// ---- Per frame -------------------------------------------------------------------------------------
// View (BG_KickAngles) [C]: fixed KICK_ANGLES_TIME_STEP substeps, remainder carried to the next frame,
//   at most KICK_ANGLES_MAX_TIME_STEPS per frame (then the remainder is dropped). Params: hip set if
//   fWeaponPosFrac <= 0.5 else ADS: accel = f{Hip,Ads}ViewKickCenterSpeed, {hip,ads}ViewKickReturnAccelScale,
//   {hip,ads}ViewKickReturnSpeedCurveScale; max = fViewMaxPitch/fViewMaxYaw; kick scales 1,1. Output
//   kickAngles = raw * viewKickPitch/YawScale (attachment scalars, 1.0 on the base weapon).
// Gun (BG_CalculateWeaponMovement_Recoil, only if the weapon can ADS) [C]: substeps of 0.005 s with a
//   shorter last step (no carry). Every param lerped hip->ads by fWeaponPosFrac: f{Hip,Ads}GunKickAccel,
//   {hip,ads}GunKickReturnAccelScale, {hip,ads}GunKickReturnSpeedCurveScale; max = fGunMaxPitch/fGunMaxYaw.
//   Output recoilAngles = raw * gunKickPitch/YawScale. useNewGunKick=0 falls back to the IW3 per-axis spring.
//
// ---- What the MP view does with kickAngles [C] -------------------------------------------------------
//   The kick is NOT folded into the aim while it is moving: CL_InputMP_FinishMove sends
//   usercmd.angles = clViewangles + kickAngles, and the refdef uses the same sum. So the whole kick moves the
//   aim/bullets, and the spring pulls it back to 0 unless something transfers it into clViewangles:
//   * CgViewSystem::UpdateViewKickState (Iw8ViewKickReturn below). While weaponState == FIRING it only
//     accumulates counterMag = total |camera movement| the player made while firing (reset to
//     min(counterMag, |kick|) when the input stops; 0 if disableInputDrivenViewReturnDampening). On the first
//     frame after firing where |kick| shrinks it computes
//       correction = kick * viewKickMaintainFraction + counterMag * kick/|kick|, clamped per axis to |kick|
//     and then, as the spring returns, adds correction * (|dKick| / |kick at release|) to clViewangles each
//     frame (per axis at most |dKick|) until the ratio sums to 1. That is the permanent part.
//     viewKickMaintainFraction is 0 on galima and sierrax: with no player input the aim ends exactly where
//     it started. With input, whatever the player pulled against the kick (up to the kick size) is kept,
//     so the view does not sink below the target when the kick recentres.
//   * No kick return (fAdsViewKickCenterSpeed == 0 && fHipViewKickCenterSpeed == 0): kickAngles are added to
//     clViewangles every frame and zeroed, and kickAVel *= no_kick_velocity_dampen (0.35) each frame.
//
// ---- Constants ---------------------------------------------------------------------------------------
// Read from the MW2019 Xbox Test build's PDB (PDB symbol report):
// Iw8KickConsts / kIw8KickReal below. kIw8KickLegacy keeps the pre-PDB guesses the guns were first tuned with
// (wop_kick_consts=legacy). The macros left are the values the PDB confirmed.
#pragma once
#include <cmath>

#ifndef IW8KICK_TIME_STEP_MS
#define IW8KICK_TIME_STEP_MS 5  // [C] KICK_ANGLES_TIME_STEP; IW3 CG_KickAngles and the IW8 gun path both use 5 ms
#endif
#ifndef IW8KICK_STOP_OFFSET
#define IW8KICK_STOP_OFFSET 0.01f  // [C] STOP_THRESHOLD_OFFSET (deg): snap to 0 if closer than this...
#endif
#ifndef IW8KICK_NEAR_SIDE_MULT
#define IW8KICK_NEAR_SIDE_MULT 1.0f  // [C] the other blend operand (xmm6) inside STOP_SIDE_MOVEMENT_OFFSET
#endif
#ifndef IW8KICK_CATCH_UP_MS
#define IW8KICK_CATCH_UP_MS 100.0f  // [C] CATCH_UP_TIME (view return, once the kick is already 0)
#endif

namespace
{
	// The integrator constants from the MW2019 PDB build (IW8_xbone\REPORT_iw8_symbols.md, section 1), and the
	// pre-PDB guesses every gun was first tuned against. wop_kick_consts=iw8|legacy picks one, live.
	struct Iw8KickConsts
	{
		int maxSteps;
		float fudge, stopSpeed, stopSideOffset, sideMult, farDist, edgeMult, centerMult;
		bool clampReturn, scalesReturn;
		int counterResetMs;
		float adsKickFrac;  // bg_viewAndGunKickAdsFrac: ADS kick sets from this ADS fraction on
	};
	constexpr Iw8KickConsts kIw8KickReal = {20, 1.066f, 0.25f, 0.1f, 0.225f, 10.0f, 27.0f, 0.18f, true, false, 1500, 0.0001f};
	constexpr Iw8KickConsts kIw8KickLegacy = {40, 1.0f, 1.0f, 0.01f, 0.1f, 1.0f, 4.0f, 1.0f, false, true, 500, 0.5f};
	inline const Iw8KickConsts *g_iw8kc = &kIw8KickReal;

	struct Iw8KickParams
	{
		float maxPitch, maxYaw;         // fViewMaxPitch/Yaw or fGunMaxPitch/Yaw (deg)
		float accel;                    // recenterAngAcceleration (deg/s^2): f*ViewKickCenterSpeed / f*GunKickAccel
		float returnAccelScale;         // *KickReturnAccelScale
		float returnSpeedCurveScale;    // *KickReturnSpeedCurveScale
		float kickPitchScale = 1.0f;    // IW8 always passes 1,1 here (the scalars are applied to the output)
		float kickYawScale = 1.0f;
	};

	struct Iw8KickState
	{
		float angles[2] = {0, 0};  // raw kick angles (pitch, yaw), deg
		float vel[2] = {0, 0};     // deg/s
		bool needsToCrossCenter = false;
		float timeRemainingMs = 0;  // BG_KickAngles substep carry, [0, step); float so sub-ms frames don't round
	};

	inline float Iw8Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

	// BG_WeaponFireRecoil's effect on one kick state [C].
	inline void Iw8KickFire(Iw8KickState &s, float pitchVel, float yawVel)
	{
		const float p = s.angles[0], y = s.angles[1];
		s.needsToCrossCenter = (p * p > 1.0000001e-6f || y * y > 1.0000001e-6f) && (y * yawVel + p * pitchVel) < 0.0f;
		s.vel[0] = pitchVel;
		s.vel[1] = yawVel;
	}

	// BG_CalculateKickPolar for one set [C, spread inside Dev and pitchScale use I]. r in [0,1).
	inline void Iw8KickPolar(float dirDeg, float devDeg, float strength, float pitchScale, float r, float &pitchVel,
	                         float &yawVel)
	{
		const float theta = (dirDeg + (r - 0.5f) * devDeg) * (3.14159265f / 180.0f);
		pitchVel = -strength * cosf(theta);  // [C] IW8 never reads PitchScale for players
		(void)pitchScale;
		yawVel = -strength * sinf(theta);
	}

	// BG_GetAngular{View,Gun}KickSettings set index for ADS [C].
	inline int Iw8KickSelectSet(const int bullet[6], const bool useSet[6], int shotNum)
	{
		int k = 1;
		while (useSet[k] && bullet[k] <= shotNum)
			if (++k >= 6)
				return 5;
		return k - 1;
	}

	// BG_CalculateKickMovement: one substep of the 2D recentring spring [C]. Returns true when it snapped to 0.
	inline bool Iw8KickStep(Iw8KickState &s, const Iw8KickParams &p, float dt)
	{
		float *ang = s.angles, *vel = s.vel;
		const float dist = sqrtf(ang[0] * ang[0] + ang[1] * ang[1]);
		const float speed = sqrtf(vel[0] * vel[0] + vel[1] * vel[1]);
		if (dist < IW8KICK_STOP_OFFSET && speed < g_iw8kc->stopSpeed)
		{
			ang[0] = ang[1] = vel[0] = vel[1] = 0.0f;
			return true;
		}
		// Unit vector toward the centre (c) and along the velocity (u); defaults when degenerate.
		float cP = 1.0f, cY = 0.0f;
		if (dist > 1e-6f)
		{
			cP = -ang[0] / dist;
			cY = -ang[1] / dist;
		}
		float uP = 1.0f, uY = 0.0f;
		if (speed > 1e-6f)
		{
			uP = vel[0] / speed;
			uY = vel[1] / speed;
		}
		float acc = dt * p.accel * g_iw8kc->fudge;  // this step's acceleration budget (deg/s)

		// Damp the sideways part of the velocity (perpendicular to the centre direction).
		const float rP = cY, rY = -cP;  // "rightAngDirection"
		const float sideMult =
		    dist <= g_iw8kc->stopSideOffset ? IW8KICK_NEAR_SIDE_MULT
		                                     : (1000.0f / IW8KICK_TIME_STEP_MS) * dt * g_iw8kc->sideMult;
		const float side = Iw8Clamp((uP * rP + uY * rY) * speed * sideMult, -acc, acc);
		float sideP = -side * rP, sideY = -side * rY;

		// Recentre. Already heading in: steer the inward speed to a distance-shaped target speed.
		// Heading out (just kicked): full acceleration toward the centre.
		const bool headingIn = (uP * cP + uY * cY) > 0.0f;
		if (dist > 0.0f && speed > 0.0f && headingIn)
		{
			const float farDist = g_iw8kc->farDist * p.returnSpeedCurveScale;
			const float t = farDist > 0.0f ? Iw8Clamp((farDist - dist) / farDist, 0.0f, 1.0f) : 0.0f;
			const float t2 = t * t;
			const float target = ((1.0f - t2) * g_iw8kc->edgeMult + t2 * g_iw8kc->centerMult) * acc;
			const float want = target - (cP * vel[0] + cY * vel[1]);
			acc = g_iw8kc->clampReturn ? Iw8Clamp(want, -acc, acc) : want;  // [C] PDB build: the clamped value is used
		}
		float recP = cP * acc, recY = cY * acc;
		if (!headingIn && g_iw8kc->scalesReturn)
		{
			recP *= p.kickPitchScale;
			recY *= p.kickYawScale;
			sideP *= p.kickPitchScale;
			sideY *= p.kickYawScale;
			acc = sqrtf(recP * recP + recY * recY);
		}
		// Returning (the new velocity points inward and we are speeding up toward the centre): scale by
		// returnAccelScale. Braking (acc < 0) and the outward phase keep the full value.
		const float nP = recP + vel[0], nY = recY + vel[1];
		const float nLen = sqrtf(nP * nP + nY * nY);
		float nuP = 1.0f, nuY = 0.0f;
		if (nLen > 1e-6f)
		{
			nuP = nP / nLen;
			nuY = nY / nLen;
		}
		if (dist > 0.0f && nLen > 0.0f && (cY * nuY + nuP * cP) > 0.0f && (cP * nP + cY * nY) * acc > 0.0f)
		{
			recP = cP * (acc * p.returnAccelScale);
			recY = cY * (acc * p.returnAccelScale);
		}
		vel[0] = (sideP + vel[0]) + recP;
		vel[1] = (sideY + vel[1]) + recY;
		ang[0] += vel[0] * dt;
		ang[1] += vel[1] * dt;

		const float maxA[2] = {p.maxPitch, p.maxYaw};
		for (int i = 0; i < 2; i++)
		{
			if (ang[i] > maxA[i])
			{
				ang[i] = maxA[i];
				if (vel[i] > 0.0f)
					vel[i] = 0.0f;
			}
			else if (ang[i] < -maxA[i])
			{
				ang[i] = -maxA[i];
				if (vel[i] < 0.0f)
					vel[i] = 0.0f;
			}
		}

		// Crossed the centre this step? Snap to 0, unless this kick was flagged to pass through once.
		const float nd = sqrtf(ang[0] * ang[0] + ang[1] * ang[1]);
		if (dist <= 1e-6f || nd <= 1e-6f || ((-ang[1] / nd) * cY + (-ang[0] / nd) * cP) >= 0.0f)
			return false;
		if (s.needsToCrossCenter)
		{
			s.needsToCrossCenter = false;
			return false;
		}
		ang[0] = ang[1] = vel[0] = vel[1] = 0.0f;
		return true;
	}

	// BG_KickAngles substep loop (view kick) [C]. frameMs = cg->frametime.
	// frameMs is fractional: IW8's cg->frametime is whole ms at its own tick, but rounding BO3's variable frames
	// to whole ms skews the integrated climb by up to ~20% at high fps.
	inline void Iw8KickAdvance(Iw8KickState &s, const Iw8KickParams &p, float frameMs)
	{
		const float total = frameMs + s.timeRemainingMs;
		int steps = static_cast<int>(total / IW8KICK_TIME_STEP_MS);
		if (steps > g_iw8kc->maxSteps)
		{
			steps = g_iw8kc->maxSteps;
			s.timeRemainingMs = 0;
		}
		else
			s.timeRemainingMs = total - steps * static_cast<float>(IW8KICK_TIME_STEP_MS);
		const float dt = IW8KICK_TIME_STEP_MS * 0.001f;
		for (; steps > 0; steps--)
			Iw8KickStep(s, p, dt);  // IW8 ignores the return value here
	}

	// BG_CalculateWeaponMovement_Recoil substep loop (gun kick, useNewGunKick) [C]. Stops once it snaps to 0.
	inline void Iw8KickAdvanceGun(Iw8KickState &s, const Iw8KickParams &p, float frameSec)
	{
		float left = frameSec;
		while (left > 0.0f)
		{
			float dt;
			if (left <= 0.005f)
			{
				dt = left;
				left = 0.0f;
			}
			else
			{
				dt = 0.005f;
				left -= 0.005f;
			}
			if (Iw8KickStep(s, p, dt))
				break;
		}
	}

	// CgViewSystem::UpdateViewKickState: moves the maintained part of the view kick into the player's
	// persistent view angles once firing stops [C; constants I]. Call once per frame AFTER Iw8KickAdvance.
	//   kick       = current output kickAngles (pitch, yaw)
	//   lookDelta  = change of the player's own view angles since last frame (pitch, yaw)
	//   hasInput   = stick past its deadzone / mouse moved this frame
	//   out        = angles to add to the player's view angles now (0 while firing)
	struct Iw8ViewKickReturn
	{
		int state = 0;  // 0 idle, 1 firing, 2 transferring
		bool hadInput = false;
		float prevKick[2] = {0, 0};
		float counterMag = 0.0f;
		float correction[2] = {0, 0};
		float correctionMag = 0.0f, kickMagFiringEnd = 0.0f, ratio = 0.0f;
		int idleStartMs = 0;
	};

	inline float Iw8AngleNormalize180(float a) { return (a * (1.0f / 360.0f) - floorf(a * (1.0f / 360.0f) + 0.5f)) * 360.0f; }

	inline void Iw8ViewKickReturnUpdate(Iw8ViewKickReturn &r, const float kick[2], bool firing, bool hasInput,
	                                    const float lookDelta[2], float maintainFraction, bool dampeningDisabled,
	                                    int nowMs, int frameMs, float out[2])
	{
		out[0] = out[1] = 0.0f;
		const float k[2] = {Iw8AngleNormalize180(kick[0]), Iw8AngleNormalize180(kick[1])};
		const float kickSq = k[0] * k[0] + k[1] * k[1];
		if (firing)
		{
			r.state = 1;
			if (dampeningDisabled)
				r.counterMag = 0.0f;
			else if (r.hadInput && !hasInput)
				r.counterMag = fminf(sqrtf(kickSq), r.counterMag);
			else if (hasInput)
				r.counterMag += sqrtf(lookDelta[0] * lookDelta[0] + lookDelta[1] * lookDelta[1]);
		}
		else if (r.prevKick[0] * r.prevKick[0] + r.prevKick[1] * r.prevKick[1] > kickSq && r.state == 1)
		{
			const float mag = sqrtf(kickSq);
			float cp = 0.0f, cy = 0.0f;
			if (mag > 0.001f)
			{
				cp = Iw8AngleNormalize180(k[0] * maintainFraction + r.counterMag * (k[0] / mag));
				cy = Iw8AngleNormalize180(k[1] * maintainFraction + r.counterMag * (k[1] / mag));
				if (fabsf(cp) > fabsf(k[0]))
					cp = cp >= 0.0f ? fabsf(k[0]) : -fabsf(k[0]);
				if (fabsf(cy) > fabsf(k[1]))
					cy = cy >= 0.0f ? fabsf(k[1]) : -fabsf(k[1]);
			}
			if (fabsf(cp) <= 1e-6f && fabsf(cy) <= 1e-6f)
			{
				r.state = 0;
				r.idleStartMs = nowMs;
			}
			else
			{
				r.correction[0] = cp;
				r.correction[1] = cy;
				r.correctionMag = sqrtf(cp * cp + cy * cy);
				r.kickMagFiringEnd = mag;
				r.state = 2;
				r.ratio = 0.0f;
			}
		}

		if (r.state == 2)
		{
			const float d0 = k[0] - r.prevKick[0], d1 = k[1] - r.prevKick[1];
			float step = sqrtf(d0 * d0 + d1 * d1) / r.kickMagFiringEnd;
			if (!(kickSq > 1e-6f))
				step = fminf(frameMs / IW8KICK_CATCH_UP_MS, 1.0f);  // [C]
			float o0 = step * r.correction[0], o1 = step * r.correction[1];
			bool clipped = false;
			if (fabsf(d0) > 1e-6f && fabsf(o0) > fabsf(d0))
			{
				o0 = o0 >= 0.0f ? fabsf(d0) : -fabsf(d0);
				clipped = true;
			}
			if (fabsf(d1) > 1e-6f && fabsf(o1) > fabsf(d1))
			{
				o1 = o1 >= 0.0f ? fabsf(d1) : -fabsf(d1);
				clipped = true;
			}
			const float moved = sqrtf(o0 * o0 + o1 * o1);
			if (clipped)
				step = moved / r.correctionMag;
			r.counterMag = fmaxf(r.counterMag - moved, 0.0f);
			r.ratio += step;
			out[0] = o0;
			out[1] = o1;
			if (r.ratio >= 1.0f)
			{
				r.state = 0;
				r.idleStartMs = nowMs;
			}
		}
		else if (r.state == 0 && nowMs - r.idleStartMs >= g_iw8kc->counterResetMs)
			r.counterMag = 0.0f;

		r.prevKick[0] = k[0];
		r.prevKick[1] = k[1];
		r.hadInput = hasInput;
	}
}
