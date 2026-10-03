# Converts an IW8 (MW2019) weapon def dump's weapon offset patterns into bo3_additive.cfg lines.
# Usage: python iw8_wop_cfg.py <iw8 weapon .json> <bo3 weapon name> [fire time ms] [--sway] [--gdt] [--wop]
#                              [--att <attachment .json>]... [--idle <iw8_advidle.json>]
#   (no flag)  the wop_* lines, as before
#   --sway     the sway_* lines (bo3_sway.h): advanced hip sway, advanced idle, stance pivots, ADS gun bob
#   --gdt      the phase-1 BO3 GDT key/values (ADS sway, move/strafe/stance offsets and rates, adsViewBobMult,
#              swayMaxAngle 0 when sway_adv is emitted, hip/adsIdleAmount 0 when sway_idle is emitted)
#   --wop      the wop_* lines too, when combined with --sway / --gdt
#   --att      merges an attachment's swaySettings where it has overrideHip / overrideAds (-1 = inherit); only for
#              attachments the BO3 model actually has (a baked-in optic or stock)
#   --idle     AdvancedIdleSettings re-read from the game data (an iw8_advidle.json: {<weapon stem>: record});
#              default: $IW8_ADVIDLE, else iw8_advidle.json next to this script; with no such file the 14 values
#              of the weapon dump are used (they cover only 14 of the 39 settings, see idle_settings).
# The fire time defaults to the IW8 iFireTime; pass the BO3 weapon's own when they differ (the DLL detects
# sustained fire and counts full-auto ramps in its shots).
# <iw8 weapon .json> is a MW2019 weapon def dump (iw8_*.json, a top-level object with a 'weapDef' key).
# Formulas: bo3_wop.h (from OpenIW8 bg_weapon_offsets.cpp), bo3_sway.h (from OpenIW8 cg_view_motion.cpp /
# bg_weapons_view.cpp). Output goes to stdout: paste it into weapon_tech.cfg.
import json, os, sys

CURVES = ['weaponOffsetCurveHoldFireSlow', 'weaponOffsetCurveHoldFireFast', 'weaponOffsetCurveKick',
          'weaponOffsetCurveSnapDecay', 'weaponOffsetCurveAds', 'weaponOffsetCurveAlwaysOn']
CURVE_IDS = ['WOBC_HOLD_FIRE_BLEND_SLOW', 'WOBC_HOLD_FIRE_BLEND_FAST', 'WOBC_KICK_BLEND', 'WOBC_SNAP_DECAY_BLEND',
             'WOBC_ADS_BLEND', 'WOBC_ALWAYS_ON']
INTERP = ['WOBIT_LINEAR', 'WOBIT_CUBIC_EASE_IN', 'WOBIT_CUBIC_EASE_OUT', 'WOBIT_QUARTIC_EASE_IN', 'WOBIT_QUARTIC_EASE_OUT',
          'WOBIT_EXPONENTIAL_EASE_IN', 'WOBIT_EXPONENTIAL_EASE_OUT']
PATTERNS = ['WOP_KEYFRAME', 'WOP_NOISY_SINE', 'WOP_RANDOM_SQUARE', 'WOP_SINE']
TARGETS = ['WOTT_VIEW_ORIGIN', 'WOTT_VIEW_ANGLES', 'WOTT_WEAPON_ORIGIN', 'WOTT_WEAPON_ANGLES']
IDLE_JSON = os.environ.get('IW8_ADVIDLE') or os.path.join(os.path.dirname(os.path.abspath(__file__)), 'iw8_advidle.json')

g = lambda v: ('%.6g' % v)


