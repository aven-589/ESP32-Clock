@echo off
setlocal
cd /d "%~dp0"
chcp 65001 >nul
title ESP32 PC Clock Alarm
set "PYTHONIOENCODING=utf-8"

set "PY="
python -c "import sys" >nul 2>nul
if not errorlevel 1 set "PY=python"
if not defined PY (
  py -3 -c "import sys" >nul 2>nul
  if not errorlevel 1 set "PY=py -3"
)
if not defined PY (
  echo [ERROR] Python 3 not found. Install Python 3 first.
  pause
  exit /b 1
)

if "%~1"=="" (
  %PY% alarm_cli.py
  pause
  exit /b %errorlevel%
)

%PY% alarm_cli.py %*
exit /b %errorlevel%
