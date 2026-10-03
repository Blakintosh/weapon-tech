#!/usr/bin/env python3
"""Migrate a flat weapon_tech.cfg to the sectioned layout (format v2), keeping what the DLL does exactly the same.

    python cfg_migrate.py <old.cfg> [-o <new.cfg>] [--check] [--dump-tool <wt_cfgdump.exe>] [--in-place]

What it does:
  * [features] is derived from the old switches: every *_enable key, empty_melee_fix, the global wop_kickreturn,
    the global empty_lastshot and the global vmfov are folded into it. A live A/B switch that is 0
    (additive_enable, locomotion_enable, idle_active_enable, sway_enable) stays in its section, so it still fades.
  * Every other line moves into its feature's [section], keeping its relative order, its full key name and the
    comments above it. Lines the DLL would not have read the same way inside a section (leading blanks, spaces around
    '=') stay at the top, before [features], where the flat format still applies.
  * Simple per-gun lines collapse into lists when every line of that key round-trips exactly:
        inspect=<gun>,1[,<s>]          -> [inspect]    guns = <gun>[:<s>]     (<gun>:off for ,0)
        ik=<gun>,<1|0>[,hands[,..]]    -> [ik]         guns = <gun>[:off][:lr[:w[:orient]]]
        segreload_empty=<gun>,<mode>   -> [segreload]  guns = <gun>:<mode>
        empty_lastshot=<gun>,<mode>    -> [last_shot]  guns = <gun>:<mode>
        ammohide_auto=<gun>,0          -> [ammo_hide]  auto_off = <gun>
        slots_take_jukes=<gun>         -> [additives]  take_jukes = <gun>
    Lines inside a generator block are never collapsed (the generator writes them again).
  * Generator blocks ("# ==== BEGIN <title> ====" .. END) stay whole. The four known generators' fences become
    "# ==== BEGIN generated:<tool> ====" with the block's own [section] header inside, the form the generators now
    write; hand-written fenced blocks keep their fences.

--check runs wt_cfgdump.exe (weapon_tech.dll's own parser, offline) on the old and the new file and compares the parsed
state table by table: the migration is only good when every hash matches.
"""
import argparse
import datetime
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))

SECTION_ORDER = ['general', 'additives', 'ammo_hide', 'kick', 'camera', 'locomotion', 'inspect', 'last_shot', 'empty_melee',
                 'interrupts', 'segreload', 'slide', 'vmfov', 'ik']
SECTION_NOTES = {
    'general': 'perf_* and debug switches, and keys no feature owns',
    'additives': 'additive= layers (empty / recoil / bullet, purpose slots), slots_*',
    'ammo_hide': 'ammohide= joints, hide order / spend / reverse, the round guard',
    'kick': 'IW8 weapon offset patterns and kick: wop_* (wop_alias gives a variant its source\'s block)',
    'camera': 'cam_shake / camera_free (global, then per gun)',
    'locomotion': 'walk / jog (locomotion*), idle_active*, sway_*',
    'inspect': 'MW inspect: key, empty, hidehud; guns = <gun>[:<seconds>] (<gun>:off to turn one off)',
    'last_shot': 'empty_lastshot per gun: guns = <gun>:<auto|iw|hold>',
    'empty_melee': 'empty-gun melee and raise fixes',
    'interrupts': 'IW-style interrupts (experimental): interrupt=<gun>,<states>,<source>[,<actions>]',
    'segreload': 'empty segmented reloads: guns = <gun>:<end|start|off>',
    'slide': 'MW2019 slide (slide_enable is [features] slide)',
    'vmfov': 'viewmodel FOV pin options (the mode is [features] vmfov)',
    'ik': 'hand IK: guns = <gun>[:off][:<l|r|lr>[:<noteless weight>[:<orient>]]], ik_notes=, ik_debug=',
}

GENERATORS = [  # (title start in the old fence, tool id)
    ('bullet/empty additives', 'build_bullet_empty_additives'),
    ('slot additives', 'build_bullet_empty_additives.slots'),
    ('BO7 fill', 'build_bo7_fill'),
    ('ammohide order', 'ammohide_order'),
]

