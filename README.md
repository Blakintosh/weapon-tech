# weapon_tech

**ALPHA.** Expect rough edges, behaviour that changes between builds, and features that have only been exercised in a few
maps and guns. Nothing here is a stable API.

`weapon_tech.dll` is a Black Ops III viewmodel and weapon-feel DLL. It hooks the game's viewmodel animation update and the
BG weapon code and, driven entirely by a text file (`weapon_tech.cfg`), adds the things modern Call of Duty guns do that
BO3's weapon system can't: additive animation layers (recoil, bullets, empty), IW8 kick and camera patterns, MW19 sway and
locomotion, inspect, interrupts, akimbo per-hand layers, hand IK, the MW2019 slide and more.

* **Supported executables:** BO3 Enhanced (CL 20659811; `SizeOfImage 0x1A53F000`, `TimeDateStamp 0x67363F2A`) and the stock
  retail exe (CL 13892626; `0x1D74B000`, `0x693D731E`). On any other build the DLL installs nothing.
* **Independent of T7Overcharged** (or any other DLL). It only needs to be loaded from the map's UI Lua. It also carries its
  own Arxan neutraliser for retail.
* **Everything is opt-in.** With no cfg, or a cfg without lines for a feature, that feature installs no hooks and changes
  nothing.

## Install and loading

1. Build `weapon_tech.dll` (below) or take a release build.
2. Put `weapon_tech.dll` and `weapon_tech.cfg` together in the map's `zone` folder, e.g.
   `usermaps\<map>\zone\`. The cfg is read from next to the DLL.
3. Load the DLL once from the map's UI Lua (main/LUI thread), before the first weapon raise, e.g. from a file the map's
   Lua already loads:

   ```lua
   local pkg = require("package")
   local ok, init = pcall(pkg.loadlib, [[.\usermaps\<map>\zone\weapon_tech.dll]], "init")
   if ok and init then
       local called, active = pcall(init, true)
       if called and active == true then Engine.SetDvar("weapontech_active", 1) end
   end
   ```

   `init(true)` returns `true` when the weapon tech is active (cfg found, known exe, hooks in), `nil` otherwise. It is safe
   to call on every level load; the work is done once per process. Scripts can read the `weapontech_active` dvar.
4. Log: `weapon_tech.log` next to `BlackOps3.exe`. It names the cfg it used, every hook it installed, and anything it
   refused. **Read it after the first run.**

Where the cfg comes from (first found wins): the loose `weapon_tech.cfg` next to the DLL (live-reloaded while the game runs,
see below), else a rawfile `weapon_tech/weapon_tech.cfg` baked into the map's zone (read once). Weapon names in the cfg are
the weapon variant names (e.g. `ar_mike16_kar_zm`), case-sensitive.

Cfg syntax: `key=value`, one per line, up to 255 characters (longer lists continue on `<key>=<weapon>,+,...` lines).
Put `#` comments on their own line (a trailing comment breaks `additive=` and `wop=` lines). Unknown keys are logged once.
Start from [`examples/weapon_tech.cfg`](examples/weapon_tech.cfg) and see
[`docs/CONFIG_REFERENCE.md`](docs/CONFIG_REFERENCE.md) for every key, default, and what is live.

Live tuning: with `weapon_tech.cfg` loose, a watcher thread re-reads it when its modification time changes. Keys marked
"live" in the reference apply immediately; the rest (anything that installs a hook or writes an anim slot) need a restart.

## Features and cfg keys

Details, formats and defaults for every key are in the reference. This is the overview.

### Additive layers and semantic slots
Write an additive xanim into a spare slot of the gun's anim tree and drive its weight and time from game state.
* `additive=<weapon>,<empty|recoil|bullet>,<193|195>,<xanim>[,weight[,mag|rate]]`: the legacy form (root 193/195).
* `additive=<weapon>,<kind>,slot:<purpose>,<xanim>[,weight[,mag|rate]][,side:left]`: purpose slots built from spare juke
  leaves: `bullets` (root 192 / leaf 117), `empty` (190 / 118), `recoil_ads` (189 / 119), `idle`, `recoil`.