def wop_lines(top, name, src, fire_ms=None):
    w = top['weapDef']
    out = []
    p = out.append
    p('# %s from %s' % (name, src.replace('\\', '/').split('/')[-1]))
    p('wop_weapon=%s,%d' % (name, int(fire_ms) if fire_ms else w['iFireTime']))
    # View-kick return (CgViewSystem::UpdateViewKickState; used with the global wop_kickreturn=1). Only written when it
    # differs from the default (maintain 0, input dampening on), and here so that wop_alias copies of this block get it.
    maintain = w.get('viewKickMaintainFraction', top.get('viewKickMaintainFraction', 0))
    no_damp = w.get('disableInputDrivenViewReturnDampening', top.get('disableInputDrivenViewReturnDampening', 0))
    if maintain or no_damp:
        p('wop_kickreturn=%s,1,%s,%d' % (name, g(maintain), 1 if no_damp else 0))
    for i, key in enumerate(CURVES):
        c = w[key]
        p('wop_curve=%s,%d,%s,%s,%s,%s,%s,%s,%d,%d' % (name, i, g(c['blendTime']), g(c['decayTime']), g(c['shotDecayFireTimeFrac']),
          g(c['holdTime']), g(c['adsFractionBegin']), g(c['adsFractionEnd']), INTERP.index(c['interpType']), INTERP.index(c['interpTypeOut'])))
    # Per-shot kick: angular sets (bullet k on uses set k: angle dir +- dev/2 from straight up, strength min..max),
    # scaled from the starting to the ending kick percent over the ADS kick bullets.
    p('wop_kickpct=%s,%d,%d,%s,%s,%s,%s,%s,%s,%s,%s' % (name, w['adsStartingKickBullets'], w['adsEndingKickBullets'],
      g(w['hipStartingGunKickPercent']), g(w['hipEndingGunKickPercent']), g(w['hipStartingViewKickPercent']),
      g(w['hipEndingViewKickPercent']), g(w['adsStartingGunKickPercent']), g(w['adsEndingGunKickPercent']),
      g(w['adsStartingViewKickPercent']), g(w['adsEndingViewKickPercent'])))
    for ads in ('hip', 'ads'):
        for kind in ('View', 'Gun'):
            if not w.get('useAngular%sKick' % kind, 0):
                continue
            for k in range(6):
                pre = '%sAngular%sKick' % (ads, kind)
                smax = w.get(pre + 'StrengthMax%d' % k, 0)
                bullet = w.get(pre + 'Bullet%d' % k, 0 if k == 0 else None)
                # BG_GetAngular*KickSettings: set 0, then advance through sets 1.. while each is enabled (UseSet;
                # the dump spells it UseSe) and its bullet is reached; the first disabled set ends the walk. Hip
                # fire only ever uses set 0.
                if k > 0:
                    use = w.get(pre + 'UseSet%d' % k, w.get(pre + 'UseSe%d' % k, 0))
                    if ads == 'hip' or not use:
                        break
                # A reached set with zero strength is still IW8's kick (none), so it is kept: without it the DLL
                # would leave BO3's own GDT kick in place (romeo870 gun kick, xmike109 hip gun kick).
                if bullet is None:
                    continue
                p('wop_kick=%s,%d,%d,%d,%s,%s,%s,%s,%s' % (name, ads == 'ads', kind == 'Gun', bullet, g(w[pre + 'Dir%d' % k]),
                  g(w[pre + 'Dev%d' % k]), g(w[pre + 'StrengthMin%d' % k]), g(smax), g(w.get(pre + 'PitchScale%d' % k, 1.0))))
    # Kick springs (BG_CalculateKickMovement): view = centre speed, gun = kick accel; return accel scale, return
    # speed curve scale, max pitch/yaw.  wop_spring=<weapon>,<0 view|1 gun>,<0 hip|1 ads>,accel,retAccel,retCurve,maxP,maxY
    def f(key):
        return w.get(key, top.get(key, 0))
    for gun, kind in ((0, 'View'), (1, 'Gun')):
        for ads, mode in ((0, 'hip'), (1, 'ads')):
            accel = f('f%s%sKickCenterSpeed' % (mode.capitalize(), kind)) if not gun else f('f%s%sKickAccel' % (mode.capitalize(), kind))
            p('wop_spring=%s,%d,%d,%s,%s,%s,%s,%s' % (name, gun, ads, g(accel), g(f('%s%sKickReturnAccelScale' % (mode, kind))),
              g(f('%s%sKickReturnSpeedCurveScale' % (mode, kind))), g(f('f%sMaxPitch' % kind)), g(f('f%sMaxYaw' % kind))))
    # Gun tilt (BG_ComputeAndApplyWeaponMovement_TiltAngles): extra gun angles from the gun kick, pitch*P, yaw*Y,
    # roll = yaw*R, rotated about a point <offset> forward (CG_CalculateWeaponMovement_CalcAngles).
    #   wop_tilt=<weapon>,hipP,hipY,hipR,hipOffset,adsP,adsY,adsR,adsOffset
    p('wop_tilt=%s,%s' % (name, ','.join(g(f('%sGunTilt%s' % (m, k))) for m in ('hip', 'ads') for k in ('PitchFactor', 'YawFactor', 'RollFactor', 'Offset'))))
    for i in range(w['numWeaponOffsetPatterns']):
        pt = w['weaponOffsetPatterns'][str(i)]
        if not pt['active']:
            continue
        m = pt['magnitude']
        p('wop=%s,%d,%d,%d,%s,%s,%s,%s,%s,%s,%s,%s,%d,%s,%d  # %s' % (name, CURVE_IDS.index(pt['curveType']),
          PATTERNS.index(pt['patternType']), TARGETS.index(pt['transformType']), g(pt['frequency']), g(pt['blendTime']),
          g(m['X']), g(m['Y']), g(m['Z']), g(pt['hipScale']), g(pt['rotationOffset']), g(pt['fullAutoScale']),
          pt['fullAutoBullets'], g(pt['fullAutoDecay']), pt['kickOrSnapDecayIndex'], pt['patternKey']))
    return out


