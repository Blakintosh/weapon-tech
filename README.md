# weapon_tech

> **Alpha.** Things will change between builds, and some features have only been tried on a handful of maps and guns.

`weapon_tech.dll` brings modern Call of Duty weapon feel to Black Ops III custom maps. It adds recoil and empty-state
animation layers, MW2019-style kick and sway, inspects, akimbo per-hand animation, and more. Everything is driven by
one text file, `weapon_tech.cfg`.

- **Works on both PC builds:** BO3 Enhanced and the standard retail exe. On any other exe it does nothing.
- **Standalone:** no dependency on T7Overcharged or any other DLL.
- **Opt-in:** each feature only switches on for the guns you list in the cfg. Guns you don't mention behave exactly
  as stock BO3.

## What it can do

| Feature | What you get |
|---|---|
| **Additive layers** | Recoil, live round count (bullets) and empty-gun pose (bolt back, slide locked) as additive animations on any gun |
| **Akimbo per-hand layers** | Each dual-wield gun shows its own empty and bullet state |
| **Ammo hide** | Spent rounds disappear from see-through mags and belts as you fire, and come back on reload |
| **IW8 kick & recoil patterns** | MW2019 weapon-offset patterns, view kick, kick return, camera shake |
| **Idle, locomotion & sway** | MW2019 idle_active, walk and jog loops, advanced sway |
| **Inspect** | Press a key to inspect; separate empty-gun inspect; optional HUD hide |
| **Last shot** | The gun locks empty at the right moment, with or without a fire-last animation |
| **Melee & interrupts** *(interrupts experimental)* | Rapid melee on an empty gun; raises and reloads can be cut short like in IW games |
| **Segmented reloads** | Shell-by-shell reloads with MW2019- or MWII-style empty handling |
| **MW slide** | MW2019 slide movement and slide gestures (global opt-in) |
| **Viewmodel FOV pin** | Keeps the gun the same size on screen whatever the player's FOV (global opt-in) |
| **Hand IK** *(experimental)* | Keeps the hands on the gun during non-rigid additives |

## Quick start

