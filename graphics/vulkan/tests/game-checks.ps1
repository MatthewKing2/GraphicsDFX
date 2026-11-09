param([switch]$SkipBuild,[switch]$Benchmark,[switch]$NBody,[switch]$Profile)
$ErrorActionPreference='Stop'
$project=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if(-not $SkipBuild) {
    & make -C $project game
    if($LASTEXITCODE -ne 0) {throw 'Game build failed'}
    if($NBody) {
        & make -C $project shaders build/nbody_test.exe
        if($LASTEXITCODE -ne 0) {throw 'N-body test build failed'}
    }
}
if(-not ('GameTestWindow' -as [type])) {Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class GameTestWindow {
 public delegate bool EnumProc(IntPtr w,IntPtr state);
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback,IntPtr state);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr w,out uint id);
 [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr w,int command);
 public static void Hide(uint processId) {
  EnumWindows((w,s)=>{uint id;GetWindowThreadProcessId(w,out id);if(id==processId)ShowWindow(w,0);return true;},IntPtr.Zero);
 }
}
'@}
function Run-GameCheck([string]$Name,[string[]]$GameArguments,[string]$Executable='survival_game.exe') {
    $out=Join-Path $project "build/game-$Name-output.log"
    $err=Join-Path $project "build/game-$Name-errors.log"
    $launch=@{FilePath=Join-Path $project "build/$Executable";WorkingDirectory=$project;
        WindowStyle='Hidden';RedirectStandardOutput=$out;RedirectStandardError=$err;PassThru=$true}
    if($GameArguments.Count) {$launch.ArgumentList=$GameArguments}
    $process=Start-Process @launch
    $handle=$process.Handle
    $timer=[Diagnostics.Stopwatch]::StartNew()
    while(-not $process.HasExited) {
        [GameTestWindow]::Hide($process.Id)
        if($timer.Elapsed.TotalSeconds -gt 120) {Stop-Process -Id $process.Id;throw "Timed out: $Name"}
        Start-Sleep -Milliseconds 50
    }
    $process.WaitForExit()
    Get-Content -LiteralPath $out
    if($process.ExitCode -ne 0) {Get-Content -LiteralPath $err;throw "Failed: $Name"}
    if(Select-String -LiteralPath $err -Pattern 'Validation Error|SYNC-HAZARD|Game error:') {
        throw "Validation errors: $err"
    }
    Write-Host "PASS $Name (exit 0, no Vulkan validation errors)"
}
$oldSync=$env:VK_LAYER_VALIDATE_SYNC
try {
    $env:VK_LAYER_VALIDATE_SYNC='1'
    Run-GameCheck 'validation' @('--validate')
    Run-GameCheck 'gpu-smoke' @('--smoke','--backend','gpu','--capture-frame','150','--capture','build/game-fork.bmp')
    Run-GameCheck 'cpu-smoke' @('--smoke','--backend','cpu')
    Run-GameCheck 'four-way' @('--smoke','--frames','460','--capture-frame','450','--capture','build/game-fourway.bmp')
    Run-GameCheck 'inside' @('--smoke','--frames','235','--capture-frame','225','--capture','build/game-inside.bmp')
    Run-GameCheck 'exit' @('--smoke','--frames','325','--capture-frame','315','--capture','build/game-exit.bmp')
    Run-GameCheck 'title' @('--frames','1','--seed','12345','--capture-frame','0','--capture','build/game-title.bmp')
    if($NBody) {Run-GameCheck 'nbody-regression' @() 'nbody_test.exe'}
    if($Benchmark) {
        & make -C $project build/survival_benchmark.exe
        if($LASTEXITCODE -ne 0) {throw 'Benchmark build failed'}
        Run-GameCheck 'benchmark' @('--benchmark','--output','build/reactor-benchmark.csv') 'survival_benchmark.exe'
        Run-GameCheck 'frozen-benchmark' @('--benchmark','--freeze-reactor','--output','build/reactor-frozen.csv') 'survival_benchmark.exe'
    }
    if($Profile) {
        & make -C $project build/survival_benchmark.exe
        if($LASTEXITCODE -ne 0) {throw 'Profile build failed'}
        foreach($mode in @('off','gpu','cpu')) {
            $arguments=@('--autoplay','--seed','12345','--frames','600','--profile',
                '--profile-output',"build/reactor-profile-$mode.csv")
            if($mode -eq 'off') {$arguments+='--no-prediction'}
            else {$arguments+=@('--backend',$mode)}
            Run-GameCheck "profile-$mode" $arguments 'survival_benchmark.exe'
        }
    }
} finally {$env:VK_LAYER_VALIDATE_SYNC=$oldSync}
