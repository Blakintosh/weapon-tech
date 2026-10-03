@echo off
rem Wrapper for build.ps1 (finds Visual Studio itself). Usage: build.bat [-Out <dir>] [-Extras]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0build.ps1" %*
