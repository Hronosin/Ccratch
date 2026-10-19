@echo off
rem Starts Ccratch on Windows (in WSL, see build.cmd), building it first if
rem needed. Drop a .c file onto run.cmd to open it.
rem
rem   run.cmd              the start screen
rem   run.cmd game.c       with a file open
setlocal
cd /d "%~dp0"

where wsl >nul 2>nul
if errorlevel 1 goto nowsl
wsl -e true >nul 2>nul
if errorlevel 1 goto nowsl

rem Windows paths (C:\...) become WSL ones in run.sh
wsl --cd "%~dp0." -- bash ./run.sh %*
if errorlevel 1 (
    echo.
    echo Ccratch did not start: the messages above say why.
    pause
    exit /b 1
)
exit /b 0

:nowsl
echo Ccratch runs in WSL (Linux inside Windows), and WSL is not set up yet.
echo.
echo   1. Open PowerShell as administrator and run:   wsl --install
echo   2. Restart the computer, open "Ubuntu" once and pick a user name.
echo   3. Double-click run.cmd again.
echo.
pause
exit /b 1
