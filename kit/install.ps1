<#
Installs weapon_tech into a Black Ops III usermap.

  install.bat <mapname> [-GameDir <path>] [-Bake] [-Force]

  -GameDir  the BO3 folder (the one with usermaps\). Found from Steam if left out.
  -Bake     also bake the cfg into the map as a rawfile (weapon_tech\weapon_tech.cfg + a zone line)
  -Force    replace an edited loader Lua with the kit's (the old one is backed up)

Copies weapon_tech.dll and weapon_tech.cfg into usermaps\<map>\zone\, the loader Lua into usermaps\<map>\ui\t7\utility\,
and adds the loader's rawfile line to zone_source\<map>.zone (backup first). Never overwrites an existing cfg, and a
second run changes nothing. Prints what it did and the one line you add to your CSC by hand.
#>
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$MapName,
    [string]$GameDir,
    [switch]$Bake,
    [switch]$Force,
    [string]$Dll
)
$ErrorActionPreference = 'Stop'
$kit = $PSScriptRoot
$utf8 = New-Object System.Text.UTF8Encoding($false)
$changes = New-Object System.Collections.Generic.List[string]
$manual = New-Object System.Collections.Generic.List[string]

function Fail($msg) { Write-Host "install: $msg" -ForegroundColor Red; exit 1 }
function Changed($msg) { $script:changes.Add($msg); Write-Host "  changed    $msg" }
function Same($msg) { Write-Host "  unchanged  $msg" }

if ($MapName -notmatch '^[A-Za-z0-9_\-]+$') { Fail "'$MapName' isn't a map folder name (letters, digits, _ and - only)." }