PREFIX = {'additives': 'additive', 'ammo_hide': 'ammohide', 'kick': 'wop', 'locomotion': 'locomotion', 'inspect': 'inspect',
          'last_shot': 'empty_lastshot', 'interrupts': 'interrupt', 'segreload': 'segreload', 'slide': 'slide',
          'vmfov': 'vmfov', 'ik': 'ik'}
FAMILIES = ['additive', 'slots', 'belt_dump', 'ammohide', 'wop', 'cam_shake', 'camera_free', 'locomotion', 'idle_active',
            'sway', 'inspect', 'empty_lastshot', 'empty_melee_fix', 'additive_melee_fade', 'interrupt', 'segreload', 'slide',
            'vmfov', 'ik', 'perf', 'cfg_dump', 'feature']  # wt_cfgv2.h kWtcFamilies
TOOL_SECTION = {'ammohide_order': 'ammo_hide'}  # a generator block with no lines yet goes here (else additives)
WEAPONISH = re.compile(r'^[A-Za-z][A-Za-z0-9]*_[A-Za-z0-9_]*$')


def short_line(ln, sec):
    """'inspect_key=I' in [inspect] -> 'key = I' (a section's own global keys); per-gun lines keep their full key"""
    pre = PREFIX.get(sec)
    if not pre or not ln.key.startswith(pre + '_'):
        return ln.text
    short = ln.key[len(pre) + 1:]
    if any(short == f or short.startswith(f + '_') for f in FAMILIES):
        return ln.text
    first = ln.value.split(',')[0].strip()
    if WEAPONISH.match(first) or first == '*':
        return ln.text
    return '%s = %s' % (short, ln.value)


FENCE_RE = re.compile(r'^#\s*====\s*(BEGIN|END)\s+(.*?)\s*====\s*$')
GEN_RE = re.compile(r'^#\s*====\s*(BEGIN|END)\s+generated:(\S+)')


def section_of(key):
    if key in ('empty_melee_fix', 'additive_melee_fade', 'interrupt_empty_melee'):
        return 'empty_melee'
    fams = [('additive', 'additives'), ('slots', 'additives'), ('belt_dump', 'additives'), ('ammohide', 'ammo_hide'),
            ('wop', 'kick'), ('cam_shake', 'camera'), ('camera_free', 'camera'), ('locomotion', 'locomotion'),
            ('idle_active', 'locomotion'), ('sway', 'locomotion'), ('inspect', 'inspect'),
            ('empty_lastshot', 'last_shot'), ('interrupt', 'interrupts'), ('segreload', 'segreload'),
            ('slide', 'slide'), ('vmfov', 'vmfov'), ('ik', 'ik')]
    for fam, sec in fams:
        if key == fam or key.startswith(fam + '_'):
            return sec
    return 'general'


def split_comment(value):
    """value -> (value without an inline comment, the comment text or '')"""
    i = value.find('#')
    if i < 0:
        return value.rstrip(), ''
    return value[:i].rstrip(), value[i + 1:].strip()


# ---- guns lists: line value <-> list entry, both ways, so a collapse is only done when it round-trips -----------------
def onoff_entry(v):
    f = v.split(',')
    if len(f) < 2 or not f[0] or f[1] not in ('0', '1'):
        return None
    rest = f[2:]
    e = f[0] + (':off' if f[1] == '0' else '')
    if rest:
        e += ':' + ':'.join(rest)
    return e


def onoff_expand(e):
    g, _, p = e.partition(':')
    mode, rest = '1', p
    first = p.split(':')[0] if p else ''
    if first in ('on', 'off'):
        mode = '1' if first == 'on' else '0'
        rest = p[len(first) + 1:] if len(p) > len(first) else ''
    return g + ',' + mode + (',' + rest.replace(':', ',') if rest else '')


def mode_entry(v):
    f = v.split(',')
    if len(f) != 2 or not f[0] or not f[1]:
        return None
    return f[0] + ':' + f[1]


def mode_expand(e):
    g, _, p = e.partition(':')
    return g + ',' + p.replace(':', ',')


