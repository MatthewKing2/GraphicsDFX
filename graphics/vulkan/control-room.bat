@echo off
cd /d "%~dp0"
make control-room VULKAN_SDK=C:/VulkanSDK/1.4.341.1
if errorlevel 1 (
    pause
    exit /b 1
)
build\control_room_cpu.exe %*
if errorlevel 1 pause