* `slots_take_jukes=<weapon|all>` lets slots use juke roots on a gun that names its juke anims; `slots_debug`, `slots_dump`.
* `additive_enable`, `additive_melee_fade`, `additive_debug`.
* `empty` = the gun-empty pose (bolt back, slide locked); `bullet` = round count driven by the clip (anim frame per round);
  `recoil` = an additive scrubbed per shot at `rate`.

### Akimbo (dual wield) per-side layers
`side:left` drives a slot line from the LEFT gun's clip and state (empty root 186 / leaf 124, bullets 185 / 108), so each
hand's slide or rounds follow their own gun. The left xanim must key the left gun's bones (see Authoring).
`side:right` is the default. No left recoil layer.

### IW8 kick and recoil patterns
The IW8 weapon-offset (WOP) patterns and view kick: `wop_weapon`, `wop_curve`, `wop` (patterns), `wop_kick` (+ `wop_kickpct`),
`wop_spring` (replaces BO3's view-kick integrator), `wop_tilt`, `wop_alias`, `wop_kick_consts=iw8|legacy`, `wop_debug`.
`tools/iw8_wop_cfg.py` converts an IW8 weapon json.
* **Kick return:** `wop_kickreturn=1` (global, plus per weapon `wop_kickreturn=<weapon>,<0|1>[,maintain[,noDampening]]`).
* **Camera shake / free:** `cam_shake=<pitch/yaw>,<roll>,<origin>[,pitchUp]` (global or per weapon) scales the pattern camera
  shake; `camera_free=<0..1>` makes tag_camera move only the camera (IW8 behaviour). Only act on WOP weapons.

### idle_active, locomotion, sway
* `idle_active=<weapon>,<xanim|*>,<leaf>[,weight[,rate]]` plus `idle_active_fade=iw8|linear[,s]`: IW8's looping hip idle
  additive (full weight at the hip, off in ADS).
* `locomotion=<weapon>,walk,bob,<strides>` and `locomotion=<weapon>,jog,<leaf>[,...]`, `locomotion_alias`, `locomotion_jog`:
  MW19 walk loops locked to BO3's bob (multi-stride anims) and a jog layer.
* `sway_*` (`sway_adv`, `sway_advgun`, `sway_idle`, `sway_stance`, `sway_adsbob`, ...): MW19 advanced hip sway, idle,
  stance pivots and ADS gun bob, ported from IW8.

### Inspect, empty inspect, HUD hide
`inspect_enable=1` + `inspect=<weapon>,1[,seconds]` + `inspect_key=I`. The inspect plays the gun's `lowReadyLoop` anim. MWII
empty inspects use `lowReadyIn` / `lowReadyOut` when the clip is 0 (`inspect_empty=in|out|off`). `inspect_hidehud=1` hides the
HUD (`hudItems.weaponTech.inspecting` / `inspectHideHud` UI models); the crosshair always hides. `inspect_akimbo` (default 1)
handles dual wield.

### empty_lastshot
`empty_lastshot=<auto|iw|hold>` or `<weapon>,<mode>`: how the empty layer comes on after the last round. `hold` stays off
while a real fire_last anim plays and then takes `1 - lastShot weight` (no double lock); `iw` is MW2019's rule (on at clip 0,
0.05 s blend); `auto` (default) picks per gun from its anim names. The empty layer also hands off to reloads without a pop.

### Empty melee and interrupts
* `empty_melee_fix=1`: melee works with an empty clip (three code patches).
* `interrupt_empty_melee=1`: rapid melee with an empty clip (post-melee quick raise stays interruptible).
* `interrupt=<weapon>,<states>,<source>[,<actions>]`: IW-style interrupts, a raise / reload / rechamber ends early at its
  interrupt point when fire, ADS, sprint, melee, reload or a weapon switch is pending (source: the anim's `interruptible` /
  `state_timer_end` notetracks, a time, or a heuristic). `interrupt_enable`, `interrupt_debug`, `interrupt_trace`.
* While the viewmodel shows another weapon (a knife melee, an offhand) the held gun's layers are not written into that tree.

