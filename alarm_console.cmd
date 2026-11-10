@echo off
setlocal EnableExtensions
cd /d "%~dp0"
chcp 65001 >nul
title ESP32 Clock Console
set "PYTHONIOENCODING=utf-8"
set "PORT=8765"

netstat -ano | findstr /R /C:":%PORT% .*LISTENING" >nul 2>nul
if not errorlevel 1 goto wait_serial

echo [PC] 正在启动时钟服务...
start "ESP32 Clock Service" /min "%~dp0start_clock.cmd"

set /a tries=0
:wait
timeout /t 1 /nobreak >nul 2>nul
netstat -ano | findstr /R /C:":%PORT% .*LISTENING" >nul 2>nul
if not errorlevel 1 goto wait_serial
set /a tries+=1
if %tries% GEQ 20 goto failed
goto wait

:wait_serial
set /a tries=0
:serial_probe
call "%~dp0alarm.cmd" "ALARM LIST" > "%TEMP%\esp32_clock_probe.txt" 2>&1
findstr /C:"OK ALARM" "%TEMP%\esp32_clock_probe.txt" >nul 2>nul
if not errorlevel 1 goto ready
set /a tries+=1
if %tries% GEQ 4 goto serial_warn
timeout /t 1 /nobreak >nul 2>nul
goto serial_probe

:serial_warn
echo.
echo [提示] 控制端口已打开，但还没检测到 ESP32 串口。
echo        现在输入命令会返回 ERR PC_CLOCK not connected。
echo        插好 ESP32 后服务会自动重扫；不用重启本控制台。
echo.

:failed
echo.
echo [ERR] 20 秒内没有启动成功。
echo 请检查：
echo   1. ESP32 是否插好，COM 口是否被串口监视器占用
echo   2. 双击 start_clock.cmd 查看具体错误
echo.
pause
exit /b 1

:ready
echo.
echo ==========================================
echo   ESP32 时钟闹钟控制台
echo   Type a command below; no python or .py needed.
echo.
echo   list
echo   add 7:30 daily
echo   add 6:45 daily bird 80 20
echo   close alarm1
echo   open alarm1
echo   delete alarm1
echo   test alarm1
echo   stop
echo.
echo   You may also prefix a command with "alarm".
echo.
echo   输入 exit 退出
echo ==========================================
echo.

:prompt
set "CMD="
set /p "CMD=alarm> "
if not defined CMD goto done
if /i "%CMD%"=="exit" goto done
if /i "%CMD%"=="quit" goto done
if "%CMD%"=="退出" goto done
call "%~dp0alarm.cmd" "%CMD%"
echo.
goto prompt

:done
echo.
echo 已退出控制台。时钟服务仍在后台运行。
exit /b 0