# ---------------------------------------------------------------------------------------------------- sway
def _inherit(base, over):
    """An attachment override block: -1 (or a vec with -1 parts) keeps the weapon's value [I, REPORT_sway.md s2]."""
    if isinstance(base, dict) and isinstance(over, dict):
        return {k: _inherit(base.get(k), over[k]) if k in base else over[k] for k in over} | {k: v for k, v in base.items() if k not in over}
    if isinstance(over, (int, float)) and over == -1:
        return base
    return over


def merged_sway(top, att_tops=()):
    """weapDef.swaySettings with the attachments' overrideHip (hip + adv) / overrideAds (ads) blocks merged, in order."""
    sw = json.loads(json.dumps(top['weapDef']['swaySettings']))
    used = []
    for att in att_tops:
        s = att.get('swaySettings') or {}
        name = att.get('szInternalName') or att.get('internalName') or '?'
        if s.get('overrideHip'):
            sw['hip'] = _inherit(sw['hip'], s['hip'])
            sw['adv'] = _inherit(sw['adv'], s['adv'])
            used.append('%s (hip + advanced)' % name)
        if s.get('overrideAds'):
            sw['ads'] = _inherit(sw['ads'], s['ads'])
            used.append('%s (ADS)' % name)
    return sw, used


def idle_record(stem, idle_path=None):
    path = idle_path or IDLE_JSON
    if not os.path.exists(path):
        return None
    return json.load(open(path, encoding='utf-8')).get(stem)


FIELDS9 = ['BulletDirScale', 'IdleSpeed', 'WeaponMagnitudeX', 'WeaponMagnitudeY', 'WeaponMagnitudeZ', 'WeaponMagnitudeF',
           'WeaponRotationOffset', 'ViewMagnitudeX', 'ViewMagnitudeY']


def idle_settings(top, stem, idle_path=None):
    """(use, random, gasp, {1: (hip, ads), 2: ...}, note). hip/ads = dicts of FIELDS9.
    From iw8_advidle.json when there is a record; else the mwweapons dump's 14 dwords re-read against the
    game's layout (2 bools, gasp, 9 floats hip, 9 ads, setting 2 the same), which stops at setting 1's ADS pitch: the rest
    falls back to the hip value scaled by |ADS pitch / hip pitch| (rotation offset: the hip one; view: 0)."""
    rec = idle_record(stem, idle_path)
    if rec and rec.get('idle'):
        sets = {s: (rec['idle']['s%dHip' % s], rec['idle']['s%dAds' % s]) for s in (1, 2)}
        return bool(rec['use']), bool(rec['random']), rec['gasp'], sets, 'AdvancedIdleSettings from the game data (iw8_advidle.json)'
    W = top['weapDef']
    gk = lambda k: W.get('advanced' + k, 0.0)
    flags = int(round(gk('hipIdleSpeed') / 1.401298e-45)) if gk('hipIdleSpeed') else 0
    hip = dict(zip(FIELDS9, [gk('hipWeaponMagnitudeY'), gk('hipWeaponMagnitudeZ'), gk('hipWeaponRotationOffset'), gk('hipViewMagnitudeX'),
                             gk('hipViewMagnitudeY'), gk('adsIdleSpeed'), gk('adsWeaponMagnitudeX'), gk('adsWeaponMagnitudeY'),
                             gk('adsWeaponMagnitudeZ')]))
    ads = dict(zip(FIELDS9, [gk('adsWeaponRotationOffset'), gk('adsViewMagnitudeX'), gk('adsViewMagnitudeY'), 0, 0, 0, 0, 0, 0]))
    r = abs(ads['WeaponMagnitudeX'] / hip['WeaponMagnitudeX']) if hip['WeaponMagnitudeX'] else 0.0
    for k in ('WeaponMagnitudeY', 'WeaponMagnitudeZ', 'WeaponMagnitudeF'):
        ads[k] = hip[k] * r
    ads['WeaponRotationOffset'] = hip['WeaponRotationOffset']
    return (bool(flags & 1), bool(flags & 0x100), gk('hipWeaponMagnitudeX'), {1: (hip, ads)},
            'FALLBACK: setting-1 hip + ADS speed/pitch from the dump; ADS yaw/roll/push = hip x %.2f, rotation = hip, view 0; no setting 2' % r)


