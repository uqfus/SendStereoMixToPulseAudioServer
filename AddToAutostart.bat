@echo off
setlocal

set "APPNAME=SendStereoMixToPulseAudioServer.exe"
set "APPEXE=%~dp0%APPNAME%"
set "REGISTERYRUNKEY=HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run"

if /i "%~1"=="/?"      goto :usage
if /i "%~1"=="/h"      goto :usage
if /i "%~1"=="-h"      goto :usage
if /i "%~1"=="--help"  goto :usage
if /i "%~1"=="-s"      goto :show
if /i "%~1"=="--show"  goto :show
if /i "%~1"=="-r"      goto :remove
if /i "%~1"=="--remove" goto :remove

if "%~1"=="" goto :noparam

if not exist "%APPEXE%" goto :noexe

set "SERVER=%~1"
echo [INFO] Pinging server: %SERVER%
ping -n 1 -w 1000 %SERVER% >nul 2>&1
if errorlevel 1 goto :badping

set "REGDATA=\"%APPEXE%\" %SERVER%"

reg add "%REGISTERYRUNKEY%" /f /v "%APPNAME%" /t REG_SZ /d "%REGDATA%" >nul
if errorlevel 1 goto :regerror

echo [OK] Autostart entry:
echo      "%APPEXE%" %SERVER%
exit /b 0

:noparam
echo [ERROR] Server name or IP address is missing. 1>&2
rem show usage

:usage
echo Usage:
echo   %~nx0 [server-name-or-IP]  - add or update the autostart entry
echo   %~nx0 -s --show            - show the existing registry entry
echo   %~nx0 -r --remove          - delete the autostart entry
echo   %~nx0 /? /h -h --help      - this help
echo.
echo   Server name or IP address is required.
echo.
echo   Examples: %~nx0 cubieboard2.local
echo             %~nx0 192.168.1.50

choice.exe /M "Perform mDNS query for available PulseAudio sound servers"
if "%ERRORLEVEL%" == "1" (mdns.exe --query _pulse-server._tcp.local.)
exit /b 0

:show
set "SAVED="
for /f "tokens=1,2,*" %%a in ('reg query "%REGISTERYRUNKEY%" /v "%APPNAME%" 2^>nul ^| findstr /i /c:"REG_"') do set "SAVED=%%c"
if not defined SAVED (echo [OK] No autostart entry found.) else (echo [OK] Autostart entry:&echo      %SAVED%)
exit /b 0

:remove
reg delete "%REGISTERYRUNKEY%" /f /v "%APPNAME%" >nul 2>&1
if errorlevel 1 (echo [INFO] No autostart entry found.) else (echo [OK] Autostart entry removed.)
exit /b 0

:noexe
echo [ERROR] File not found: "%APPEXE%" 1>&2
echo         Run the script from the folder that contains %APPNAME% 1>&2
exit /b 3

:badping
echo [ERROR] Ping test failed for server name: "%SERVER%" 1>&2
exit /b 2

:regerror
echo [ERROR] "reg add" failed, exit code %ERRORLEVEL% 1>&2
exit /b 1
