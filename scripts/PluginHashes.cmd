@echo off
rem Double-click from the Soulash 2 folder: lists the plugin hashes and asks
rem before writing anything into SoulashExtender.ini.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0plugin-hashes.ps1" -Prompt %*
echo.
pause