def sway_lines(top, name, att_tops=(), idle_path=None, stem=None):
    """sway_* lines for one weapon (formats: bo3_sway.h). Returns (lines, info)."""
    W = top['weapDef']
    stem = stem or top.get('szInternalName', '')
    sw, used = merged_sway(top, att_tops)
    a = sw['adv']
    v2 = lambda d: '%s,%s' % (g(d['X']), g(d['Y']))
    v3 = lambda d: '%s,%s,%s' % (g(d['X']), g(d['Y']), g(d['Z']))
    out, info = [], dict(adv=False, idle=False, attachments=used)
    p = out.append
    if used:
        p('# %s: swaySettings merged from %s' % (name, ', '.join(used)))
    if a.get('enabled'):
        info['adv'] = True
        p('sway_adv=%s,%s,%s,%d,%s,%s,%s,%s,%s,%s' % (name, g(a['torsoGoalSmoothSpeed']), g(a['fireTorsoGoalSmoothSpeed']),
          a['torsoGoalViewSmoothDurationMs'], v2(a['torsoGoalDeadzoneAdjustSpeed']), v2(a['torsoGoalViewSpeedToMaxDeadzone_viewspeed']),
          v2(a['torsoGoalViewSpeedToMaxDeadzone_maxDeadzone']), v2(a['torsoMass']), v2(a['torsoSpring']), v2(a['torsoDamper'])))
        p('sway_advgun=%s,%d,%s,%s,%s,%s,%s,%s,%s' % (name, a['gunGoalViewSmoothDurationMs'], v2(a['gunGoalViewSpeedToOffset_viewspeed']),
          v2(a['gunGoalViewSpeedToOffset_offset']), v2(a['gunMass']), v2(a['gunSpring']), v2(a['gunDamper']), v3(a['gunPivotPoint']),
          g(a['gunYawToRollScale'])))
        p('sway_advfire=%s,%d,%d,%d,%s,%s' % (name, a['fireDurationMs'], a['fireStartBlendDurationMs'], a['fireFinishBlendDurationMs'],
          g(a['fireTorsoDeadzoneScale']), g(a['fireTorsoToGunDirScale'])))
    use, rnd, gasp, sets, note = idle_settings(top, stem, idle_path)
    info['idle_note'] = note
    if use:
        if rnd:
            p('# %s: useRandomPointsAlgorithm is set; bo3_sway.h has only the sine path' % name)
        for s, (h, d) in sorted(sets.items()):
            vals = [h['IdleSpeed'], d['IdleSpeed']] + [h[k] for k in FIELDS9[2:7]] + [d[k] for k in FIELDS9[2:7]] + \
                   [h['ViewMagnitudeX'], h['ViewMagnitudeY'], d['ViewMagnitudeX'], d['ViewMagnitudeY']]
            if not any(vals[2:]):
                continue
            info['idle'] = True
            p('sway_idle=%s,%d,%s' % (name, s, ','.join(g(x) for x in vals)))
        if info['idle']:
            p('sway_idlemisc=%s,%s,%s,%s' % (name, g(W['fIdleCrouchFactor']), g(W['fIdleProneFactor']), g(gasp)))
            if not note.startswith('AdvancedIdleSettings from'):
                p('# %s idle: %s' % (name, note))
    st = [W['vStandOfs'], W['vStandOfsRot'], W['vStandOfsRotPivot'], W['vDuckedOfsRot'], W['vDuckedOfsRotPivot']]
    if any(v for d in (W['vStandOfs'], W['vStandOfsRot'], W['vDuckedOfsRot']) for v in d.values()):
        p('sway_stance=%s,%s,%s' % (name, ','.join(v3(d) for d in st), g(W['fDuckedOfsRotRate'])))
    bob = [W[k] for k in ('fAdsGunBobPitchScale', 'fAdsGunBobYawScale', 'fAdsGunBobTiltPitchScale', 'fAdsGunBobTiltYawScale',
                          'fAdsGunBobTiltRollScale')]
    if any(bob):
        p('sway_adsbob=%s,%s,%s,%s' % (name, ','.join(g(x) for x in bob), g(W['fAdsGunBobTiltOffset']), g(W['fAdsGunBobCrouchFactor'])))
    return out, info


