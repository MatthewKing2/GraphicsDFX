param([switch]$SkipBuild)
$ErrorActionPreference='Stop'
$project=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if(-not $SkipBuild) {
    & make -C $project control-room-test VULKAN_SDK=C:/VulkanSDK/1.4.341.1
    if($LASTEXITCODE -ne 0) {throw 'Control room build/tests failed'}
}
if(-not ('RoomTestWindow' -as [type])) {Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class RoomTestWindow {
 public delegate bool EnumProc(IntPtr w,IntPtr state);
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback,IntPtr state);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr w,out uint id);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr w,int command);
 public static void Hide(uint pid) {
  EnumWindows((w,s)=>{uint id;GetWindowThreadProcessId(w,out id);if(id==pid)ShowWindow(w,0);return true;},IntPtr.Zero);
 }
}
'@}
function Check-Room([string]$name,[string[]]$arguments) {
    $out=Join-Path $project "build/room-$name-output.log"
    $err=Join-Path $project "build/room-$name-errors.log"
    $p=Start-Process -FilePath (Join-Path $project 'build/control_room_cpu.exe') -WorkingDirectory $project -ArgumentList $arguments -WindowStyle Hidden -PassThru -RedirectStandardOutput $out -RedirectStandardError $err
    $handle=$p.Handle
    $watch=[Diagnostics.Stopwatch]::StartNew()
    while(-not $p.HasExited) {
        [RoomTestWindow]::Hide($p.Id)
        if($watch.Elapsed.TotalSeconds -gt 120) {Stop-Process -Id $p.Id;throw "Timeout: $name"}
        Start-Sleep -Milliseconds 50
    }
    $p.WaitForExit()
    Get-Content -LiteralPath $out
    if($p.ExitCode -ne 0 -or (Select-String -LiteralPath $err -Pattern 'Validation Error|SYNC-HAZARD|Control room error')) {
        Get-Content -LiteralPath $err;throw "Failed: $name"
    }
    Write-Host "PASS $name"
}
$oldSync=$env:VK_LAYER_VALIDATE_SYNC
try {
    $env:VK_LAYER_VALIDATE_SYNC='1'
    Check-Room 'overview' @('--room-view','--frames','90','--profile','--capture','build/control-room.bmp')
    Check-Room 'console' @('--panel-view','--frames','300','--profile','--capture','build/control-panel.bmp')
    Check-Room 'alternate-layout' @('--panel-view','--reactors','1','--rods','8','--banks','3','--plugs','4','--segments','64','--trials','16','--frames','90','--capture','build/control-layout.bmp')
} finally {$env:VK_LAYER_VALIDATE_SYNC=$oldSync}