def fixed0_entry(v):
    f = v.split(',')
    return f[0] if len(f) == 2 and f[0] and f[1] == '0' else None


def bare_entry(v):
    return v if v and ',' not in v and ' ' not in v and v.lower() != 'all' else None


LISTS = {  # cfg key -> (section, list key, entry(), expand())
    'inspect': ('inspect', 'guns', onoff_entry, onoff_expand),
    'ik': ('ik', 'guns', onoff_entry, onoff_expand),
    'segreload_empty': ('segreload', 'guns', mode_entry, mode_expand),
    'empty_lastshot': ('last_shot', 'guns', mode_entry, mode_expand),
    'ammohide_auto': ('ammo_hide', 'auto_off', fixed0_entry, lambda e: e + ',0'),
    'slots_take_jukes': ('additives', 'take_jukes', bare_entry, lambda e: e),
}


class Line:
    def __init__(self, text, lead):
        self.text = text            # the line, no newline
        self.lead = lead            # comment / blank lines above it
        self.key = self.value = None
        self.clean = False
        m = re.match(r'^([A-Za-z_][A-Za-z0-9_]*)=(.*)$', text)
        if m:
            self.key, self.value = m.group(1), m.group(2)
            self.clean = True
        self.section = section_of(self.key) if self.clean else None
        self.fence = None           # the Fence it sits in


class Fence:
    def __init__(self, begin, lead):
        self.begin = begin
        self.end = None
        self.lead = lead
        self.body = []              # Line objects and comment strings, in order
        m = FENCE_RE.match(begin)
        self.title = m.group(2) if m else ''
        self.tool = next((t for start, t in GENERATORS if self.title.startswith(start)), None)
        g = GEN_RE.match(begin)
        if g:
            self.tool = g.group(2)


def read_units(text):
    """-> header comment lines, units (Line / Fence), trailing comment lines"""
    raw = text.replace('\r\n', '\n').split('\n')
    if raw and raw[-1] == '':
        raw.pop()
    header = []
    i = 0
    while i < len(raw) and raw[i].startswith('#'):
        header.append(raw[i])
        i += 1
    units, lead, fence = [], [], None
    for t in raw[i:]:
        m = FENCE_RE.match(t)
        if m and m.group(1) == 'BEGIN' and fence is None:
            fence = Fence(t, lead)
            lead = []
            continue
        if m and m.group(1) == 'END' and fence is not None:
            fence.end = t
            fence.body.extend(lead)
            lead = []
            units.append(fence)
            fence = None
            continue
        s = t.strip()
        if not s or s.startswith('#') or s.startswith('//'):
            lead.append(t)
            continue
        ln = Line(t, lead)
        lead = []
        if fence is not None:
            ln.fence = fence
            fence.body.append(ln)
        else:
            units.append(ln)
    if fence is not None:  # an unterminated fence: keep its lines as they were
        units.append(fence)
    return header, units, lead


def all_lines(units):
    for u in units:
        if isinstance(u, Fence):
            for b in u.body:
                if isinstance(b, Line):
                    yield b
        else:
            yield u


def last_value(lines, key):
    v = None
    for ln in lines:
        if ln.clean and ln.key == key:
            v = ln.value
    return v


def is_global(ln):
    return ln.clean and ',' not in split_comment(ln.value)[0]


