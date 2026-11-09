@echo off
cd /d "%~dp0"
make game
if errorlevel 1 (
    pause
    exit /b 1
)
build\survival_game.exe %*
if errorlevel 1 pause