# ---- the BO3 folder ----------------------------------------------------------------------------------------------
function Get-SteamLibraries {
    $roots = @()
    foreach ($k in @('HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam', 'HKCU:\Software\Valve\Steam')) {
        try {
            $p = Get-ItemProperty -Path $k -ErrorAction Stop
            foreach ($n in @('InstallPath', 'SteamPath')) { if ($p.$n) { $roots += ($p.$n -replace '/', '\') } }
        } catch { }
    }
    $libs = @()
    foreach ($r in ($roots | Select-Object -Unique)) {
        $libs += $r
        $vdf = Join-Path $r 'steamapps\libraryfolders.vdf'
        if (Test-Path $vdf) {
            foreach ($m in [regex]::Matches((Get-Content $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $libs += ($m.Groups[1].Value -replace '\\\\', '\')
            }
        }
    }
    $libs | Select-Object -Unique
}

if ($GameDir) {
    if (-not (Test-Path (Join-Path $GameDir 'usermaps'))) { Fail "no usermaps folder in -GameDir '$GameDir'." }
} else {
    $fallback = $null
    foreach ($lib in (Get-SteamLibraries)) {
        $cand = Join-Path $lib 'steamapps\common\Call of Duty Black Ops III'
        if (Test-Path (Join-Path $cand "usermaps\$MapName")) { $GameDir = $cand; break }
        if (-not $fallback -and (Test-Path (Join-Path $cand 'usermaps'))) { $fallback = $cand }
    }
    if (-not $GameDir) { $GameDir = $fallback }
    if (-not $GameDir) { Fail "couldn't find Black Ops III through Steam. Pass the folder: install.bat $MapName -GameDir `"C:\...\Call of Duty Black Ops III`"" }
}
$mapDir = Join-Path $GameDir "usermaps\$MapName"
if (-not (Test-Path $mapDir)) { Fail "no usermaps\$MapName in '$GameDir'. Create the map in the Mod Tools first, or check the name." }

# ---- the kit's files ---------------------------------------------------------------------------------------------
if (-not $Dll) {
    foreach ($c in @("$kit\..\weapon_tech.dll", "$kit\weapon_tech.dll", "$kit\..\build\weapon_tech.dll")) {
        if (Test-Path $c) { $Dll = (Resolve-Path $c).Path; break }
    }
}
if (-not $Dll -or -not (Test-Path $Dll)) { Fail "weapon_tech.dll not found next to the kit. Build it (build.bat) or pass -Dll <path>." }
$cfgSrc = Join-Path $kit 'weapon_tech.cfg'
$luaSrc = Join-Path $kit 'ui\t7\utility\weapon_tech_loader.lua'
foreach ($f in @($cfgSrc, $luaSrc)) { if (-not (Test-Path $f)) { Fail "missing kit file $f" } }

Write-Host "weapon_tech install: map '$MapName' in $GameDir"

function Same-File($a, $b) { (Test-Path $b) -and ((Get-FileHash $a -Algorithm SHA256).Hash -eq (Get-FileHash $b -Algorithm SHA256).Hash) }
function New-Dir($d) { if (-not (Test-Path $d)) { New-Item -ItemType Directory -Force $d | Out-Null } }

# ---- DLL ---------------------------------------------------------------------------------------------------------
$zoneDir = Join-Path $mapDir 'zone'
New-Dir $zoneDir
$dllDst = Join-Path $zoneDir 'weapon_tech.dll'
if (Same-File $Dll $dllDst) { Same 'zone\weapon_tech.dll' }
else {
    try { Copy-Item $Dll $dllDst -Force } catch { Fail "couldn't write $dllDst (is the game running?): $($_.Exception.Message)" }
    Changed 'zone\weapon_tech.dll copied'
}

# ---- cfg: never overwrite ----------------------------------------------------------------------------------------
$cfgDst = Join-Path $zoneDir 'weapon_tech.cfg'
if (-not (Test-Path $cfgDst)) {
    Copy-Item $cfgSrc $cfgDst
    Changed 'zone\weapon_tech.cfg created from the starter template'
} elseif (Same-File $cfgSrc $cfgDst) {
    Same 'zone\weapon_tech.cfg (the starter template)'
} else {
    $tpl = Join-Path $zoneDir 'weapon_tech.cfg.template'
    if (Same-File $cfgSrc $tpl) { Same 'zone\weapon_tech.cfg kept (yours); zone\weapon_tech.cfg.template is current' }
    else {
        Copy-Item $cfgSrc $tpl -Force
        Changed 'zone\weapon_tech.cfg exists, left alone; the kit''s template written to zone\weapon_tech.cfg.template'
    }
}

# ---- loader Lua --------------------------------------------------------------------------------------------------
$luaDir = Join-Path $mapDir 'ui\t7\utility'
$luaDst = Join-Path $luaDir 'weapon_tech_loader.lua'
$luaWant = (Get-Content $luaSrc -Raw).Replace('__WT_MAPNAME__', $MapName)
New-Dir $luaDir
if (-not (Test-Path $luaDst)) {
    [IO.File]::WriteAllText($luaDst, $luaWant, $utf8)
    Changed "ui\t7\utility\weapon_tech_loader.lua created (MAP_NAME = `"$MapName`")"
} elseif ((Get-Content $luaDst -Raw) -ceq $luaWant) {
    Same 'ui\t7\utility\weapon_tech_loader.lua'
} elseif ($Force) {
    $stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
    Copy-Item $luaDst "$luaDst.$stamp.bak"
    [IO.File]::WriteAllText($luaDst, $luaWant, $utf8)
    Changed "ui\t7\utility\weapon_tech_loader.lua replaced (backup weapon_tech_loader.lua.$stamp.bak)"
} else {
    Same 'ui\t7\utility\weapon_tech_loader.lua differs from the kit''s (edited?); left alone, -Force replaces it'
}

# ---- .zone lines -------------------------------------------------------------------------------------------------
function Test-ZoneLine($text, $pattern) {
    foreach ($l in ($text -split "`r?`n")) {
        $t = $l.Trim()
        if ($t.StartsWith('//')) { continue }
        if ($t -match $pattern) { return $true }
    }
    return $false
}

$loaderLine = 'rawfile,ui/t7/utility/weapon_tech_loader.lua'
$cfgLine = 'rawfile,weapon_tech/weapon_tech.cfg'
$zoneFile = Join-Path $mapDir "zone_source\$MapName.zone"
$add = New-Object System.Collections.Generic.List[string]
if (Test-Path $zoneFile) {
    $zoneText = [IO.File]::ReadAllText($zoneFile)
    if (Test-ZoneLine $zoneText '^rawfile\s*,\s*ui/t7/utility/weapon_tech_loader\.lua') { Same "zone_source\$MapName.zone has the loader line" }
    else { $add.Add($loaderLine) }
    if ($Bake) {
        # a hand-made line, or the weapontech linker feature's include, already bakes the cfg
        if (Test-ZoneLine $zoneText 'weapon_tech/weapon_tech\.cfg|^include\s*,\s*weapon_tech\s*$') { Same "zone_source\$MapName.zone already bakes the cfg" }
        else { $add.Add($cfgLine) }
    }
    if ($add.Count -gt 0) {
        $stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
        $bak = "$zoneFile.pre_weapontech_$stamp.bak"
        Copy-Item $zoneFile $bak
        $nl = if ($zoneText.Contains("`r`n")) { "`r`n" } else { "`n" }
        $lead = if ($zoneText.Length -gt 0 -and -not $zoneText.EndsWith("`n")) { $nl } else { '' }
        $block = $lead + $nl + '// weapon_tech (install.bat)' + $nl + (($add.ToArray()) -join $nl) + $nl
        [IO.File]::AppendAllText($zoneFile, $block, $utf8)
        Changed "zone_source\$MapName.zone: added $($add -join ', ') (backup $(Split-Path $bak -Leaf))"
    }
} else {
    $lines = @($loaderLine); if ($Bake) { $lines += $cfgLine }
    $manual.Add("No zone_source\$MapName.zone found. Add these to your map's .zone file:`n      " + ($lines -join "`n      "))
}

# ---- -Bake: the cfg source for the rawfile -----------------------------------------------------------------------
if ($Bake) {
    $bakeDir = Join-Path $mapDir 'weapon_tech'
    $bakeDst = Join-Path $bakeDir 'weapon_tech.cfg'
    New-Dir $bakeDir
    if (-not (Test-Path $bakeDst)) {
        Copy-Item $cfgDst $bakeDst
        Changed 'weapon_tech\weapon_tech.cfg created from zone\weapon_tech.cfg (the file the rawfile bakes)'
    } elseif (Same-File $cfgDst $bakeDst) {
        Same 'weapon_tech\weapon_tech.cfg matches zone\weapon_tech.cfg'
    } else {
        Same 'weapon_tech\weapon_tech.cfg exists and differs from zone\weapon_tech.cfg; left alone'
    }
    $manual.Add('-Bake: keep weapon_tech\weapon_tech.cfg in step with your edits. A loose zone\weapon_tech.cfg always wins over the baked one, so delete it from zone\ before you ship if you want the baked one used.')
}

# ---- the CSC line (told, never edited) ---------------------------------------------------------------------------
$luiLine = 'LuiLoad("ui.t7.utility.weapon_tech_loader");'
$scriptsDir = Join-Path $mapDir 'scripts'
$hit = $null
if (Test-Path $scriptsDir) {
    $hit = Get-ChildItem $scriptsDir -Recurse -Include *.csc -ErrorAction SilentlyContinue |
        Where-Object { (Get-Content $_.FullName -Raw) -match 'ui\.t7\.utility\.weapon_tech_loader' } | Select-Object -First 1
}
if ($hit) {
    Same "CSC already loads it ($($hit.FullName.Substring($mapDir.Length + 1)))"
} else {
    $csc = Join-Path $mapDir "scripts\zm\$MapName.csc"
    $where = if (Test-Path $csc) { "scripts\zm\$MapName.csc" } else { 'your map''s client script (scripts\zm\<map>.csc)' }
    $manual.Add("ADD THIS LINE at the top of main() in ${where}, then relink the map:`n      $luiLine")
}

# ---- summary -----------------------------------------------------------------------------------------------------
Write-Host ''
if ($changes.Count -eq 0) { Write-Host 'Nothing to change: already installed.' }
else { Write-Host "$($changes.Count) change(s) made." }
if ($manual.Count -gt 0) {
    Write-Host ''
    Write-Host 'Still to do by hand:'
    foreach ($m in $manual) { Write-Host "  * $m" }
}
Write-Host ''
Write-Host 'After your first run, read weapon_tech.log next to BlackOps3.exe: it says what was read and installed.'
exit 0