GDT_VEC = [('standMove', 'vStandMove', 'FRU'), ('standRot', 'vStandRot', 'PYR'), ('duckedMove', 'vDuckedMove', 'FRU'),
           ('duckedRot', 'vDuckedRot', 'PYR'), ('proneMove', 'vProneMove', 'FRU'), ('proneRot', 'vProneRot', 'PYR'),
           ('strafeMove', 'strafeMove', 'FRU'), ('strafeRot', 'strafeRot', 'PYR'), ('duckedOfs', 'vDuckedOfs', 'FRU'),
           ('proneOfs', 'vProneOfs', 'FRU')]
GDT_SCALAR = [('posMoveRate', 'fPosMoveRate'), ('posRotRate', 'fPosRotRate'), ('posProneMoveRate', 'fPosProneMoveRate'),
              ('posProneRotRate', 'fPosProneRotRate'), ('standMoveMinSpeed', 'fStandMoveMinSpeed'), ('standRotMinSpeed', 'fStandRotMinSpeed'),
              ('duckedMoveMinSpeed', 'fDuckedMoveMinSpeed'), ('duckedRotMinSpeed', 'fDuckedRotMinSpeed'),
              ('proneMoveMinSpeed', 'fProneMoveMinSpeed'), ('proneRotMinSpeed', 'fProneRotMinSpeed'), ('adsViewBobMult', 'fAdsViewBobMult')]
# Every key this generator owns in a BO3 weapon GDT block (nothing else is touched).
GDT_KEYS = ['adsSwayMaxAngle', 'adsSwayLerpSpeed', 'adsSwayTransitionLerpSpeed', 'adsSwayPitchScale', 'adsSwayYawScale',
            'adsSwayHorizScale', 'adsSwayVertScale', 'swayMaxAngle', 'hipIdleAmount', 'adsIdleAmount'] + \
           [b + c for b, _, cs in GDT_VEC for c in cs] + [b for b, _ in GDT_SCALAR]


# bulletweapon.awi ranges of the keys above (values outside them are clamped, and said so)
GDT_RANGE = {'adsSwayMaxAngle': (0, 180), 'adsSwayLerpSpeed': (1, 50), 'adsSwayTransitionLerpSpeed': (0, 50), 'swayMaxAngle': (0, 180),
             'adsViewBobMult': (0, 100), 'hipIdleAmount': (0, 150), 'adsIdleAmount': (0, 150)}
for _k in ('adsSwayPitchScale', 'adsSwayYawScale', 'adsSwayHorizScale', 'adsSwayVertScale'):
    GDT_RANGE[_k] = (-2, 2)
for _b, _, _cs in GDT_VEC:
    for _c in _cs:
        GDT_RANGE[_b + _c] = (-300, 300)
for _b, _ in GDT_SCALAR:
    GDT_RANGE.setdefault(_b, (0, 1000 if 'Rot' in _b and 'MinSpeed' in _b else 3000 if 'MinSpeed' in _b else 300))


