@echo off
setlocal
cd /d "%~dp0"
chcp 65001 >nul
title ESP32 PC Clock
set "PYTHONIOENCODING=utf-8"

set "PY="
python -c "import sys" >nul 2>nul
if not errorlevel 1 set "PY=python"
if not defined PY (
  py -3 -c "import sys" >nul 2>nul
  if not errorlevel 1 set "PY=py -3"
)
if not defined PY (
  echo [ERROR] Python 3 not found. Please install Python 3 first.
  pause
  exit /b 1
)

%PY% -c "import serial" >nul 2>nul
if errorlevel 1 (
  echo [PC] Installing pyserial...
  %PY% -m pip install -r requirements.txt
  if errorlevel 1 (
    echo [ERROR] Failed to install pyserial. Check your network.
    pause
    exit /b 1
  )
)

echo [PC] Starting ESP32 PC Clock...
echo [PC] Auto-scanning serial ports; press Ctrl+C to stop.
echo [PC] Alarm control: alarm 查看闹钟
%PY% pc_clock.py %*

echo.
echo [PC] Stopped.
pause
