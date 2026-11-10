@echo off
cd /d "%~dp0"
python lab.py
if errorlevel 1 pause