def derive_features(units):
    """-> (ordered [(name, value, note)], set of id(Line) folded into [features])"""
    lines = list(all_lines(units))
    folded = set()
    feats = []

    def fold(key, pred=lambda ln: True):
        for ln in lines:
            if ln.clean and ln.key == key and pred(ln):
                folded.add(id(ln))

    def ival(v, default):
        if v is None:
            return default
        m = re.match(r'\s*(-?\d+)', v)
        return int(m.group(1)) if m else default

    has = lambda pre: any(ln.clean and (ln.key == pre or ln.key.startswith(pre + '_')) for ln in lines)
    # A/B switches: on; a 0 stays in its section (live fade)
    a = ival(last_value(lines, 'additive_enable'), 1)
    if a:
        fold('additive_enable')
    feats.append(('additives', 'on', 'additive= layers' + ('' if a else ' (additive_enable = 0 in [additives] fades them)')))
    feats.append(('ammo_hide', 'on', 'spent-round hiding, round guard'))
    feats.append(('kick', 'on', 'IW8 WOP kick + patterns' + ('' if has('wop') else ' (no wop lines)')))
    kr = last_value([ln for ln in lines if is_global(ln)], 'wop_kickreturn')
    fold('wop_kickreturn', is_global)
    feats.append(('kick_return', 'on' if ival(kr, 0) else 'off', 'IW8 view-kick return (per gun: wop_kickreturn= in [kick])'))
    feats.append(('camera', 'on', 'cam_shake / camera_free'))
    loco_off = []
    for k in ('locomotion_enable', 'idle_active_enable', 'sway_enable'):
        if ival(last_value(lines, k), 1):
            fold(k)
        else:
            loco_off.append(k)
    feats.append(('locomotion', 'on', 'walk / jog / idle_active / sway' + (' (%s = 0 in [locomotion])' % ', '.join(loco_off) if loco_off else '')))
    ins = ival(last_value(lines, 'inspect_enable'), 0)
    fold('inspect_enable')
    feats.append(('inspect', 'on' if ins else 'off', 'MW inspect'))
    el = last_value([ln for ln in lines if is_global(ln)], 'empty_lastshot')
    fold('empty_lastshot', is_global)
    el_mode = split_comment(el)[0].strip().split()[0].lower() if el and split_comment(el)[0].strip() else 'auto'
    feats.append(('last_shot', el_mode, 'empty layer on the last shot: auto | iw | hold'))
    em = None
    for ln in lines:
        if ln.clean and ln.key == 'empty_melee_fix':
            if ln.text == 'empty_melee_fix=1':
                em = 1
            elif ln.text == 'empty_melee_fix=0':
                em = 0
            else:  # a line the DLL doesn't take as 0 or 1: leave it where it is
                continue
            folded.add(id(ln))
    feats.append(('empty_melee', 'on' if em else 'off', 'an empty gun keeps its own melee and normal raise / drop'))
    ie = ival(last_value(lines, 'interrupt_enable'), 1)
    fold('interrupt_enable')
    feats.append(('interrupts', 'on' if ie else 'off', 'experimental' + ('' if any(ln.clean and ln.key == 'interrupt' for ln in lines) else '; no interrupt= lines, so nothing installs')))
    se = ival(last_value(lines, 'segreload_enable'), 1)
    fold('segreload_enable')
    feats.append(('segreload', 'on' if se else 'off', 'empty segmented reloads'))
    sl = ival(last_value(lines, 'slide_enable'), 0)
    fold('slide_enable')
    feats.append(('slide', 'on' if sl else 'off', 'MW2019 slide (map-wide)'))
    vm = last_value([ln for ln in lines if is_global(ln)], 'vmfov')
    fold('vmfov', is_global)
    vm = split_comment(vm)[0].strip() if vm else 'off'
    feats.append(('vmfov', vm or 'off', 'viewmodel FOV pin: off | mw | <deg>'))
    ik_lines = any(ln.clean and ln.key == 'ik' for ln in lines)
    ike = ival(last_value(lines, 'ik_enable'), 0)
    if ike or not ik_lines:
        fold('ik_enable')
        feats.append(('ik', 'on' if ike else 'off', 'experimental hand IK'))
    else:  # ik= lines with ik_enable 0: the hooks still go in (calibration / ik_debug); keep the old keys deciding
        feats.append(('# ik', None, 'not set here: ik_enable = 0 with ik= lines stays in [ik] (hooks in, solve off), as before'))
    return feats, folded


def collapsible(units, folded):
    """cfg keys whose lines all become guns-list entries"""
    out = {}
    for key, (sec, lk, entry, expand) in LISTS.items():
        ls = [ln for ln in all_lines(units) if ln.clean and ln.key == key and id(ln) not in folded]
        if not ls or any(ln.fence for ln in ls):
            continue
        ok = True
        for ln in ls:
            v, _ = split_comment(ln.value)
            e = entry(v)
            if e is None or expand(e) != v:
                ok = False
                break
        if ok:
            out[key] = ls
    return out


