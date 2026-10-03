<#
Builds weapon_tech.dll and packs a release zip: dist\weapon_tech-<version>.zip

  .\tools\package.ps1            build (build.bat) + zip
  .\tools\package.ps1 -NoBuild   zip the existing build\weapon_tech.dll

The version comes from src\wt_version.h (WT_VERSION_STRING). The zip holds one folder, weapon_tech-<version>\:
weapon_tech.dll, kit\ (installer, cfg template, loader Lua), README.md, LICENSE, docs\, examples\, INSTALL.txt.
#>
param([switch]$NoBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent

$hdr = Get-Content (Join-Path $root 'src\wt_version.h') -Raw
if ($hdr -notmatch '#define\s+WT_VERSION_STRING\s+"([^"]+)"') { throw 'WT_VERSION_STRING not found in src\wt_version.h' }
$version = $Matches[1]
$name = "weapon_tech-$version"

if (-not $NoBuild) {
    & cmd.exe /c "`"$root\build.bat`""
    if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
}
$dll = Join-Path $root 'build\weapon_tech.dll'
if (-not (Test-Path $dll)) { throw "$dll not found" }

$stage = Join-Path $root "dist\stage\$name"
if (Test-Path (Join-Path $root 'dist\stage')) { Remove-Item (Join-Path $root 'dist\stage') -Recurse -Force }
New-Item -ItemType Directory -Force $stage | Out-Null

Copy-Item $dll $stage
Copy-Item (Join-Path $root 'kit') (Join-Path $stage 'kit') -Recurse
Copy-Item (Join-Path $root 'docs') (Join-Path $stage 'docs') -Recurse
Copy-Item (Join-Path $root 'examples') (Join-Path $stage 'examples') -Recurse
Copy-Item (Join-Path $root 'README.md'), (Join-Path $root 'LICENSE') $stage

$install = @"
weapon_tech $version
====================

Modern CoD weapon feel for Black Ops III custom maps. Alpha: see README.md.

Install (2 minutes)
-------------------
1. Unzip this folder anywhere.
2. Open a command prompt in it and run:

       kit\install.bat <mapname>

   <mapname> is your map's folder under usermaps\. If the installer can't find Black Ops III, add
   -GameDir "C:\...\Call of Duty Black Ops III". Add -Bake to also bake the cfg into the map.
3. Add this line at the top of main() in your map's client script (scripts\zm\<mapname>.csc), then relink:

       LuiLoad("ui.t7.utility.weapon_tech_loader");

   The installer has already copied the loader Lua and added its rawfile line to your .zone file.
4. Run the map, then read weapon_tech.log next to BlackOps3.exe. The first line says
   "weapon_tech $version loaded from ..."; it then lists the cfg it read and what it installed.

The installer never overwrites an existing zone\weapon_tech.cfg, and running it twice changes nothing.

Next
----
docs\QUICKSTART.md       turn on each feature for one gun
docs\CONFIG_REFERENCE.md every key
README.md                overview, shipping your map, troubleshooting

By hand (no installer)
----------------------
Copy weapon_tech.dll and kit\weapon_tech.cfg to usermaps\<mapname>\zone\, copy
kit\ui\t7\utility\weapon_tech_loader.lua to usermaps\<mapname>\ui\t7\utility\ and set MAP_NAME in it, add
"rawfile,ui/t7/utility/weapon_tech_loader.lua" to zone_source\<mapname>.zone, and add the LuiLoad line above.
"@
[IO.File]::WriteAllText((Join-Path $stage 'INSTALL.txt'), ($install -replace "`r?`n", "`r`n"), (New-Object System.Text.UTF8Encoding($false)))

$zip = Join-Path $root "dist\$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::Open($zip, 'Create')
try {
    $base = (Resolve-Path (Join-Path $root 'dist\stage')).Path.TrimEnd('\') + '\'
    foreach ($f in Get-ChildItem $stage -Recurse -File) {
        $entry = $f.FullName.Substring($base.Length).Replace('\', '/')
        [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $f.FullName, $entry, [IO.Compression.CompressionLevel]::Optimal)
    }
} finally { $archive.Dispose() }
Remove-Item (Join-Path $root 'dist\stage') -Recurse -Force

Write-Host "wrote $zip ($([math]::Round((Get-Item $zip).Length / 1KB)) KB)"
$read = [IO.Compression.ZipFile]::OpenRead($zip)
try { $read.Entries | ForEach-Object { Write-Host ("  " + $_.FullName) } } finally { $read.Dispose() }
