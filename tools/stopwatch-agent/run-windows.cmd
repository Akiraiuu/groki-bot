@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"
if exist ".venv\Scripts\python.exe" goto existing
py -3 -c "import sys; sys.exit(sys.version_info < (3, 11))" >nul 2>&1
if not errorlevel 1 goto use_launcher
python -c "import sys; sys.exit(sys.version_info < (3, 11))" >nul 2>&1
if not errorlevel 1 goto use_python
echo Python 3.11 or newer was not found using py or python.
echo Install Python from https://www.python.org/downloads/windows/
echo Then close this window and run this file again.
pause
exit /b 1
:use_launcher
set "probe_bootstrap=py -3"
goto bootstrap
:use_python
set "probe_bootstrap=python"
:bootstrap
if "%~1"=="--check-python" exit /b 0
%probe_bootstrap% -m venv .venv
if errorlevel 1 goto failed
:existing
".venv\Scripts\python.exe" -c "import sys; sys.exit(sys.version_info < (3, 11))" >nul 2>&1
if errorlevel 1 goto failed
if "%~1"=="--check-python" exit /b 0
".venv\Scripts\python.exe" -m pip install -r requirements.txt
if errorlevel 1 goto failed
".venv\Scripts\python.exe" verify_agent.py --session-only --auth key
set "probe_exit=%errorlevel%"
echo.
echo Finished. Exit code: %probe_exit%
echo Diagnostic mode: no response audio was requested.
echo Open the latest results folder and send report.json for review.
pause
exit /b %probe_exit%
:failed
echo Setup failed. No API credentials were requested.
pause
exit /b 1