### Segmented reloads
`segreload_empty=<weapon>,<end|start>` (`mw19` = `end`, `mwii` = `start`, `off`): shell-by-shell reloads with an MW2019
rechamber end or an MWII empty start, using `reloadEmptyAnim` / `reloadEmptyTime`. `segreload_enable=0` turns it off.

### Hand IK (experimental)
`ik=<weapon>,1[,l|r|lr[,notelessWeight[,orient]]]`, `ik_enable=1` (global, default 0), `ik_alias`, `ik_notes`, `ik_blend`,
`ik_always`, `ik_debug`. A two-bone solve of the hands onto the gun's `tag_ik_loc_*`, weighted by the IW8
`ik_{in,out}_{start,end}_{left,right}_hand` notetracks. Needed for non-rigid idle_active additives.

### MW slide
`slide_enable=1` (default 0). IW8 slide movement in BG (server and prediction agree): `slide_suit` (preset or SuitDef
overrides), `slide_dvars`, `slide_ads_ends`, `slide_sprint_lock`, `slide_camera`, `slide_view`, `slide_snap_round`
(rounds BO3's velocity truncation), and the slide gesture additive layer on nodes 187/188/191 (`slide_gesture*`,
`slide_native_anims`). `slide_debug=1..3`.

### Viewmodel FOV pin
`vmfov=off|mw|<degrees>` (65 = the viewmodels' authored FOV), `vmfov_ads=fade|hold`, `vmfov_depth`, `vmfov_max`,
`vmfov_debug`. BO3 has one projection, so the gun is moved along the view axis to match the on-screen size of the pinned FOV.

### Ammo hide
Spent rounds hide through the DObj hide bits (like HidePart), driven by the clip, for every gun with a `bullet` line.
* `ammohide_auto=0|1` (default 1, or `<weapon>,0` to opt out): joints = the bullets anim's round-named parts (`bullet`,
  `round`, `shell`, `j_b_<n>`, and BO7/IW9 `j_ammo_*` incl. left-gun `_le` / `1`-suffix names; `tag_ammo_*` tags,
  followers and mags are not rounds). Default order: nearest the follower / pusher (else `j_bolt` / `tag_flash`) first.
* `ammohide_order=<weapon>,<joint>,...` (first spent first) and `ammohide_spend=<weapon>,<joint>:<clip>,...` (hidden at
  `<clip>` rounds or fewer), `ammohide_reverse=<weapon>`: generated by `tools/ammohide_order.py` from the bullets anim
  (BEGIN/END section in the cfg). An explicit `ammohide=<weapon>,<joint>,...` line wins over everything.
* `ammohide_reload=0|1` (default 1): through a reload the mag shows the count the reload anim's `gramien_hide_full_magazine` /
  `gramien_show_full_magazine` / `gramien_watch_ammo` notes give.
* **Gramien notes:** if the mag still uses a Gramien `_dyn` scripted-animation material, its vertex animation adds a second
  copy of the feed motion on top of the bullets additive. Skip the gun in the Gramien script (script names, no `_zm`) or give
  the mag a static material.
* **Round guard** (`ammohide_guard=0|1`, `ammohide_park`, `ammohide_debug`): bullet anims park spent rounds far away. After
  each viewmodel skeleton build, any round-like bone parked more than `ammohide_park` (12) units from its base pose is hidden
  with everything under it, so a spent round never floats near the camera.

### Pause clock
No key. Every time-driven layer runs on a game clock held while cg time stalls (the solo pause menu), so layers freeze
and resume from the same pose.

### Arxan handling on retail
The retail exe's Arxan integrity checks (1,069 on CL 13892626: 1,000 plain plus 69 obfuscated-store checks) would kill the
game ~20 s after any code patch. weapon_tech neutralises them first (`bo3_arxan.h`, once per process, shared through a named
record with other modules that carry the same code); if that fails it installs nothing. A standalone copy is in
[`extras/arxan`](extras/arxan). Debug: `WEAPONTECH_CRASHLOG=1` logs access violations; `WEAPONTECH_SKIP=a,b,...` leaves
install groups out (melee, recoil, anim, perf, ik, inspect, intmelee, interrupt, segreload, slide, vmfov, arxan).

Perf keys: `perf_eventhooks`, `perf_cfgwatch`, `perf_pollms`, `perf_hotlog`, `perf_timing`.

