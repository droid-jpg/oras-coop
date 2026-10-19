@echo off
rem Removes the oras-coop plugin from Azahar (keeps your settings file).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" -Uninstall %*
pause
