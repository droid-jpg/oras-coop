@echo off
rem Removes the oras-mmo plugin from Azahar (keeps your settings file).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Uninstall %*
pause
