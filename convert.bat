@echo off
rem GT2 PSP converter - Windows (double-click). Starts the local web app and opens it in your browser.
cd /d "%~dp0"
if not exist input mkdir input
where py >nul 2>nul && (py -3 converter\server.py & goto :end)
where python >nul 2>nul && (python converter\server.py & goto :end)
echo Python 3 is needed. Install it from https://www.python.org/downloads/windows/
echo (tick "Add python.exe to PATH") and start this again.
pause
:end