1. **Build** the DLL (see [Building](#building)), or grab a release.
2. **Copy** `weapon_tech.dll` and a `weapon_tech.cfg` into your map's `zone` folder, e.g. `usermaps\<map>\zone\`.
3. **Load it** once from your map's UI Lua, before the player first raises a weapon:

   ```lua
   local pkg = require("package")
   local ok, init = pcall(pkg.loadlib, [[.\usermaps\<map>\zone\weapon_tech.dll]], "init")
   if ok and init then
       local called, active = pcall(init, true)
       if called and active == true then Engine.SetDvar("weapontech_active", 1) end
   end
   ```

   `init(true)` returns `true` when weapon_tech is running. It's safe to call on every level load.
4. **Add a gun** to the cfg. For example, a recoil layer and an inspect for one weapon:

   ```ini
   [features]
   additives = on
   inspect   = on

   [additives]
   additive=smg_charlie9_kar_zm,recoil,195,vm_sm_charlie9_recoil_additive,1.0,2.27

   [inspect]
   guns = smg_charlie9_kar_zm
   ```

5. **Check the log:** `weapon_tech.log` next to `BlackOps3.exe` lists the cfg it read, what it installed, and
   anything it skipped and why. Read it after your first run.

[`examples/weapon_tech.cfg`](examples/weapon_tech.cfg) has a commented example of every feature to copy from.

## The cfg file

- **Sections:** `[features]` switches each feature on or off: a feature that is off installs nothing, even with its
  lines in the file. Every other section holds one feature's lines: `[additives]`, `[ammo_hide]`, `[kick]`,
  `[camera]`, `[locomotion]`, `[inspect]`, `[last_shot]`, `[empty_melee]`, `[interrupts]`, `[segreload]`, `[slide]`,
  `[vmfov]`, `[ik]`, and `[general]` for perf and debug keys.
- **Keys:** `key = value`, max 255 characters a line. Inside a section the feature prefix may be dropped (`key = I` in
  `[inspect]` is `inspect_key`); full key names work in any section.
- **Guns lists:** the simple per-gun features take a list: `guns = gun_a, gun_b:6.6, gun_c:off` (inspect, ik,
  segreload, last_shot). Data-heavy lines (`additive=`, `wop=`, `sway_*=`) stay one line per record in their section.
- **Per weapon:** `[weapon:<name>]` sections hold one gun's settings across features with the `wt*` keys a GDT
  compiler will emit; they override that gun's guns-list entries.
- **Generated blocks:** tools write between `# ==== BEGIN generated:<tool> ====` and `# ==== END generated:<tool> ====`
  and rewrite the block whole. Edit outside the fences.
- **Old flat files** (no `[section]`) still work unchanged. `tools/cfg_migrate.py <cfg> --in-place --check` converts one
  and checks the DLL parses both to the same state (`cfg_dump = 1` does the same check in game).
- Comments start with `#`. Weapon names are the full variant names (e.g. `ar_mike16_kar_zm`) and are case-sensitive.
- **Live tuning:** while the game runs, saving the cfg reloads it (`[features]` itself is read at start). Simple values apply straight away; anything that
  sets up hooks or animation slots needs a game restart. [`docs/CONFIG_REFERENCE.md`](docs/CONFIG_REFERENCE.md) marks
  which keys are live and lists every key and default.
- **Baked cfg:** if there's no loose cfg next to the DLL, weapon_tech reads a rawfile `weapon_tech/weapon_tech.cfg`
  from your map's zone instead (see [Shipping your map](#shipping-your-map)).

## Shipping your map

No linker changes are needed. When you publish:

1. Keep `weapon_tech.dll` in `usermaps\<map>\zone\`, next to your fastfile, so it uploads with the map.
2. Ship the cfg one of two ways:
   - **Loose file** (simplest): leave `weapon_tech.cfg` next to the DLL. Players can see and edit it.
   - **Baked into the map:** put it at `usermaps\<map>\weapon_tech\weapon_tech.cfg` and add
     `rawfile,weapon_tech/weapon_tech.cfg` to your map's `.zone` file, then remove the loose copy from `zone\`.
     A loose file, if present, always wins, which is handy for tuning during development.
3. Make sure your UI Lua loads the DLL (step 3 of the Quick start) in the shipped build, not just a dev one.

## Feature guide

A short guide to each feature. The full syntax for every key is in the [config reference](docs/CONFIG_REFERENCE.md).

| Feature | Turn it on with | Default | Enhanced | Retail | Status |
|---|---|---|:-:|:-:|---|
| Additive layers (recoil / bullets / empty) | `additive=` lines per gun | off | ✅ | ✅ | Stable |
| Akimbo per-hand layers | `,side:left` on a slot line | off | ✅ | ✅ | Stable |
| Ammo hide | any gun with a `bullet` line | **on** | ✅ | ✅ | Stable |
| Round guard | comes with ammo hide | **on** | ✅ | ✅ | New |
| IW8 kick & recoil patterns | `wop_*` lines per gun | off | ✅ | ✅ | Stable |
| Kick return | `wop_kickreturn=1` | off | ✅ | ✅ | New |
| Camera shake / free camera | `cam_shake=`, `camera_free=` | off | ✅ | ✅ | Stable |
| idle_active / locomotion / sway | `idle_active=`, `locomotion=`, `sway_*` | off | ✅ | ✅ | Stable |
| Inspect & empty inspect | `inspect_enable=1` + `inspect=` | off | ✅ | ✅ | Stable |
| Last shot (`empty_lastshot`) | automatic with an `empty` layer | auto | ✅ | ✅ | Stable |
| Empty-gun melee / rapid melee | `empty_melee_fix=1`, `interrupt_empty_melee=1` | off | ✅ | ✅ | Stable |
| Interrupts | `interrupt=` lines | off | ✅ | ✅ | Experimental |
| Segmented reloads | `segreload_empty=` | off | ✅ | ✅ | Stable |
| MW slide | `slide_enable=1` (map-wide) | off | ✅ | ✅ | Stable |
| Viewmodel FOV pin | `vmfov=mw` (map-wide) | off | ✅ | ✅ | Stable |
| Hand IK | `ik_enable=1` + `ik=` | off | ✅ | ✅ | Experimental |
| Pause clock | always | **on** | ✅ | ✅ | Stable |

### Additive layers
Plays an additive animation on top of the gun's normal animation, driven by game state.

- `empty`: the empty-gun pose (slide locked, bolt back), shown at 0 ammo.
- `bullet`: a round-count animation, one frame per round, driven by the clip.
- `recoil`: played per shot.

Recoil uses `additive=<weapon>,recoil,195,<xanim>,<weight>,<rate>`. Empty and bullets go in named slots:
`additive=<weapon>,empty,slot:empty,<xanim>,1.0`, or `slot:bullets` with the mag size as the last value.

### Akimbo per-hand layers
Add `,side:left` to an `empty` or `bullet` line to drive it from the left gun, so each hand's slide and rounds follow
their own ammo. The left animation must use the left gun's bone names (see [Authoring](#authoring-animations)).

### Ammo hide
On by default for every gun with a `bullet` line: spent rounds are hidden as the clip drops, and come back when the
reload puts a full mag in.

- **Hide order:** `tools/ammohide_order.py` works out the order from the bullets animation and writes it into your cfg.
- **Reload timing:** follows Gramien-style `gramien_hide_full_magazine` / `gramien_show_full_magazine` /
  `gramien_watch_ammo` notetracks on the reload anim if they're there. Otherwise the old count shows until the
  ammo is added.
- **Round guard:** any round the animation throws far away is hidden as well, so nothing floats near the camera.
- **Opt out:** `ammohide_auto=<weapon>,0` for one gun.
- If a mag still uses a Gramien `_dyn` material, give it a normal material or skip it in the Gramien script, or the
  feed motion is doubled.

### IW8 kick & recoil patterns
MW2019's weapon-offset patterns (`wop_*` lines) and view kick (`wop_kick`, `wop_spring`).

- **From IW8 data:** `tools/iw8_wop_cfg.py` turns an IW8 weapon JSON into cfg lines.
- **Kick return:** `wop_kickreturn=1`.
- **Camera:** `cam_shake` scales camera shake, and `camera_free` lets camera animations move only the camera.

### Idle, locomotion & sway
- `idle_active`: MW2019's looping hip idle.
- `locomotion`: walk loops synced to BO3's view bob, plus a jog layer.
- `sway_*`: MW2019's advanced hip sway, idle sway and stance pivots.

### Inspect
`inspect_enable=1`, then `inspect=<weapon>,1` per gun. The default key is **I**. It plays the gun's
`lowReadyLoopAnim`.

- **Empty inspect:** with an empty clip it plays `lowReadyInAnim` instead (MWII style). `inspect_empty` switches
  between `in`, `out` and `off`.
- **HUD:** `inspect_hidehud=1` hides the HUD while inspecting. The crosshair always hides.
- **Akimbo:** dual-wield guns are supported.

### Last shot
`empty_lastshot` decides when the empty pose comes in after the last round. The default `auto` handles both cases:

- A gun with a real fire-last animation plays it, then locks empty with no pop.
- A gun without one locks empty straight away, like MW2019.

### Melee & interrupts (interrupts experimental)
- `empty_melee_fix=1`: melee works on an empty gun.
- `interrupt_empty_melee=1`: rapid melee on an empty gun, the same as with ammo.
- `interrupt=...`: lets raises, reloads and rechambers end early when the player fires, aims, sprints, melees,
  reloads or switches. The cut-off point comes from the anim's notetracks, a time, or a heuristic.

### Segmented reloads
`segreload_empty=<weapon>,end` (MW2019: rechamber at the end) or `start` (MWII: rechamber first) for shell-by-shell
weapons.

### MW slide
`slide_enable=1` turns on MW2019 slide movement for the whole map, with the slide gesture animations. Off by default.

### Viewmodel FOV pin
`vmfov=mw` (or a number of degrees) keeps the gun the same size on screen whatever the player's FOV. Off by default.

### Hand IK *(experimental)*
`ik_enable=1` plus `ik=<weapon>,1` keeps the hands on the gun, using IW8's IK notetracks. It has seen little
in-game testing.

## Authoring animations

- **Pose additives need 3 frames:** frame 0 is the reference (dropped by the linker), then the pose twice. A 2-frame
  pose never shows up, and the log will tell you.
- **Bullets animations:** one frame per round, with frame 0 = full mag. Spent rounds move to their final spot, either
  into the mag or far away, as IW and BO7 do.
- **Akimbo left gun:** the dual-wield rig renames the left gun's bones, e.g. `j_slide1`, `j_bolt1`,
  `tag_weapon_le`, `j_ammo_011`. `tools/gen_left_additives.py` builds the left-gun version of a right-gun additive.
- **Inspect:** the normal inspect goes in `lowReadyLoopAnim`, the empty inspect in `lowReadyInAnim`. Remove any
  script that calls `SetLowReady` on the same gun.
- **Walk loops:** MW2019 walk loops cover several strides, so tell weapon_tech how many with a
  `locomotion=<weapon>,walk,bob,<strides>` line.
- **After GDT edits,** update your GDT database before linking, or the old data gets used.

`tools/` has the authoring helpers; see [`tools/README.md`](tools/README.md).

## Building

You need Visual Studio with the C++ x64 tools.

```
build.bat
```

This writes `build\weapon_tech.dll`.

- `-Out <dir>` changes the output folder.
- `-Extras` also builds `extras\arxan`.
- If it can't find Visual Studio, set `VCVARS` to your `vcvars64.bat`.

## Troubleshooting

- **Which version is this?** The first line of `weapon_tech.log` says, e.g. `weapon_tech 0.1.0-alpha loaded from ...`.
  It's also in the DLL's file properties, and tools can call the exported `weapon_tech_version()`.
- **Nothing happens:** check `weapon_tech.log`. It says whether the exe was recognised, which cfg was read, and why
  a feature was skipped.
- **A cfg change didn't apply:** that key probably needs a game restart; see the reference.
- **A typo in a key:** it's ignored and logged once.
- **Retail crashes after about 20 seconds:** another mod is patching code without handling Arxan fully. weapon_tech
  handles it itself; see [`extras/arxan`](extras/arxan) if you need the same fix in your own DLL.

## Known issues

- Only BO3 Enhanced (CL 20659811) and retail (CL 13892626) are supported.
- Most setup keys need a game restart to change.
- On dual-wield guns, the left gun's spent rounds don't hide yet. Its empty and bullets layers do work.
- The bullets animation can run about one round ahead near a full mag.
- Hand IK and interrupts are experimental (little in-game testing yet). Segmented reloads and inspects may need per-gun tuning.
- Kick return needs at least one `wop_kick` line for the gun.

## Repo layout

```
src/            the DLL source (weapon_tech.cpp + headers)
docs/           CONFIG_REFERENCE.md: every key in detail
examples/       weapon_tech.cfg: a commented sample of every feature
tools/          authoring helpers (hide order, left-gun anims, IW8 recoil data)
extras/arxan/   standalone Arxan neutraliser for retail
build.bat, build.ps1
```

## Credits

- **BOIII:** the retail Arxan neutralising approach comes from the BOIII
  client, as do many of the reference points used to map the retail exe.
- **T7Overcharged:** weapon_tech's Arxan handling started as a port of T7Overcharged's `arxan.cpp`, and it
  follows T7Overcharged's approach to supporting both Enhanced and retail.

## License

MIT. See [LICENSE](LICENSE).