def tidy_blank(lines):
    out = []
    for t in lines:
        if not t.strip() and (not out or not out[-1].strip()):
            continue
        out.append(t)
    while out and not out[-1].strip():
        out.pop()
    return out


def migrate(text, source_name):
    header, units, trailing = read_units(text)
    feats, folded = derive_features(units)
    lists = collapsible(units, folded)
    listed = {id(ln) for ls in lists.values() for ln in ls}

    top = []                                  # lines the flat format must keep reading (before [features])
    secs = {s: [] for s in SECTION_ORDER}     # section -> output lines
    carry = []                                # comments of folded lines flow to the next unit

    def emit_line(ln, into):
        lead = carry + ln.lead
        carry.clear()
        if id(ln) in folded or id(ln) in listed:
            carry.extend(lead)
            return
        if not ln.clean:
            top.extend(lead + [ln.text])
            return
        into.extend(lead + [short_line(ln, ln.section)])

    for u in units:
        if isinstance(u, Line):
            if u.clean and id(u) not in folded and id(u) not in listed:
                emit_line(u, secs[u.section])
            else:
                emit_line(u, top)
            continue
        # a fence: whole, in the section most of its lines belong to
        body = [b for b in u.body if isinstance(b, Line) and b.clean and id(b) not in folded]
        counts = {}
        for b in body:
            counts[b.section] = counts.get(b.section, 0) + 1
        sec = max(counts, key=counts.get) if counts else TOOL_SECTION.get(u.tool, 'additives')
        out = secs[sec]
        out.extend(carry + u.lead)
        carry.clear()
        if u.tool:
            out.append('# ==== BEGIN generated:%s ====' % u.tool)
            out.append('[%s]' % sec)
        else:
            out.append(u.begin)
        for b in u.body:
            if isinstance(b, str):
                out.append(b)
                continue
            out.extend(b.lead)
            if id(b) in folded:
                continue
            if not b.clean:
                top.append(b.text)  # can't stay in a section; the flat area still reads it the old way
                continue
            if b.section != sec:
                out.extend(['[%s]' % b.section, b.text, '[%s]' % sec])
            else:
                out.append(b.text)
        if u.tool:
            out.append('# ==== END generated:%s ====' % u.tool)
        elif u.end:
            out.append(u.end)

    # guns lists, at the top of their section
    for key, ls in lists.items():
        sec, lk, entry, _ = LISTS[key]
        notes, entries = [], []
        for ln in ls:
            notes.extend(t for t in ln.lead if t.strip())
            v, c = split_comment(ln.value)
            e = entry(v)
            entries.append(e)
            if c:
                notes.append('# %s: %s' % (e.split(':')[0], c))
        block = notes[:]
        cur = ''
        for e in entries:
            if cur and len(cur) + len(e) + 2 > 200:
                block.append('%s = %s' % (lk, cur))
                cur = ''
            cur = e if not cur else cur + ', ' + e
        if cur:
            block.append('%s = %s' % (lk, cur))
        secs[sec][0:0] = block + ['']

    out = header[:] if header else ['# weapon_tech.cfg']
    out += ['#',
            '# Format v2 (sectioned): [features] switches each feature on or off; every other section holds that feature\'s',
            '# lines. Full key names work in any section; inside one, the feature prefix may be left out. Reference:',
            '# docs/CONFIG_REFERENCE.md in the weapon-tech repo. Migrated from the flat cfg by cfg_migrate.py on %s;' % datetime.date.today(),
            '# the flat original is %s.pre_cfgv2.bak.' % source_name]
    if top or carry and False:
        out += ['', '# ---- read in the flat format (before the first [section]): lines a section would read differently']
        out += tidy_blank(top)
    out += ['', '[features]']
    w = max(len(n) for n, v, _ in feats if v is not None)
    for n, v, note in feats:
        if v is None:
            out.append('%s %s' % (n, note))
        else:
            out.append('%-*s = %-5s  # %s' % (w, n, v, note))
    for s in SECTION_ORDER:
        body = tidy_blank(secs[s])
        while body and not body[0].strip():
            body.pop(0)
        if not body:
            continue
        out += ['', '[%s]' % s, '# ' + SECTION_NOTES[s]]
        out += body
    tail = tidy_blank(carry + trailing)
    if any(t.strip() for t in tail):
        out += [''] + tail
    return '\n'.join(out) + '\n'


