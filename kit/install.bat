@echo off
rem Installs weapon_tech into a BO3 usermap. Usage: install.bat <mapname> [-GameDir <path>] [-Bake] [-Force]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
exit /b %ERRORLEVEL%
