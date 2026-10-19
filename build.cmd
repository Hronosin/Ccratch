@echo off
rem Builds Ccratch on Windows. Ccratch is a Linux program (it talks to the
rem compiler, gdb and git the Unix way), so on Windows it lives in WSL:
rem this runs build.sh there. Windows 11 (or 10 with WSLg) shows its window
rem like any other.
rem
rem   build.cmd             the normal build
rem   build.cmd debug       or: paranoid, clean, deps
setlocal
cd /d "%~dp0"

where wsl >nul 2>nul
if errorlevel 1 goto nowsl
wsl -e true >nul 2>nul
if errorlevel 1 goto nowsl

wsl --cd "%~dp0." -- bash ./build.sh %*
if errorlevel 1 (
    echo.
    echo The build did not work: the messages above say why.
    echo If tools are missing, "build.cmd deps" installs them inside WSL.
    pause
    exit /b 1
)
echo.
echo Built. Start it with run.cmd
pause
exit /b 0

:nowsl
echo Ccratch runs in WSL (Linux inside Windows), and WSL is not set up yet.
echo.
echo   1. Open PowerShell as administrator and run:   wsl --install
echo   2. Restart the computer, open "Ubuntu" once and pick a user name.
echo   3. Double-click build.cmd again.
echo.
pause
exit /b 1