def find_dump_tool(arg):
    for p in ([arg] if arg else []) + [os.environ.get('WT_CFGDUMP'), os.path.join(HERE, 'wt_cfgdump.exe'),
                                       os.path.join(HERE, '..', 'build', 'wt_cfgdump.exe')]:
        if p and os.path.exists(p):
            return p
    return None


def parse_dump(path):
    tables = {}
    for t in open(path, encoding='utf-8', errors='replace'):
        m = re.match(r'^(\S+) (?:count=\d+ )?hash=([0-9a-f]+)', t)
        if m:
            tables[m.group(1)] = t.strip()
    return tables


def check(old, new, tool):
    """-> True when weapon_tech's parsed state is the same for both files"""
    tmp = tempfile.mkdtemp(prefix='wtcfg_')
    res = {}
    for tag, path in (('old', old), ('new', new)):
        d = os.path.join(tmp, tag + '.dump')
        r = subprocess.run([tool, path, d, os.path.join(tmp, tag + '.norm')], capture_output=True, text=True)
        res[tag] = (r, d)
        summary = [ln for ln in r.stderr.splitlines() if 'features (' in ln]
        print('%s: %s  %s' % (tag, r.stdout.strip(), summary[-1].split('weapon_tech: ', 1)[-1] if summary else ''))
        for ln in r.stderr.splitlines():
            if 'bad' in ln or 'unknown' in ln or 'warning' in ln.lower():
                print('   ' + ln)
    a, b = parse_dump(res['old'][1]), parse_dump(res['new'][1])
    same = True
    for k in sorted(set(a) | set(b)):
        if a.get(k) != b.get(k):
            same = False
            print('DIFF %s\n  old: %s\n  new: %s' % (k, a.get(k), b.get(k)))
    print('parsed state: %s (dumps in %s)' % ('IDENTICAL' if same else 'DIFFERENT', tmp))
    return same


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('cfg')
    ap.add_argument('-o', '--out', help='new file (default: <cfg>.v2)')
    ap.add_argument('--in-place', action='store_true', help='back up <cfg>.pre_cfgv2.bak and replace <cfg>')
    ap.add_argument('--check', action='store_true', help='compare the parsed state of old and new with wt_cfgdump.exe')
    ap.add_argument('--dump-tool')
    ap.add_argument('--check-only', metavar='NEW', help='only compare <cfg> with an existing NEW file')
    args = ap.parse_args()
    if args.check_only:
        tool = find_dump_tool(args.dump_tool)
        sys.exit(0 if tool and check(args.cfg, args.check_only, tool) else 1)
    text = open(args.cfg, encoding='utf-8', errors='replace').read()
    if any(re.match(r'^\s*\[[^\]]+\]', t) for t in text.split('\n')):
        sys.exit('%s already has [sections]: nothing to migrate' % args.cfg)
    new = migrate(text, os.path.basename(args.cfg))
    out = args.out or args.cfg + '.v2'
    if args.in_place:
        out = args.cfg + '.v2.tmp'
    open(out, 'w', encoding='utf-8', newline='\n').write(new)
    ok = True
    if args.check:
        tool = find_dump_tool(args.dump_tool)
        if not tool:
            sys.exit('wt_cfgdump.exe not found (build.ps1 builds it into build\\; or --dump-tool / WT_CFGDUMP)')
        ok = check(args.cfg, out, tool)
    if args.in_place:
        if not ok:
            os.remove(out)
            sys.exit('parsed state differs: %s left unchanged' % args.cfg)
        bak = args.cfg + '.pre_cfgv2.bak'
        if not os.path.exists(bak):
            os.replace(args.cfg, bak)
        os.replace(out, args.cfg)
        out = args.cfg
        print('backup: ' + bak)
    print('wrote ' + out)
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