## Authoring anims for it

* **Additives:** frame 0 is the reference pose; the linker drops it, so a pose additive needs **3 frames** (reference, pose,
  pose). A 2-frame pose additive never binds on a slot (the log says so). Slot xanims are additive on the gun's own bones,
  never the hands.
* **Bullets anims:** one frame per round spent, frame 0 = full mag. Rounds the clip has spent move to their final pose
  (IW/BO7 convention: far away or into the mag). The `mag` argument of the `bullet` line is the anim's round count; BO3 clip
  may be mag + 1 with a chambered round. Run `tools/ammohide_order.py` to derive the hide order.
* **Akimbo left gun:** the dual-wield rig renames the left gun's bones with a suffix (`j_slide1`, `tag_pistol_offset1`,
  `tag_weapon_le`, `j_bolt1`, `j_ammo_011`, `tag_brass_le`). A `side:left` xanim must key those names.
  `tools/gen_left_additives.py` builds the left twin from the right gun's additive (left idle pose x the right bone's own-frame
  delta). The GDT model must be a skeleton that holds both guns.
* **Inspect:** put the anims in `lowReadyLoopAnim` (normal) and `lowReadyInAnim` / `lowReadyOutAnim` (empty); remove any
  script that calls `SetLowReady` for the same gun.
* **Slide gesture:** in / loop / out additives in `jukeForwardAnim` (187), `jukeBackwardAnim` (188), `jukeForwardADSAnim` (191);
  each xanim = the idle frame, then the gesture frames.
* **IK notes:** `ik_{in,out}_{start,end}_{left,right}_hand` notetracks, or `ik_notes=` lines if the linker strips them.
* **Locomotion:** multi-stride walk loops (4 strides for an MW19 `walk_loop`) need a `locomotion=...,walk,bob,<strides>` line,
  otherwise BO3 plays the whole anim per stride, several times too fast.
* **GDT edits** must be re-read by your linker pipeline before linking, or the stale data is used.

## Building

Needs Visual Studio with the C++ x64 tools. `build.bat` (or `build.ps1`) finds `vcvars64.bat` through `vswhere`, then runs

```
cl /nologo /O2 /MT /EHsc /std:c++17 /LD weapon_tech.cpp /Fe:weapon_tech.dll /link kernel32.lib
```

from `src\` and writes `build\weapon_tech.dll`. `build.bat -Out <dir>` changes the output folder; `-Extras` also compile-checks
`extras\arxan\arxan.cpp`. Set `VCVARS` to a `vcvars64.bat` path to skip the search.

## Repo layout

```
src/                 the DLL source: weapon_tech.cpp + bo3_*.h headers (one translation unit)
docs/                CONFIG_REFERENCE.md
examples/            weapon_tech.cfg: a commented sample for each feature
tools/               ammohide_order.py, gen_left_additives.py and other content authoring helpers (tools/README.md)
extras/arxan/        standalone Arxan neutraliser (arxan.cpp / arxan.hpp), no project dependencies
build.bat, build.ps1
```

## Known issues and limitations (alpha)

* Only the two exes above are supported; the addresses are matched against the code at install time and a mismatch installs
  that hook group off rather than guessing.
* Most hook-installing keys are not live: edit the cfg and restart the game (the reference marks what is live).
* Hand IK is experimental and has seen little in-game testing. `camera_free` only acts on WOP weapons. Interrupts, segmented
  reloads and inspect are tuned per gun and may need per-gun cfg work.
* Auto ammo hide uses one `bullet` line per weapon variant; the left gun of a dual-wield pair does not get its own hide
  (the `j_ammo_*` left names are recognised, but only the first bullet line's anim is read). The bullets time mapping can run
  about one round ahead early in the mag, because the linker drops frame 0. Compiled anims can drop parts the GDT skeleton
  lacks; the generated order lines name every joint so hiding does not depend on them.
* The kick-return capture only takes effect with at least one `wop_kick` line.
* An older neutraliser that patches only the 1,000 plain Arxan checks crashes retail about 20 s later; use the one in this
  repo.
* Typos in keys are only logged, not rejected.
* No license has been chosen yet.
