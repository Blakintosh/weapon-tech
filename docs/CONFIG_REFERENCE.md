# weapon_tech.cfg reference

Every key `weapon_tech.dll` reads, taken from the parsers in `src/` (weapon_tech.h and the `bo3_*.h` headers). Where a
header comment and the code disagree, this document follows the code. A commented example with a working line for each
feature is in `examples/weapon_tech.cfg`.

The file layout comes first (format v2: `[features]`, one section per feature, guns lists,
`[weapon:<name>]`, generated blocks), then the keys, grouped by the section they live in.

## File structure (format v2)

A cfg is either the old **flat** list of `key=value` lines or the **sectioned** layout below. weapon_tech tells them
apart by the first `[section]` header. A file without one is read exactly as before. A sectioned file is turned into
flat lines first (`src/wt_cfgv2.h`), so every key in this reference works in both. The weapontech linker feature uses the
same code, so its checks read the same lines. `tools/cfg_migrate.py` converts a flat file and proves the result
configures the DLL identically (see [Migrating](#migrating-a-flat-cfg)).

```ini
# lines before the first [section] are read the flat way (old files, or anything you want kept as is)

[features]            # master switches: an off feature installs nothing, even with its lines in the file
additives   = on
ammo_hide   = on
kick        = on      # IW8 WOP kick + patterns
kick_return = off
camera      = on      # cam_shake / camera_free
locomotion  = on      # idle_active / walk / jog / sway
inspect     = on
last_shot   = auto    # auto | iw | hold: the default for every gun
empty_melee = on
interrupts  = off     # experimental
segreload   = on
slide       = off     # map-wide
vmfov       = off     # off | mw | <deg>
ik          = off     # experimental

[inspect]
key  = I              # short for inspect_key
guns = smg_a_zm, smg_b_zm:6.6, smg_c_zm:off

[additives]
additive=smg_a_zm,recoil,195,vm_smg_a_recoil_additive,1.0,1.2   # per-gun detail lines keep their full key

[weapon:smg_d_zm]     # one gun's settings across features, as a GDT compiler writes them
wtInspect = on
wtFireTimeMs = 80
```

### Sections

| Section | Holds | Short keys (prefix added) |
|---|---|---|
| `[features]` | the master switches (below) | |
| `[general]` | `perf_*`, `cfg_dump`, anything no feature owns | none: keys as written |
| `[additives]` | `additive=`, `slots_*`, `belt_dump`, `additive_*` | `additive_` (`enable` = `additive_enable`) |
| `[ammo_hide]` | `ammohide*` | `ammohide_` (`guard`, `park`, `order`, ...) |
| `[kick]` | `wop*` (patterns, kick sets, springs, tilt, per-gun `wop_kickreturn`, `wop_alias`) | `wop_` (`kick_consts`, `debug`, ...) |
| `[camera]` | `cam_shake`, `camera_free` | `shake`, `free` |
| `[locomotion]` | `locomotion*`, `idle_active*`, `sway_*` | `locomotion_` (`jog`, `debug`, `enable`, `alias`) |
| `[inspect]` | `inspect*` | `inspect_` (`key`, `empty`, `hidehud` or `hide_hud`, `akimbo`) |
| `[last_shot]` | per-gun `empty_lastshot` | `default` = the global mode |
| `[empty_melee]` | `empty_melee_fix`, `interrupt_empty_melee`, `additive_melee_fade` | `fix`, `raise`, `additive_fade` |
| `[interrupts]` | `interrupt*` | `interrupt_` |
| `[segreload]` | `segreload*` | `segreload_` |
| `[slide]` | `slide_*` | `slide_` (`suit`, `dvars`, `gesture`, ...) |
| `[vmfov]` | `vmfov_*` | `vmfov_` (`ads`, `depth`, `max`, `debug`); `mode` = `vmfov` |
| `[ik]` | `ik*` | `ik_` (`notes`, `alias`, `debug`, `blend`, ...) |
| `[weapon:<name>]` | one weapon's settings (below) | |

* **Keys:** `key = value` (spaces around `=` are fine in a section). Inside a section the feature prefix may be left
  out; full key names are accepted in every section. A key that already belongs to a feature (`sway_parts` in
  `[locomotion]`, `additive_melee_fade` in `[empty_melee]`) is never prefixed.
* **Sections can repeat.** A second `[additives]` later in the file adds to the first. Lines keep their file order
  within a section, which matters for `wop_alias` (it copies a block written above it).
* **Unknown sections** are logged once (`unknown section(s): [name]`); their lines are read with the keys as written.
* **Comments:** `#` lines and blank lines are dropped. A detail line keeps a trailing `# ...` (the parsers ignore it
  where they did before); `[features]` values and guns lists drop it.
* **Line length:** lines are still read 255 characters at a time, and `<key>=<weapon>,+,...` still continues a long
  `ammohide_order=` / `ammohide_spend=` list. Split a long guns list over several `guns =` lines: they add up.
* **`[features]` is read at start.** Change it and restart; the live keys inside the sections stay live.

### `[features]`

| Feature | Values | On | Off |
|---|---|---|---|
| `additives` | on / off | layers as configured (`additive_enable` stays a live A/B fade) | no additive or slot layer |
| `ammo_hide` | on / off | | no ammohide, hide order, auto hide or round guard |
| `kick` | on / off | | no `wop*` block: no patterns, kick sets or springs |
| `kick_return` | on / off | `wop_kickreturn=1` | `wop_kickreturn=0` |
| `camera` | on / off | | no `cam_shake` / `camera_free`, global or per gun (live reloads keep it off) |
| `locomotion` | on / off | | no walk, jog, idle_active or sway |
| `inspect` | on / off | `inspect_enable=1` | `inspect_enable=0` |
| `last_shot` | auto / iw / hold | the default `empty_lastshot` mode | |
| `empty_melee` | on / off | `empty_melee_fix=1` | no empty-melee fix and no `interrupt_empty_melee` |
| `interrupts` | on / off | | `interrupt_enable=0` |
| `segreload` | on / off | | `segreload_enable=0` |
| `slide` | on / off | `slide_enable=1` | `slide_enable=0` |
| `vmfov` | off / mw / <deg> | the mode, as `vmfov=` | `vmfov=off` |
| `ik` | on / off | `ik_enable=1` | no IK lines at all (no hooks) |

A feature `[features]` doesn't name keeps the old keys' behaviour, so a file without `[features]` works as it always
did. Each `*_enable` key maps onto its feature: `cfg_migrate.py` folds them into `[features]`. The A/B switches that fade
a live feature (`additive_enable`, `locomotion_enable`, `idle_active_enable`, `sway_enable`) still work inside their
section, so `enable = 0` in `[additives]` still fades the layers without a restart.

At start the log has one line with what each feature ended up with, for example:

    weapon_tech: features (format v2, [features] applied): additives(93 guns) ammo_hide(29) kick(86) kick_return:on
    camera(1) locomotion(51, sway 30) inspect(101) last_shot:auto(+0) empty_melee:on interrupts(0) segreload(2) slide:off
    vmfov:off ik(1)

### Guns lists

The simple per-gun features take a list instead of one line per gun. `<gun>:<p1>:<p2>` passes parameters; `on` / `off`
as the first parameter sets the switch.

| Section | Key | Entry | Same as |
|---|---|---|---|
| `[inspect]` | `guns` | `<gun>[:off][:<seconds>]` | `inspect=<gun>,<1\|0>[,<seconds>]` |
| `[ik]` | `guns` | `<gun>[:off][:<l\|r\|lr>[:<noteless weight>[:<orient>]]]` | `ik=<gun>,<1\|0>[,...]` |
| `[segreload]` | `guns` | `<gun>:<end\|start\|off>` (a bare gun = end) | `segreload_empty=<gun>,<mode>` |
| `[last_shot]` | `guns` | `<gun>:<auto\|iw\|hold>` | `empty_lastshot=<gun>,<mode>` |
| `[ammo_hide]` | `auto_off` | `<gun>` | `ammohide_auto=<gun>,0` |
| `[additives]` | `take_jukes` | `<gun>` | `slots_take_jukes=<gun>` |

Per-gun data that doesn't fit a list (additive layers, WOP blocks, sway blocks, `wop_kickreturn` per gun, which has to
come before the gun's `wop_alias` lines) stays as detail lines in its section.

### `[weapon:<name>]`: the per-weapon form

One section per weapon, holding that weapon's settings across features. The keys are the `wt*` keys of the GDT design
(`gdt_schema/DESIGN_weapontech_gdt.md`), so a compiler that reads them from the weapon GDTs can write this form 1:1.
Precedence: a weapon's entry here replaces its entry in a feature section's guns list; `[features]` gates everything.
Any per-weapon cfg key also works here with the weapon left out (`wop = 1,0,2,...` is `wop=<name>,1,0,2,...`).

| Key | Becomes |
|---|---|
| `wtSource` | `wop_alias=<w>,<source>`, written first so the weapon's own lines override the copy |
| `wtFireTimeMs` | `wop_weapon=<w>,<ms>` |
| `wtKickPct` | `wop_kickpct=<w>,<v>` |
| `wtKick1` ... `wtKick24` | `wop_kick=<w>,<v>` (file order) |
| `wtSpringViewHip` / `ViewAds` / `GunHip` / `GunAds` | `wop_spring=<w>,0,0,<v>` / `0,1` / `1,0` / `1,1` |
| `wtTilt` | `wop_tilt=<w>,<v>` |
| `wtWopCurveHoldSlow` / `HoldFast` / `Kick` / `SnapDecay` / `Ads` / `AlwaysOn` | `wop_curve=<w>,0..5,<v>` |
| `wtWop1` ... `wtWop16` | `wop=<w>,<v>` (a `# label` may follow) |
| `wtKickReturn`, `wtKickMaintain`, `wtKickNoDampening` | `wop_kickreturn=<w>,<on>[,<maintain>[,<noDamp>]]` |
| `wtCamShakeAngles` / `Roll` / `Origin` / `PitchUp` | `cam_shake=<w>,<a>,<r>,<o>[,<pitchUp>]` (unset = -1: falls back) |
| `wtCameraFree` | `camera_free=<w>,<v>` |
| `wtSwayAdv` / `AdvGun` / `AdvFire` / `IdleMisc` / `Stance` / `AdsBob` | `sway_adv` / `sway_advgun` / ... `=<w>,<v>` |
| `wtSwayIdle1` / `wtSwayIdle2` | `sway_idle=<w>,1\|2,<v>` |
| `wtSwayGraphDeadzone` / `wtSwayGraphGun` | `sway_graph=<w>,0\|1,<v>` |
| `wtLocoWalkStrides`, `wtLocoWalkRate` | `locomotion=<w>,walk,bob,<n>[,<rate>]` |
| `wtLocoJogLeaf`, `Weight`, `Rate`, `Strides` | `locomotion=<w>,jog,<leaf>[,<w>[,<rate>[,<n>]]]` |
| `wtIdleActiveAnim`, `Leaf` (194), `Weight`, `Rate` | `idle_active=<w>,<anim>,<leaf>[,<w>[,<rate>]]` |
| `wtAdditive{Empty,Recoil,Bullet}{Anim,Root,Weight}`, `wtAdditiveRecoilRate`, `wtAdditiveBulletMag` | `additive=<w>,<kind>,<root>,<anim>[,<weight>[,<rate\|mag>]]` (roots 195 / 195 / 193) |
| `wtAdditiveSlot1` ... | `additive=<w>,<v>`: a purpose-slot line (`bullet,slot:bullets,<anim>,1,30,side:left`) |
| `wtAmmoHide` / `Order` / `Spend` / `Reverse` | `ammohide` / `ammohide_order` / `_spend` / `_reverse` `=<w>,<v>` |
| `wtAmmoHideAuto` | `ammohide_auto=<w>,<1\|0>` |
| `wtIk`, `wtIkHands`, `wtIkNotelessWeight`, `wtIkOrient` | `ik=<w>,<on>[,<hands>[,<w>[,<orient>]]]` |
| `wtSegReloadEmpty` | `segreload_empty=<w>,<v>` |
| `wtInspect`, `wtInspectTime` | `inspect=<w>,<on>[,<seconds>]` |
| `wtInterrupt1` ... `wtInterrupt8` | `interrupt=<w>,<v>` |
| `wtEmptyLastShot` | `empty_lastshot=<w>,<v>` |
| `wtRecoil`, `wtSway`, `wtLoco`, `wtInterrupt` = off | that group's keys in this section are left out |

Modes take `on` / `off` (or `1` / `0`). An unknown `wt*` key is logged and ignored.

### Generated blocks

A tool that writes into the cfg owns a fenced block and rewrites it whole:

```ini
# ==== BEGIN generated:<tool> ====
[additives]
additive=...
# ==== END generated:<tool> ====
```

The block opens its own section, and the section in force before `BEGIN` comes back at `END`, so a block can sit
anywhere in the file. Edit by hand outside the fences only. The generators that write this form: `ammohide_order`
(`tools/ammohide_order.py`, `[ammo_hide]`), and the Karelia pipeline's `build_bullet_empty_additives`,
`build_bullet_empty_additives.slots` and `build_bo7_fill` (`[additives]`). They find and replace their old
`# ==== BEGIN <title> ====` fences too.

### Checking a cfg: `cfg_dump`

`cfg_dump = 1` (in `[general]`, or `cfg_dump=1` flat) writes two files next to `weapon_tech.log`:
`weapon_tech.cfgdump.txt` holds an FNV hash of every parsed table and switch after the `[features]` gates, with a line
per entry, and `weapon_tech.cfgnorm.txt` holds the flat lines the parsers read. Two cfgs with the same `state hash`
configure the DLL the same way. `src/wt_cfgdump.cpp` builds the same check as a console tool:

    cl /nologo /O2 /MT /EHsc /std:c++17 wt_cfgdump.cpp /Fe:wt_cfgdump.exe /link kernel32.lib
    wt_cfgdump <weapon_tech.cfg> <state out> [<flat lines out>]

### Migrating a flat cfg

    python tools/cfg_migrate.py <weapon_tech.cfg> --in-place --check [--dump-tool <wt_cfgdump.exe>]

It writes `[features]` from the old switches, moves every line into its section in file order with the comments above
it, collapses the simple per-gun lines into guns lists where every line round-trips exactly, keeps generator blocks
whole (old fences become `generated:` fences), and leaves anything a section would read differently above
`[features]`. `--check` parses the old and new file with `wt_cfgdump.exe` and only replaces the file (backup
`<cfg>.pre_cfgv2.bak`) when the parsed state is identical. `--check-only <new>` compares two existing files.

## Supported executables

| Build | SizeOfImage | TimeDateStamp | Notes |
|---|---|---|---|
| BO3 Enhanced (CL 20659811) | `0x1A53F000` | `0x67363F2A` | no Arxan |
| Stock retail (Steam build 24784313, Aug 2026) | `0x1D75BC00` | `0x6A7B6355` | Arxan is neutralised first (below) |

Any other image installs nothing and Arxan is not touched; the log names the image it found. The same cfg works on both
exes and no key is exe-specific. Every patch site is verified against the exe's code bytes before it is written, so a
mismatch fails closed with a log line.

## Where the config comes from

The DLL is started once per process by calling its exported `init` (from the map's LUI Lua through
`package.loadlib(..., "init")`). The first source found wins, and the log names it.

1. **Loose file**: `weapon_tech.cfg` next to `weapon_tech.dll`. The **live** keys (column "Live" below) are re-read when
   the file's modification time changes. Two conditions apply:
   * The general anim hook has to be installed. It installs if the cfg has any `additive=`, `ammohide=`, `wop*`,
     `locomotion=`, `idle_active=`, `sway_*` or `ik=` line. If none of those is present, the log says
     `live tuning OFF after all: the anim hook isn't in`: segreload, inspect and interrupt lines are read once and are
     never re-read. (`slide_*` and `vmfov*` have their own live paths, see section 10.)
   * `perf_cfgwatch=1` (the default) watches the file from a background thread every 250 ms. The parse itself is applied
     on the game thread. With `perf_cfgwatch=0`, the file is checked on the game thread every 120 frames.
2. **Rawfile** `weapon_tech/weapon_tech.cfg` baked into the map (the `weapontech` linker feature). It is read once and
   nothing is live.
3. **Neither**: nothing is installed, and the log says so.

The log is `weapon_tech.log`, next to the exe. After init the game thread only queues lines and a background thread
writes them every 100 ms.

## Line syntax

* `key=value`, one per line (inside a `[section]`, `key = value` also works: see above). `\r\n` and `\n` both work. Lines can be up to 255 characters; anything longer is cut off
  and the rest is read as a new line. A few keys have a continuation form for long lists (`ammohide_order=` and
  `ammohide_spend=`: `<weapon>,+,...` continues that weapon's earlier line of the same key).
* **Unknown keys are ignored**, but not silently: each distinct unknown key is logged once at load as
  `unknown key '<key>' ... (ignored; typo?)` and counted in the load summary line. Two prefixes are claimed by their
  feature, so a typo under them is logged as a *bad line* on every occurrence instead: `sway_*`, `slide_*` (and any key
  starting with `vmfov`, `inspect`, `interrupt`, `segreload`).
* Comments: a line whose first non-blank character is `#` is ignored. A trailing `# ...` is stripped only by the parsers
  that split fields themselves (locomotion, sway, ik, interrupt, alias, segreload, `ammohide_order=`/`ammohide_spend=`).
  On other keys a trailing comment becomes part of the last field or fails the parse, notably `additive=` and the
  `wop*` family (`wop_alias=` with no fire time reads the comment as part of the source name). **Put comments on their
  own line.**
* Weapon names are the variant names (`variant+0`, for example `ar_mike16_kar_zm`), case-sensitive. `ik=`, `ik_alias=`
  and `ammohide_order/spend/reverse` match case-insensitively.

## Routing lines (ignored here)

`route=`, `folder=`, `alias=`, `extend=`, `preload=`, `persist=` and `stencil=` belong to a different config. weapon_tech
logs the first one it sees and ignores them all.

## Legend

* **Live**: re-read on a loose-file edit. "partial" means only the listed fields change.
* **Installs**: what gets patched when the key is present at game start.
* **Removed line**: what happens on a live reload when the line is deleted.

---

## 1. Additive layers, ammohide, empty melee: `[additives]`, `[ammo_hide]`, `[last_shot]`, `[empty_melee]`

| Key | Format | Default | Live | Notes |
|---|---|---|---|---|
| `additive=` | `<weapon>,<empty\|recoil\|bullet>,<193\|195>,<xanim>[,<weight>[,<magSize (bullet) \| rate (recoil)>]]` | none | partial: weight, rate and magSize of a line that already exists (same weapon, kind and root) | Writes `<xanim>` into slot root+1. New, removed or changed xanim/root needs a restart. Up to 192 lines (slot lines count). An unknown kind becomes `empty`. A `bullet` line needs a mag size above 0. |
| `additive=` (purpose slot) | `<weapon>,<kind>,slot:<purpose>,<xanim>[,<weight>[,<mag \| rate>]][,side:<right\|left>]` | none | partial: weight, mag, rate | bo3_slots.h. Purposes: `bullets` (root 192 / leaf 117), `empty` (190 / 118), `recoil_ads` (189 / 119), `idle` / `recoil` (the old 193 / 195). One line per weapon, purpose and side. **`side:`** is optional and defaults to `right`. On a dual-wield (akimbo) gun `right` follows the right gun's clip, state and node and `left` the left gun's. `side:left` works on `empty` (its own root 186 / leaf 124) and `bullets` (185 / 108), so each gun fades on its own; `slot:empty_left` / `slot:bullets_left` mean the same. The left xanim must key the LEFT gun's bones (the akimbo rig renames them: `j_slide1`, `tag_pistol_offset1`, `tag_weapon_le`, ...); `tools/gen_left_additives.py` makes one from the right gun's. A `side:left` line on a gun that isn't dual wield stays off (logged). `side:both` is refused (one xanim can't follow two clips). There is no left recoil layer (no free root). |
| `slots_take_jukes=` | `<weapon>\|all` (repeatable) | none | no | Lets the slot layers use juke roots 185 / 186 / 189 / 190 although the gun names all four juke anims (its jukes then don't play). |
| `slots_debug=` | `0\|1` | 0 | yes | Bind / refuse / state lines, plus a 1 s trace per active slot line (side, clip, state, node, target, weight, time). |
| `slots_dump=` | `0\|1` | 0 | yes | Logs every szXAnims slot of each weapon a slot line binds, once. |
| `empty_lastshot=` | `<auto\|iw\|hold>` (default for all guns) or `<weapon>,<auto\|iw\|hold>` (repeatable, up to 64) | auto | yes; a removed line is auto again | When the empty layer (legacy `empty` 193/195 lines and `slot:empty`, both akimbo sides) comes on after the last round. **hold**: off while the last-shot anim plays (node 10 hip, 106 ADS, 167 left gun), because a real `fire_last` ends in the locked pose; when that anim ends the layer takes `1 - <last-shot node weight>` each frame, so the clip's lock and the layer never add up to two locks and nothing pops. **iw**: MW2019's rule: on as soon as the clip is 0, with a 0.05 s blend, whatever anim plays. Use it for a gun whose last-shot anim is only its fire anim. **auto**: decides per weapon and per hip / ADS / left gun from the WeaponDef's anim names. It uses hold when the last-shot slot is set and differs from the fire slot, and iw when it is blank or the same name. Each decision is logged once (`empty_lastshot: variant ... node ...`). If the names can't be read it falls back to hold. Reloads and melee still turn the layer off. |
| `additive_enable=` | `0\|1` | 1 | yes; a removed line counts as 1 | A/B switch: 0 fades the layers out. |
| `additive_debug=names` | exact text | off | no | Patches the slot names and sets no weights. Also skips **every** per-frame feature (wop, locomotion, IK, ammohide), not only the additive layers. |
| `ammohide=` | `<weapon>,<joint1>,<joint2>,...` (up to 24 joints) | none | no | Joint k is hidden while the clip holds fewer than k rounds. Up to 16 lines. |
| `ammohide_auto=` | `0\|1`, or `<weapon>,0` | 1 | no | Spent-round hide for every gun with a bullet line (`additive=...,bullet,...` or `slot:bullets`) and no `ammohide=` line: DObj hide bits on the round joints (plus each round's first non-round child, its link). `<weapon>,0` opts one gun out (up to 32). Round joints are recognised **by name**: `bullet`, `round`, `shell`, `j_b_<n>`, and `j_ammo*` (BO7 / IW9 `j_ammo_01`..; left gun `j_ammo_01_le`, `j_ammo_011`). Names containing `follower`, `linkempty` or `mag` are not rounds, and `tag_ammo_*` attach tags are not rounds. The joints come from the bullets anim's round-named parts, or from the `ammohide_order` / `ammohide_spend` list. |
| `ammohide_order=` | `<weapon>,<joint>,<joint>,...` | none | no | Spend order, first spent first: the last `<clip>` joints of the list stay shown. Generated by `tools/ammohide_order.py` for stack mags whose bullets anim parks every round on the same frame (nearest the follower / pusher at rest first). Joints need not be in the compiled anim (the linker drops parts its skeleton lacks). Up to 48 lines, 128 joints per weapon; use the `+` continuation for long lists. |
| `ammohide_spend=` | `<weapon>,<joint>:<clip>,...` | none | no | Per-joint spend clip: the joint is hidden while the clip holds `<clip>` rounds or fewer; unlisted joints are never hidden. Generated by `tools/ammohide_order.py` when the bullets anim brings rounds to their final pose at different frames (belts, the P90 stack): clip = mag + 1 - the export frame from which the round stays at its final pose. Same limits and `+` continuation as `ammohide_order=`. |
| `ammohide_reverse=` | `<weapon>` | none | no | Reverses the built-in order (nearest the follower / pusher, else j_bolt / tag_flash, first). Last resort when neither generated line fits. |
| `ammohide_reload=` | `0\|1` | 1 | no | Through a reload the mag shows the round count the gramien notetracks on the reload anim give (`gramien_hide_full_magazine` = keep the count the old mag left with, `gramien_show_full_magazine` = full, `gramien_watch_ammo` = live); a reload anim without them keeps the old count until the engine adds the ammo (the clip goes up), never full at the start. Applies to bullet layers (they stay on through the reload) and both hide paths. 0 = layers off and every round shown during reloads. |
| `ammohide_guard=` | `0\|1` | 1 | no | Spent-round guard (bo3_roundguard.h). IW / BO7 bullets anims *park* a spent round far out of the magazine (tens to thousands of units) instead of hiding it; BO3 draws whatever is not hidden, so a parked round floats in front of the camera wherever the name-based hide above does not cover it (unknown joint names, a hide order that differs from the park order, a frame where the hide bits are not ours). After each viewmodel skeleton build, every round-like bone is measured in its nearest non-round ancestor's frame against the XModel base pose; one further than `ammohide_park` from its base position is parked, and it and every bone below it are hidden through the DObj hide bits. The bits are rebuilt each time from the engine's own, the anim hook's and the parked set, so nothing sticks. Material-independent. It shares the DObjCalcSkel hook with IK, so with the guard on (the default) that hook is installed whenever the anim hook is, even with no `ik=` line. 0 turns it off. |
| `ammohide_park=` | float > 0, units | 12 | no | Distance from the base pose beyond which a round counts as parked. |
| `ammohide_debug=` | `0\|1` | 0 | no | Logs the parked set when it changes (`floatround:` lines). Also installs the skeleton hook even with `ammohide_guard=0`. |
| `belt_dump=` | `0\|1` | 0 | no | Debug: logs the bullet bones' model-space matrices per clip count for bullet layers (to design `ammohide_spend=`). |
| `empty_melee_fix=1` | exact text `=1` | off | no | Three code patches: melee `jz` to `jmp`, and the `BG_ClipEmpty` calls in the empty raise and empty drop replaced by `xor eax,eax`. `empty_melee_fix=0` is accepted (it is the default spelled out). |
| `additive_melee_fade=` | `0\|1` | 0 | no | Fades the held gun's own empty / bullet / recoil layers (`additive=` and slot lines) to 0 during its melee states, back in after. Only matters for a gun with its own melee; IW8 keeps them on. Always on, with no key: while the viewmodel shows another weapon (a knife melee, an offhand), the held gun's additive / slot / ammohide / locomotion / slide / IK layers are not written into that weapon's tree. |

## 2. IW8 weapon offsets, kick and camera: `[kick]`, `[camera]` (bo3_wop.h, bo3_iw8kick.h)

| Key | Format | Default | Live | Notes |
|---|---|---|---|---|
| `wop_weapon=` | `<weapon>,<fire ms>` | 100 ms | no | |
| `wop_curve=` | `<weapon>,<curve 0-5>,blend,decay,shotDecayFrac,hold,adsBegin,adsEnd,interpIn,interpOut` | zeros | no | |
| `wop=` | `<weapon>,curve,pattern,target,freq,blendTime,mx,my,mz,hipScale,rotOffset,faScale,faBullets,faDecay,kickIndex` (14 numeric fields) | none | no | Up to 16 per weapon and 128 weapons. curve 0-5, pattern 0-3, target 0-3, kickIndex <= 7. |
| `wop_kick=` | `<weapon>,ads,gun,bullet,dir,dev,sMin,sMax,pitchScale` | none | no | Up to 24 per weapon. Any `wop_kick` installs the fire hook. |
| `wop_kickpct=` | `<weapon>,startBullets,endBullets,8 floats` | 1.0 | no | |
| `wop_spring=` | `<weapon>,<gun 0\|1>,<ads 0\|1>,accel,retAccelScale,retSpeedCurveScale,maxPitch,maxYaw` | none | no | Replaces BO3's kick integrator for that weapon. |
| `wop_tilt=` | `<weapon>,8 floats` | 0 | no | |
| `wop_alias=` | `<weapon>,<source>[,<fire ms>]` | none | the sway and IK parts are live; the wop copy isn't | Copies the source's wop block (the source has to come earlier). Every other subsystem (locomotion, sway, ik, segreload, inspect, interrupt) also treats it as an alias. |
| `wop_kickreturn=` (global) | `0\|1` | **0** | no | Installs the kick-return capture stub. Needs at least one `wop_kick`. The capture is hooked at both kickAngles copies (the second is the one the zombies game mode executes; log: `kick return: hooked (... and at the twin copy)`). |
| `wop_kickreturn=` (per weapon) | `<weapon>,<0\|1>[,maintain 0..1[,noDampening 0\|1]]` | on, 0, 0 | no | Put it before that weapon's `wop_alias` lines. |
| `wop_kick_consts=` | `iw8\|legacy` | iw8 | yes; a removed line counts as iw8 | `iw8` = the MW2019 integrator constants from the PDB build; `legacy` = the earlier estimates. |
| `wop_debug=` | `<channel 0-4>,x,y,z` | 0 | yes; a removed line counts as 0 | |
| `wop_debug_yaw=` | `<deg>` | 0 | **no**, and not reset by a reload | |
| `cam_shake=` (global) | `<pitch/yaw>,<roll>,<origin>[,<pitchUp>]` | 1,1,1,0 | yes; reset to the default | A value starting with a letter is read as a weapon name. Names that start with a digit or `_` are taken as global. |
| `cam_shake=` (per weapon) | `<weapon>,py,roll,origin[,pitchUp]` | unset (uses the global) | yes | Falls back to the wop_alias source, then the global. |
| `camera_free=` | `<0..1>` or `<weapon>,<0..1>` | 0 | yes | 1 = IW8 camera anims: `tag_camera` moves only the camera and the viewmodel stays put. Only acts while a WOP weapon is held and `tag_camera` moved the view. |

## 3. Locomotion and idle active: `[locomotion]` (bo3_locomotion.h)

| Key | Format | Default | Live | Notes |
|---|---|---|---|---|
| `locomotion=` | `<weapon>,walk,bob,<strides 1-64>[,<rate>]` | none | no | The walk anim locked to the bob cycle. |
| `locomotion=` | `<weapon>,jog,<leaf 185-192>[,weight 0-2[,rate[,strides]]]` | none | no | A visual-only jog loop in a juke leaf (root 184). |
| `locomotion_alias=` | `<weapon>,<source>` | none | no | |
| `idle_active=` | `<weapon>,<xanim\|*>,<leaf 194\|196\|185-192>[,weight 0-2[,rate]]` | none | partial: weight and rate | IW8's hip idle-active additive. `*` keeps the slot's own anim. Refused when it shares a root with the same weapon's `additive=` or `jog`. |
| `locomotion_jog=` | 7 floats: start,keep,startAng,keepAng,blend,adsBlend,fireBlend | 0.6,0.5,30,40,0.3,0.15,0.15 | yes; **a removed line keeps the last value** | Speed fractions and angles at which the jog starts / stays on. IW8 uses 75 degrees; 30 / 40 keeps W+A and W+D on the walk. |
| `idle_active_fade=` | `iw8\|linear[,<blend s>]` | iw8, 0.2 | yes; a removed line keeps the last value | `iw8`: weight 1 at the hip, 0 from any ADS, linear blend. `linear`: weight x (1 - ADS fraction). |
| `locomotion_debug=` | `0\|1` | 0 | yes; a removed line keeps the last value | |
| `locomotion_enable=` | `0\|1` | 1 | yes; a removed line counts as 1 | |
| `idle_active_enable=` | `0\|1` | 1 | yes; a removed line counts as 1 | |

## 4. MW19 sway: `[locomotion]` (bo3_sway.h)

Every `sway_*` line is **live** if the cfg had at least one per-weapon sway line at game start (that is when the hooks
install). The whole sway table is rebuilt on each reload, so a removed line goes back to the defaults below.

| Key | Format | Default |
|---|---|---|
| `sway_adv=` | `<w>,` 15 floats (torso part; masses > 0) | off |
| `sway_advgun=` | `<w>,` 15 floats (gun part; masses > 0) | off |
| `sway_advfire=` | `<w>,fireMs,startMs,finishMs,dzScale,toGun` | 300,200,200,0.5,0.5 |
| `sway_graph=` | `<w\|*>,<0\|1>,x0,y0,...` (2-16 knots) | linear |
| `sway_idle=` | `<w>,<1\|2>,` 16 floats | off |
| `sway_idlemisc=` | `<w>,crouch,prone,gasp` | 1,1,0 |
| `sway_stance=` | `<w>,` 16 floats | off |
| `sway_adsbob=` | `<w>,pitch,yaw,tiltP,tiltY,tiltR,tiltOffset,crouch` | off |
| `sway_alias=` | `<w>,<source>` | none |
| `sway_enable=` | `0\|1` | 1 |
| `sway_parts=` | 5 flags: adv,idle,stance,adsbob,camera idle | 1,1,1,1,1 |
| `sway_scale=` | 4 floats: torso,gun,idle,adsbob | 1,1,1,1 |
| `sway_smoothing=` | `0\|1[,clamp deg]` | 0 |
| `sway_advparts=` | 3 flags | 1,1,1 |
| `sway_gunsign=` | `1\|-1` | 1 |
| `sway_gunbobmax=` | float >= 0 | 1 |
| `sway_gunbobtrans=` | in,out seconds | 0.25,0.25 |
| `sway_boblag=` | float | 0.25 |
| `sway_idlefwd=` | timeScale,magScale | 0.001,0.01 |
| `sway_idleview=` | pitch,yaw | 0.001,0.0007 |
| `sway_substep=` | Hz, 0-2000 | 240 |
| `sway_clock=` | `game\|qpc` | game |
| `sway_bobfilter=` | Hz, 0-30 | 0 |
| `sway_debug=` | `0\|1` | 0 |

`sway_camlead` and the `frameId` / `camLead` globals mentioned in bo3_sway.h have no cfg key; they are always on.

## 5. Hand IK: `[ik]` (bo3_ik.h)

| Key | Format | Default | Live | Notes |
|---|---|---|---|---|
| `ik=` | `<weapon>,<1\|0>[,<l\|r\|lr>[,notelessWeight[,orient]]]` | none | yes, but **the hooks install only if an `ik=` line exists at start** (or the round guard is on, section 1) | The two DObjCalcSkel call sites are redirected whenever an `ik=` line exists or the guard is on, even with `ik_enable=0`. |
| `ik_enable=` | `0\|1` | **0** | yes; a removed line counts as 0 | Without it, IK only measures (with `ik_debug=1`). |
| `ik_alias=` | `<weapon>,<source>` | none | yes | |
| `ik_notes=` | `<xanim>,<l\|r>,<is\|ie\|os\|oe>:<t>,...` | none | yes | Supplies IK in/out markers for an anim that has no `ik_in` / `ik_out` notetracks (is / ie = in start / end, os / oe = out start / end, `<t>` in seconds). |
| `ik_blend=` | seconds, 0-2 | 0 | yes | |
| `ik_always=` | `0\|1` | 0 | yes | |
| `ik_debug=` | `0\|1` | 0 | yes | |

## 6. Segmented-reload empty variants: `[segreload]` (bo3_segreload.h)

**Not live.**

| Key | Format | Default | Notes |
|---|---|---|---|
| `segreload_empty=` | `<weapon>,<end\|mw\|mw19\|mw2019\|start\|mwii\|mw2\|mw22\|off\|0>` | none | Up to 64. With any line present, 8 BG call sites are redirected (they run on the server and in client prediction). |
| `segreload_enable=` | `0\|1` | 1 | 0 means not installed. |

## 7. Inspect: `[inspect]` (bo3_inspect.h, bo3_inspect_hud.h)

**Not live.** Nothing installs unless `inspect_enable=1` **and** at least one `inspect=` line are both present.

| Key | Format | Default | Notes |
|---|---|---|---|
| `inspect_enable=` | `0\|1` | **0** | Master switch. |
| `inspect=` | `<weapon>,<1\|0>[,<seconds>]` | none | `seconds` overrides the length (default: the GDT's `lowReadyLoopTime`, else the xanim's own length). A `wop_alias` gives a PaP its source's line unless it has its own. |
| `inspect_key=` | letter or digit, F1-F24, MOUSE3/4/5, or `0xNN` | I | |
| `inspect_empty=` | `in\|out\|off\|0` (anything else means `in`) | in | Empty inspect (MWII and newer): with an empty clip, play the GDT's `lowReadyInAnim` (`in`) or `lowReadyOutAnim` (`out`) instead of the loop, timed by its `lowReadyIn/OutTime`. A blank or idle slot falls back to the normal inspect. `off` and `0` disable it. |
| `inspect_hidehud=` | `0\|1` | 0 | Also fades the whole HUD out while inspecting. The crosshair always hides. Both are done by the map's LUI from the UI models below. |
| `inspect_akimbo=` | `0\|1` | 1 | Dual-wield weapons: 1 inspects them (the inspect xanim animates both arms and guns; the left hand's idle node is faded out for the duration, and the left hand must be ready to start). 0 means they do not inspect at all. Single-wield weapons never take this path. If the akimbo checks fail at install the log says so and akimbo weapons do not inspect. |
| `inspect_debug_empty=` | `0\|1` | 0 | Debug: treat the clip as empty, to preview the empty inspects with a full mag. |

UI models published: `hudItems.weaponTech.inspecting` and `hudItems.weaponTech.inspectHideHud`.

## 8. Interrupts: `[interrupts]` (bo3_interrupt.h)

**Not live.**

| Key | Format | Default | Notes |
|---|---|---|---|
| `interrupt=` | `<weapon>,<states>,<source chain>[,<actions>]` | none | With any line present, both PM_Weapon calls are redirected (server and prediction). States: `raise first_raise quick_raise empty_raise raises reload reload_empty reload_start reload_loop reload_end reloads rechamber all`. Source chain (joined by `\|`, first that resolves wins): `notetrack`, `interruptible`, `state_timer_end`, `fingers`, `statetimer`, a fraction (`0.85` / `85%`) or ms (`850` / `850ms`). Actions (joined by `+` or `,`): `fire ads sprint melee reload switch all none`. Later lines for the same weapon and state replace earlier ones. The header lists the per-state defaults. |
| `interrupt_enable=` | `0\|1` | 1 | |
| `interrupt_debug=` | `0\|1` | 0 | |
| `interrupt_trace=` | `0\|1` | 0 | Debug: logs hand 0's BG weapon state / anim changes (`itrace:` lines, server and prediction ps). On its own it installs the two PM_Weapon call redirects, even with no `interrupt=` lines. |
| `interrupt_empty_melee=` | `0\|1` | 0 | Rapid melee with an empty clip, as with ammo. One code patch: the `BG_ClipEmpty` call in PM_Weapon_CheckForReload (Enhanced +0x27B3A35) becomes `xor eax,eax`, so the post-melee quick raise (state 32, which melee can interrupt) is no longer turned into an empty raise (state 1, which blocks melee) when the clip is empty. The patch is in BG, so server and prediction match. Independent of `interrupt=` lines and `interrupt_enable`. |

## 9. Perf and diagnostics: `[general]` (bo3_perf.h, bo3_additive.h)

| Key | Format | Default | Live | Notes |
|---|---|---|---|---|
| `cfg_dump=` | `0\|1` | 0 | no | Writes `weapon_tech.cfgdump.txt` (parsed state hashes) and `weapon_tech.cfgnorm.txt` (the flat lines read) next to the log; see [Checking a cfg](#checking-a-cfg-cfg_dump). |
| `perf_eventhooks=` | `0\|1` | 1 | **no** (a live edit logs "live" but does nothing) | 1 redirects the registration call and both tree-build calls whenever the anim hook is in. |
| `perf_cfgwatch=` | `0\|1` | 1 | **no** (same) | 1 starts the watcher thread. |
| `perf_pollms=` | 100-60000 | 2000 | yes; a removed line keeps the last value | Fallback poll interval for the variant table. |
| `perf_hotlog=` | `0\|1` | 0 | yes; a removed line keeps the last value | |
| `perf_timing=` | `0\|1` | 0 | yes; a removed line counts as 0 | A timing report every 10 s. |

No key: every time-driven layer (additive / slot recoil scrubs, idle_active and locomotion, sway, the slide gesture, the
WOP patterns = cam shake and gun kick, hand IK) runs on a game clock: the wall clock, held while the client time has not
moved for 50 ms, i.e. while the solo pause menu has the game stopped. The layers hold their pose and carry on from it
when the game runs again. Log: `gameclock: paused ...` / `gameclock: running again ...`. Inspect already runs on client
time.

## 10. Slide and viewmodel FOV: `[slide]`, `[vmfov]` (bo3_slide.h, bo3_vmfov.h)

Both are **opt-in**: with no `slide_enable=1` / `vmfov=` line nothing is installed and nothing changes.

### Slide (MW2019 slide)

IW8 slide movement in BG (server Pmove and client prediction run the same hooks, from the ps, the usercmd and the cfg
only, so they match) plus the MW slide gesture as an additive viewmodel layer. `slide_enable` and the hook set are read
at start; everything else is **live** when the cfg is the loose file (the slide code has its own watcher, 0.5 s).
Unknown `slide_*` keys are logged as bad lines.

| Key | Format | Default | Notes |
|---|---|---|---|
| `slide_enable=` | `0\|1` | **0** | Master switch, start only. |
| `slide_suit=` | `<preset>` or `<key>=<v>[,<key>=<v>...]` | `iw8_defaultsuit_mp` | Presets: `iw8_defaultsuit_mp` (every MP / operator suit) or `iw8_defaultsuit` (SP: inAcceleration 2.8, frictionScaleBlocked 0). Overrides use the SuitDef names without the `slide_` prefix: `inTimeMs inMaxSpeedScale inAcceleration max_time_ms frictionScaleNormal frictionScaleDownhill frictionScaleUphill frictionScaleBlocked outTimeMs outFrictionScaleStart outFrictionScaleFinish inAirTimeMs inAirFrictionScaleStart inAirFrictionScaleFinish strafe_speed_scale jump_speed_scale sprintDelayMs viewBlendInTimeMs viewBlendOutTimeMs player_sprintSpeedScale player_crouchSpeedScale`. Repeatable (later keys override). |
| `slide_dvars=` | `<key>=<v>[,...]` | PDB defaults | The exe-only IW8 dvars: `min_required_velocity` 140, `min_continue_velocity` 50, `subsequentSlideTime` 1000, `subsequentSlideScale` 0.15, `lateJumpGraceMs` 300, `cameraPitchOffset` 15, `cameraRotateTimeMs` 1100, `cameraAlignmentEaseMode` -1 (-1 = BO3's own ease, 2), `view_angles` `-3 0 -1` (three numbers, space separated), `viewInterpType` 1, `stopspeed` 100, `friction` 5.5. |
| `slide_ads_ends=` | `0\|1` | 1 | ADS ends the slide. |
| `slide_sprint_lock=` | `0\|1` | 0 | No sprint for `sprintDelayMs` after a slide. |
| `slide_camera=` | `0\|1` | 0 | IW8 camera: pitch eased to `cameraPitchOffset` over `cameraRotateTimeMs` (0 = BO3's 10 degrees / 500 ms). |
| `slide_view=` | `0\|1` | 0 | IW8 view angle offsets blended `viewBlendIn/Out` (0 = BO3's -8 / -7 and roll bounce). |
| `slide_snap_round=` | `0\|1` | 1 | While sliding, BO3's per-step velocity truncation becomes round-to-nearest. 0 keeps BO3's truncation, which drains roughly 150-300 u/s^2 at high frame rates. |
| `slide_gesture=` | `0\|1` | 1 | The viewmodel gesture layer (only with `slide_enable=1`). The gesture xanims (in / loop / out; frame 0 = the gun's idle as the additive reference) sit in the juke slots under additive root 184. A gun takes part when those leaves hold real anims. |
| `slide_gesture_nodes=` | `in,loop,out` | `187,188,191` | Leaves under root 184. |
| `slide_gesture_blend=` | `fadeIn,fadeOut,crossfade` (s) | `0.1,0.15,0.12` | Root fade in, root fade out, leaf crossfade. |
| `slide_gesture_weight=` | float | 1 | Root weight. |
| `slide_gesture_fps=` | float | 30 | The gesture xanims' frame rate (frame count = length x fps). |
| `slide_gesture_ref=` | `0\|1` | 1 | Frame 0 is the additive reference (played frames start at 1). |
| `slide_gesture_off=` | `<weapon>` (repeatable, up to 32) | none | No gesture layer (and no native-anim swap) for that weapon. |
| `slide_native_anims=` | `swap\|keep` | swap | `swap`: for guns with the layer, slots named `*slide_in` / `slide_loop` / `slide_out` / `slide_air_in` / `slide_in_air` are pointed at the gun's idle name, so BO3's own slide anims do not play under the gesture. Takes effect from the next tree build (the next raise). |
| `slide_debug=` | `0\|1\|2\|3` | 0 | 1: start / end / jump lines. 2: also a per-frame speed trace while sliding and for 600 ms after. 3: also velocity probes around PM_Accelerate / PM_StepSlideMove and between frames. |

The gesture runs from the viewmodel anim hook and stands down for reload, raise, drop, melee and offhand; ADS fades it.

### Viewmodel FOV pin (`vmfov*`)

BO3 draws the gun with the world projection and has no viewmodel FOV dvar. The pin is a placement compensation: the gun
is moved along the view axis so that at a reference depth it has the on-screen size it would have at the pinned FOV:
`shift = (tan(vmfov/2) / tan(worldFov/2) - 1) * vmfov_depth`, clamped to +-`vmfov_max` (negative = toward the eye). A
pure view-axis move keeps the sights on the crosshair at full ADS. It is exact at `vmfov_depth` only. BO3's own hip pull
(`2 * clamp((cg_fov - 65) * 0.05, 0, 1) * (1 - adsFrac)` units) is taken out, so the result replaces it. The world FOV
used is the one the frame is drawn with, latched at hip, so the ADS zoom never feeds the shift.

All `vmfov*` lines are live while the game runs, **but the hook is only installed if vmfov is on at load**; asking for it
live when it was off at load logs `restart the game`.

| Key | Format | Default | Notes |
|---|---|---|---|
| `vmfov=` | `off\|mw\|<degrees>` | off | `<degrees>`: pin the gun at this FOV in `cg_fov`'s convention (horizontal degrees at 4:3; 65 = BO3's and MW2019's base FOV, which the viewmodels are authored for). `mw`: MW2019's default: the gun at the world FOV with IW8's hip pull, `-6 * clamp((worldFov - 70) / 15, 0, 1) * (1 - adsFrac)`, in place of BO3's. |
| `vmfov_ads=` | `fade\|hold` | fade | `fade`: the shift fades out with the ADS fraction, so at full ADS the gun is the stock one. `hold`: the hip shift is kept through ADS. |
| `vmfov_depth=` | units | 20 | Reference depth for `<degrees>`. |
| `vmfov_max=` | units | 6 | Largest shift either way (MW2019's largest pull). |
| `vmfov_debug=` | `0\|1` | 0 | 1 Hz log line: world FOV (now / hip), ADS fraction, shift. |

---

## What installs, by configuration

| Present in the cfg at start | Patched |
|---|---|
| nothing (no cfg) | nothing |
| a cfg with only unknown or comment lines | nothing; the exe build is identified and the log writer thread starts |
| any of additive / ammohide / wop / locomotion / idle_active / sway / ik | the anim hook (a 15-byte jmp at Enhanced +0x4F3860; retail has its own site), the registration and tree-build call redirects (`perf_eventhooks`), the watcher thread (`perf_cfgwatch`), and the DObjCalcSkel call redirects for the round guard (`ammohide_guard`, on by default) |
| wop lines or sway lines | also the gun-placement, view-axis and, with kicks or sway, fire-recoil call redirects |
| `wop_kickreturn=1` plus kicks | also the kick capture call redirects |
| `ik=` | also the two DObjCalcSkel call redirects (shared with the round guard) |
| `empty_melee_fix=1` | three byte patches |
| `segreload_empty=` (and `segreload_enable` not 0) | 8 BG call redirects |
| `inspect_enable=1` plus `inspect=` | a 17-byte jmp at Enhanced +0x4F384A |
| `interrupt=` (and `interrupt_enable` not 0) | 2 PM_Weapon call redirects |
| `interrupt_empty_melee=1` | one 5-byte patch (Enhanced +0x27B3A35) |
| `slide_enable=1` | the PM_Slide / PM_WalkMove call-site redirects listed in bo3_slide.h, plus the gesture layer inside the anim hook |
| `vmfov=` not `off` | one call redirect on CG_CalculateWeaponPosition, chained in front of the gun hook when that is installed |

## Retail exe

The same cfg works on both exes. What differs on retail:

* The log says `image is the retail exe ...; retail addresses applied`.
* **Arxan.** Retail is Arxan-protected: it kills the game about 20 s after a code patch unless its integrity checks are
  neutralised first. bo3_arxan.h rewrites each check's checksum store so the check always passes, before any other patch
  is made. The log line is `arxan: 1069 integrity check(s) neutralised` (1000 intact checks plus 69 obfuscated-store
  checks). If another module already neutralised some or all of them in the same process the log says
  `already neutralised ...`, or `69 obfuscated-store integrity check(s) neutralised (the intact ones were already
  done)`. If Arxan cannot be neutralised, nothing is installed. The Enhanced exe has no Arxan and is not touched.
* The view-axis hook logs once (retail has one AnglesToAxis call where Enhanced has two).
* Everything is checked against retail's code at install, as on Enhanced.

## Debug switches (environment variables)

Environment variables of the game process, not cfg keys; for bisecting crashes.

| Variable | Effect |
|---|---|
| `WEAPONTECH_CRASHLOG=1` | access violations are written to `weapon_tech.log` as they happen (exe+RVA, stack) |
| `WEAPONTECH_SKIP=a,b,...` | leaves install groups out: `melee`, `recoil`, `anim`, `perf`, `ik`, `inspect`, `intmelee`, `interrupt`, `segreload`, `slide`, `vmfov`, `arxan` |

## Known quirks

* `perf_eventhooks=` and `perf_cfgwatch=` log "live" on a reload but have no effect until a restart.
* `locomotion_jog=`, `idle_active_fade=`, `locomotion_debug=` and `perf_pollms=` / `perf_hotlog=` keep their last value
  when the line is deleted live (put the default back instead of deleting the line).
* `additive_debug=names` skips every per-frame feature, not only the additive layers.
* Every `ammohide*` key is read at load only; a live edit needs a restart.
* The slide switches `slide_sprint_lock`, `slide_camera` and `slide_view` default to 0 when absent from the cfg; the
  example config turns them on for the full IW8 feel.