def gdt_values(top, att_tops=(), adv_on=True, idle_on=True, notes=None):
    """Phase 1: BO3 GDT key -> value (strings). IW8 X,Y,Z = BO3 F,R,U / P,Y,R [C3, same formula].
    `notes` (a list) collects what was clamped or kept."""
    notes = [] if notes is None else notes
    out = _gdt_values(top, att_tops, adv_on, idle_on)
    W = top['weapDef']
    # IW-style "gun off screen while crawling" (e.g. sn_sksierra vProneMove -160,0,-120 / vProneRot 0,300,-300): in BO3
    # the gun would vanish whenever prone and moving, so BO3's own prone move / rot stay.
    if any(abs(v) > 30 for v in W['vProneMove'].values()) or any(abs(v) > 60 for v in W['vProneRot'].values()):
        for k in [k for k in out if k.startswith(('proneMove', 'proneRot')) and 'MinSpeed' not in k]:
            del out[k]
        notes.append('vProneMove %s / vProneRot %s hide the gun while crawling; BO3 proneMove / proneRot kept' % (
            ','.join(g(v) for v in W['vProneMove'].values()), ','.join(g(v) for v in W['vProneRot'].values())))
    for k, v in out.items():
        lo, hi = GDT_RANGE.get(k, (None, None))
        if lo is not None and not lo <= float(v) <= hi:
            out[k] = g(min(max(float(v), lo), hi))
            notes.append('%s %s clamped to the GDF range: %s' % (k, v, out[k]))
    return out


def _gdt_values(top, att_tops, adv_on, idle_on):
    W = top['weapDef']
    sw, _ = merged_sway(top, att_tops)
    d = sw['ads']
    out = {}
    # BG_CalculateWeaponMovement_Sway: maxAngle x adsSwayScale0, lerp (either) x adsSwayScale1, scales x adsSwayScale2
    out['adsSwayMaxAngle'] = g(d['maxAngle'] * d['adsSwayScale0'])
    out['adsSwayLerpSpeed'] = g(d['lerpSpeed'] * d['adsSwayScale1'])
    out['adsSwayTransitionLerpSpeed'] = g(d['swayTransitionLerpSpeed'] * d['adsSwayScale1'])
    for bo3, iw in (('Pitch', 'pitch'), ('Yaw', 'yaw'), ('Horiz', 'horiz'), ('Vert', 'vert')):
        out['adsSway%sScale' % bo3] = g(d['%sScale' % iw] * d['adsSwayScale2'])
    if adv_on:
        out['swayMaxAngle'] = '0'  # the hip look sway is advanced sway (DLL)
    if idle_on:
        out['hipIdleAmount'] = '0'  # advanced idle (DLL)
        out['adsIdleAmount'] = '0'
    for bo3, iw, comps in GDT_VEC:
        for c, axis in zip(comps, 'XYZ'):
            out[bo3 + c] = g(W[iw][axis])
    for bo3, iw in GDT_SCALAR:
        out[bo3] = g(W[iw])
    return out


def main(argv):
    if len(argv) < 2 or argv[0] in ('-h', '--help'):
        with open(__file__, encoding='utf-8') as f:
            print(''.join(l[2:] for l in f.readlines()[:17] if l.startswith('#')), end='')
        return
    flags = {a for a in argv if a in ('--sway', '--gdt', '--wop')}
    args, atts, idle_path, i = [], [], None, 0
    rest = [a for a in argv if a not in flags]
    while i < len(rest):
        if rest[i] == '--att':
            atts.append(rest[i + 1]); i += 2
        elif rest[i] == '--idle':
            idle_path = rest[i + 1]; i += 2
        else:
            args.append(rest[i]); i += 1
    src, name = args[0], args[1]
    top = json.load(open(src, encoding='utf-8'))
    att_tops = [json.load(open(a, encoding='utf-8')) for a in atts]
    stem = os.path.splitext(os.path.basename(src))[0]
    if not flags or '--wop' in flags:
        print('\n'.join(wop_lines(top, name, src, args[2] if len(args) > 2 else None)))
    info = None
    if '--sway' in flags or '--gdt' in flags:
        lines, info = sway_lines(top, name, att_tops, idle_path, stem)
    if '--sway' in flags:
        print('# %s sway from %s' % (name, os.path.basename(src)))
        print('\n'.join(lines))
    if '--gdt' in flags:
        print('# %s GDT (phase 1) from %s%s' % (name, os.path.basename(src), '; ' + ', '.join(info['attachments']) if info['attachments'] else ''))
        notes = []
        for k, v in gdt_values(top, att_tops, info['adv'], info['idle'], notes).items():
            print('"%s" "%s"' % (k, v))
        for n in notes:
            print('# note: %s' % n)


if __name__ == '__main__':
    main(sys.argv[1:])
