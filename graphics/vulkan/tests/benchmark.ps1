param(
    [switch]$SkipBuild,
    [switch]$BaselineOnly,
    [int]$BaselineFrames = 60,
    [int]$SweepFrames = 8,
    [int]$MaxParticles = 1048576
)
$ErrorActionPreference = 'Stop'
$project = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
if (-not $SkipBuild) {
    & make -C $project all build/nbody_benchmark.exe
    if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
}
if ($BaselineFrames -lt 1 -or $SweepFrames -lt 1 -or $MaxParticles -lt 1500) {
    throw 'Invalid benchmark limits.'
}
$runDirectory = Join-Path $project ('build/benchmark-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,6))
New-Item -ItemType Directory -Path $runDirectory | Out-Null
Get-CimInstance Win32_Processor | Select-Object Name, NumberOfCores, NumberOfLogicalProcessors |
    ConvertTo-Json | Out-File (Join-Path $runDirectory 'cpu.json')
Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion |
    ConvertTo-Json | Out-File (Join-Path $runDirectory 'gpus.json')

# Hide only the window belonging to the process we just launched. Do not minimize
# it: minimization can stop swapchain rendering and invalidate FPS measurements.
if (-not ('BenchmarkWindow' -as [type])) { Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class BenchmarkWindow {
    public delegate bool EnumProc(IntPtr window, IntPtr state);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback, IntPtr state);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr window, int command);
    public static IntPtr Find(uint processId) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((window, state) => { uint id; GetWindowThreadProcessId(window, out id);
            if (id == processId) { found = window; return false; } return true; }, IntPtr.Zero);
        return found;
    }
}
'@
}
$script:sequence = 0
$script:results = [Collections.Generic.List[object]]::new()
function Measure-Case([string]$Mode, [int]$Count, [int]$Frames, [int]$Warmup = 10) {
    $script:sequence++
    $label = '{0:D3}-{1}-{2}' -f $script:sequence,$Mode,$Count
    $stdout = Join-Path $runDirectory ($label + '.json.log')
    $stderr = Join-Path $runDirectory ($label + '.errors.log')
    $arguments = @('--benchmark','--mode',$Mode,'--particles',$Count,'--theta','0.5',
        '--frames',$Frames,'--warmup',$Warmup)
    $launch = @{
        FilePath = Join-Path $project 'build/nbody_benchmark.exe'
        ArgumentList = $arguments; WorkingDirectory = $project; WindowStyle = 'Hidden'
        RedirectStandardOutput = $stdout; RedirectStandardError = $stderr; PassThru = $true
    }
    $process = Start-Process @launch
    $processHandle = $process.Handle # Keep the handle so ExitCode remains available.
    if (-not $process.HasExited) {
        $process.ProcessorAffinity = [IntPtr]1 # One logical CPU, same for all modes.
        $process.PriorityClass = [Diagnostics.ProcessPriorityClass]::AboveNormal
    }
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $hidden = $false
    while (-not $process.HasExited) {
        if (-not $hidden) {
            $window = [BenchmarkWindow]::Find($process.Id)
            if ($window -ne [IntPtr]::Zero) { [BenchmarkWindow]::ShowWindow($window,0) | Out-Null; $hidden = $true }
        }
        if ($clock.Elapsed.TotalSeconds -gt 120) {
            Stop-Process -Id $process.Id
            throw "Benchmark timed out: $label"
        }
        Start-Sleep -Milliseconds 50
    }
    $process.WaitForExit()
    if ($process.ExitCode -ne 0) { throw "Benchmark failed: $label - $(Get-Content -LiteralPath $stderr -Raw)" }
    $json = Get-Content -LiteralPath $stdout | Where-Object { $_.StartsWith('{') } | Select-Object -Last 1
    if (-not $json) { throw "No benchmark result: $label" }
    $result = $json | ConvertFrom-Json
    $script:results.Add($result)
    $script:results | Export-Csv -NoTypeInformation (Join-Path $runDirectory 'measurements.csv')
    Write-Host ('{0,-7} N={1,8} {2,9:N3} ms/frame {3,8:N2} FPS (CPU force {4:N3}, GPU build {5:N3}, GPU force {6:N3}, render {7:N3} ms)' -f
        $Mode,$Count,$result.frame_ms,$result.fps,$result.cpu_force_ms,$result.gpu_build_ms,$result.gpu_force_ms,$result.gpu_render_ms)
    return $result
}

