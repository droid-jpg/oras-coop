@echo off
rem Runs the oras-coop relay server (needs Node.js 18+ from https://nodejs.org).
rem Only one player needs to run this; everyone else enters this PC's address when installing.
where node >nul 2>nul || (echo Node.js is not installed. Get it from https://nodejs.org & pause & exit /b 1)
node "%~dp0server\server.mjs" %*
pause
