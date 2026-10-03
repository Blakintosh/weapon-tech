<#
Builds weapon_tech.dll from src\ with MSVC (x64). Finds Visual Studio through vswhere (the "Visual Studio\Installer"
folder), runs its vcvars64.bat, and compiles with the same command the project always used:
    cl /nologo /O2 /MT /EHsc /std:c++17 /LD weapon_tech.cpp /Fe:weapon_tech.dll /link kernel32.lib

  .\build.ps1                 -> build\weapon_tech.dll (+ build\wt_cfgdump.exe, the offline cfg checker)
  .\build.ps1 -Out C:\x       -> C:\x\weapon_tech.dll
  .\build.ps1 -Extras         -> also compiles extras\arxan\arxan.cpp (compile check only)
Set VCVARS to the full path of a vcvars64.bat to skip the search.
#>
param([string]$Out = (Join-Path $PSScriptRoot 'build'), [switch]$Extras)
$ErrorActionPreference = 'Stop'

function Find-VcVars {
    if ($env:VCVARS -and (Test-Path $env:VCVARS)) { return $env:VCVARS }
    $installer = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'
    $vswhere = Join-Path $installer 'vswhere.exe'
    if (Test-Path $vswhere) {
        $root = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($root) {
            $p = Join-Path $root 'VC\Auxiliary\Build\vcvars64.bat'
            if (Test-Path $p) { return $p }
        }
    }
    foreach ($pf in @($env:ProgramFiles, ${env:ProgramFiles(x86)})) {
        $hit = Get-ChildItem (Join-Path $pf 'Microsoft Visual Studio') -Filter vcvars64.bat -Recurse -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    throw 'vcvars64.bat not found: install Visual Studio with the "Desktop development with C++" workload, or set VCVARS.'
}

$vcvars = Find-VcVars
# vcvars64.bat calls vswhere, which lives in the Installer folder
$env:PATH += ';' + (Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer')
Write-Host "vcvars: $vcvars"
New-Item -ItemType Directory -Force $Out | Out-Null
$obj = Join-Path $Out 'obj'
New-Item -ItemType Directory -Force $obj | Out-Null
$src = Join-Path $PSScriptRoot 'src'
$objFwd = $obj.Replace([string][char]92, '/') + '/'  # cl wants a trailing slash; a backslash would escape the closing quote

$cmd = "`"$vcvars`" >nul && cd /d `"$src`" && " +
       "rc /nologo /fo `"$obj\weapon_tech.res`" weapon_tech.rc && " +
       "cl /nologo /O2 /MT /EHsc /std:c++17 /LD weapon_tech.cpp `"$obj\weapon_tech.res`" /Fo`"$objFwd`" /Fe:`"$Out\weapon_tech.dll`" /link kernel32.lib && " +
       "cl /nologo /O2 /MT /EHsc /std:c++17 wt_cfgdump.cpp /Fo`"$objFwd`" /Fe:`"$Out\wt_cfgdump.exe`" /link kernel32.lib"
if ($Extras) {
    $ax = Join-Path $PSScriptRoot 'extras\arxan'
    $cmd += " && cd /d `"$ax`" && cl /nologo /O2 /MT /EHsc /std:c++17 /W4 /c arxan.cpp /Fo`"$obj\arxan.obj`""
}
cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) { throw "build failed ($LASTEXITCODE)" }
Remove-Item (Join-Path $Out 'weapon_tech.lib'), (Join-Path $Out 'weapon_tech.exp') -ErrorAction SilentlyContinue
Write-Host "built $Out\weapon_tech.dll and $Out\wt_cfgdump.exe"