$modes = @('cpu-all','cpu-bh','gpu-all','gpu-bh')
$baselines = @{}
foreach ($mode in $modes) { $baselines[$mode] = Measure-Case $mode 1500 $BaselineFrames 10 }
$baselineResults = foreach ($mode in $modes) { $baselines[$mode] }
$baselineResults | Export-Csv -NoTypeInformation (Join-Path $runDirectory 'baseline.csv')
$limits = [Collections.Generic.List[object]]::new()
if (-not $BaselineOnly) {
    foreach ($mode in $modes) {
        $low = 1500; $lowResult = $baselines[$mode]; $high = $low
        # Coarse geometric search, followed by a ~2% resolution bracket.
        while ($lowResult.frame_ms -lt 200 -and $high -lt $MaxParticles) {
            $high = [Math]::Min($MaxParticles, $low * 2)
            $highResult = Measure-Case $mode $high $SweepFrames
            if ($highResult.frame_ms -ge 200) { break }
            $low = $high; $lowResult = $highResult
        }
        if ($low -eq $MaxParticles -and $lowResult.frame_ms -lt 200) {
            $limits.Add([pscustomobject]@{mode=$mode; lower_n=$low; upper_n=$null; lower_fps=$lowResult.fps; upper_fps=$null; status='search cap, not 5 FPS limit'})
            continue
        }
        for ($refine = 0; $refine -lt 6 -and $high - $low -gt [Math]::Max(32,$low * 0.02); $refine++) {
            $middle = [int](($low + $high) / 2)
            $middleResult = Measure-Case $mode $middle $SweepFrames
            if ($middleResult.frame_ms -lt 200) { $low = $middle; $lowResult = $middleResult }
            else { $high = $middle; $highResult = $middleResult }
        }
        # Longer measurements at both edges; report the measured bracket, not an
        # extrapolated universal maximum. Thermals and scene distribution matter.
        $lowResult = Measure-Case $mode $low 30 10
        $highResult = Measure-Case $mode $high 30 10
        # Re-bracket when sustained clocks differ from the shorter probe runs.
        for ($retry = 0; $retry -lt 8 -and $lowResult.frame_ms -ge 200; $retry++) {
            $high = $low; $highResult = $lowResult
            $low = [Math]::Max(1500, [int]($low * 0.85))
            $lowResult = Measure-Case $mode $low 30 10
        }
        for ($retry = 0; $retry -lt 8 -and $highResult.frame_ms -lt 200 -and $high -lt $MaxParticles; $retry++) {
            $low = $high; $lowResult = $highResult
            $high = [Math]::Min($MaxParticles, [int]($high * 1.15))
            $highResult = Measure-Case $mode $high 30 10
        }
        for ($retry = 0; $retry -lt 5 -and $high - $low -gt $low * 0.04; $retry++) {
            $middle = [int](($low + $high) / 2)
            $middleResult = Measure-Case $mode $middle 30 10
            if ($middleResult.frame_ms -lt 200) { $low = $middle; $lowResult = $middleResult }
            else { $high = $middle; $highResult = $middleResult }
        }
        $limits.Add([pscustomobject]@{mode=$mode; lower_n=$low; upper_n=$high; lower_fps=$lowResult.fps; upper_fps=$highResult.fps; status='measured near-5-FPS bracket'})
        $limits | Export-Csv -NoTypeInformation (Join-Path $runDirectory 'limits.csv')
    }
}
Write-Host "Results: $runDirectory"
