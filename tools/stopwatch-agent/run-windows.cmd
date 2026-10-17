@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"
py -3 --version >nul 2>&1
if errorlevel 1 (
  echo Python 3 is required. Install Python 3.11 or newer from python.org.
  pause
  exit /b 1
)
if not exist ".venv\Scripts\python.exe" (
  py -3 -m venv .venv
  if errorlevel 1 goto failed
)
".venv\Scripts\python.exe" -m pip install -r requirements.txt
if errorlevel 1 goto failed
".venv\Scripts\python.exe" verify_agent.py --play
set "probe_exit=%errorlevel%"
echo.
echo Finished. Exit code: %probe_exit%
echo Open the results folder for report.json and reply WAV files.
pause
exit /b %probe_exit%
:failed
echo Setup failed. No API credentials were requested.
pause
exit /b 1
