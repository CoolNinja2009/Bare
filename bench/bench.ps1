# bench.ps1: the Fluids example in Bare against the same simulation in Microsoft Edge.
# Method and results: bench/README.md. Keep the mouse off the screen while it runs.
#
#   cmake -S . -B build-rel -G Ninja -DCMAKE_BUILD_TYPE=Release -DBARE_BUILD_BENCH=ON
#   cmake --build build-rel
#   powershell -File bench\bench.ps1 [all|start|base|bare|edge|edgestir] [-Build build-rel] [-Sample <dir>]
param(
    [string]$Which = "all",
    [string]$Build = "build-rel",
    # The WebGL original as packaged for Lively Wallpaper (index.html, js/, LivelyProperties.json).
    [string]$Sample = "$PSScriptRoot\..\..\Wallpaper+\ScreenPlus\samples\0wj1biqk.f41"
)
$ErrorActionPreference = "Stop"
$Root = Resolve-Path "$PSScriptRoot\.."
$Bin = if ([IO.Path]::IsPathRooted($Build)) { "$Build\bin" } else { "$Root\$Build\bin" }
$Bare = "$Bin\fluid.exe"          # settles and sleeps: the idle case
$Bench = "$Bin\fluid_bench.exe"   # stirred every frame: the active case
$Edge = "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe"
$Work = "$env:TEMP\bare-bench"
$Profile = "$Work\edge-profile"
$Cores = [Environment]::ProcessorCount

# ---- the browser side: a copy of the sample with Lively's defaults applied ----
function Prepare-Web {
    if (-not (Test-Path "$Sample\index.html")) { throw "Sample not found: $Sample (pass -Sample)" }
    $web = "$Work\web"
    if (Test-Path $web) { Remove-Item -Recurse -Force $web }
    New-Item -ItemType Directory -Force $web | Out-Null
    Copy-Item -Recurse "$Sample\*" $web
    # Lively calls livelyPropertyListener(name, value) for each property on load.
    $props = Get-Content "$Sample\LivelyProperties.json" -Raw | ConvertFrom-Json
    $calls = foreach ($p in $props.PSObject.Properties) {
        if ($p.Name -ne "imgSelect") { "livelyPropertyListener($($p.Name | ConvertTo-Json), $($p.Value.value | ConvertTo-Json));" }
    }
    $script = @"
<script>
/* bench: Lively's defaults, as Lively applies them on load */
$($calls -join "`n")
/* bench: title "ready" once frames are being presented, then "fps N" each second */
(function () { let n = 0, t = 0;
  function count(now) { n++; if (now - t >= 1000) { document.title = "fps " + n; n = 0; t = now; } requestAnimationFrame(count); }
  requestAnimationFrame(() => requestAnimationFrame((now) => { document.title = "ready"; t = now; requestAnimationFrame(count); })); })();
/* bench: with ?stir, move the mouse each frame along the same path as fluid_bench */
if (location.search.includes("stir")) { let f = 0; const c = document.getElementsByTagName("canvas")[0];
  (function go() { f++; const a = f * 0.05;
    c.dispatchEvent(new MouseEvent("mousemove", { clientX: 640 + 300 * Math.cos(a), clientY: 360 + 200 * Math.sin(a * 1.3), bubbles: true }));
    requestAnimationFrame(go); })(); }
</script>
"@
    $html = Get-Content "$web\index.html" -Raw
    $i = $html.LastIndexOf("</body>")
    Set-Content "$web\index.html" ($html.Substring(0, $i) + $script + $html.Substring($i)) -Encoding utf8
    "file:///" + ("$web\index.html" -replace '\\', '/')
}

function Edge-Args([string]$Url) {
    @("--user-data-dir=$Profile", "--no-first-run", "--no-default-browser-check", "--app=$Url",
      "--window-size=1280,720", "--window-position=100,100")
}

Add-Type -Namespace BareBench -Name Win -MemberDefinition @"
[DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
"@
# Keep a window above everything while it's measured (no focus or mouse change);
# a covered Edge window stops drawing and would measure as free.
function Pin([IntPtr]$h) { [BareBench.Win]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x0013) | Out-Null }

# The Edge instance using our profile: its browser process and every descendant (GPU, renderer, utility).
function Get-EdgePids {
    $all = Get-CimInstance Win32_Process -Filter "Name='msedge.exe'"
    $set = @{}
    foreach ($p in $all) { if ($p.CommandLine -like "*$Profile*") { $set[[int]$p.ProcessId] = 1 } }
    do {
        $n = $set.Count
        foreach ($p in $all) { if ($set.ContainsKey([int]$p.ParentProcessId)) { $set[[int]$p.ProcessId] = 1 } }
    } while ($set.Count -ne $n)
    [int[]]$set.Keys
}
function Stop-Edge { Get-EdgePids | ForEach-Object { try { Stop-Process -Id $_ -Force -ErrorAction Stop } catch {} }; Start-Sleep -Milliseconds 800 }
function Get-Title([int[]]$pids) {
    foreach ($id in $pids) { $p = Get-Process -Id $id -ErrorAction SilentlyContinue; if ($p -and $p.MainWindowTitle) { return $p.MainWindowTitle } }
    ""
}
function Start-Edge([string]$Url) {
    Stop-Edge
    Start-Process $Edge -ArgumentList (Edge-Args $Url) | Out-Null; Start-Sleep 3
    foreach ($id in Get-EdgePids) { $q = Get-Process -Id $id; if ($q.MainWindowHandle -ne 0) { Pin $q.MainWindowHandle } }
    Start-Sleep 5
}

# CPU, RAM and GPU of a set of processes over $Seconds. No nvidia-smi: polling
# it stalled both apps' frame rate.
function Measure-Pids([scriptblock]$GetPids, [int]$Seconds) {
    $pids = & $GetPids
    $t0 = @{}; foreach ($id in $pids) { $t0[$id] = (Get-Process -Id $id).TotalProcessorTime.TotalMilliseconds }
    $clock = [Diagnostics.Stopwatch]::StartNew()
    $gpu = @(); $whole = @(); $fps = @(); $adapters = @{}
    for ($i = 0; $i -lt $Seconds; $i++) {
        $samples = (Get-Counter "\GPU Engine(*)\Utilization Percentage" -ErrorAction SilentlyContinue).CounterSamples
        $own = 0; $all3d = @{}
        foreach ($c in $samples) {
            $luid = $c.InstanceName -replace '.*(luid_0x[0-9a-f]+_0x[0-9a-f]+).*', '$1'
            if ($c.InstanceName -like "*engtype_3D") { $all3d[$luid] += $c.CookedValue }
            foreach ($id in $pids) {
                if ($c.InstanceName -like "pid_$($id)_*") { $own += $c.CookedValue; if ($c.CookedValue -gt 0.05) { $adapters[$luid] = 1 } }
            }
        }
        $gpu += $own
        # Whole-adapter 3D use (every process, including the compositor) on the adapter the app uses.
        $busy = @($adapters.Keys); $whole += if ($busy.Count) { $all3d[$busy[0]] } else { 0 }
        if ((Get-Title $pids) -match 'fps (\d+)') { $fps += [int]$Matches[1] }
    }
    $wall = $clock.Elapsed.TotalMilliseconds
    $cpu = 0; $ws = 0; $priv = 0
    foreach ($id in $pids) {
        $p = Get-Process -Id $id -ErrorAction SilentlyContinue
        if ($p) { $cpu += $p.TotalProcessorTime.TotalMilliseconds - $t0[$id]; $ws += $p.WorkingSet64; $priv += $p.PrivateMemorySize64 }
    }
    [pscustomobject]@{
        processes        = $pids.Count
        cpu_core_pct     = [math]::Round(100 * $cpu / $wall, 2)           # % of one core
        cpu_total_pct    = [math]::Round(100 * $cpu / $wall / $Cores, 2)  # % of the whole CPU
        gpu_pct          = [math]::Round(($gpu | Measure-Object -Average).Average, 2)
        whole_gpu_3d_pct = [math]::Round(($whole | Measure-Object -Average).Average, 2)
        working_set_mb   = [math]::Round($ws / 1MB, 1)
        private_mb       = [math]::Round($priv / 1MB, 1)
        fps              = ($fps -join ' ')
    }
}

# Launch until the first frame is on screen, in ms. Bare shows its window at its
# first frame; the page sets its title to "ready" after its second animation frame.
function Time-Start([string]$Kind, [string]$Url) {
    $clock = [Diagnostics.Stopwatch]::StartNew()
    if ($Kind -eq "bare") {
        $p = Start-Process $Bare -PassThru
        while ($true) { $p.Refresh(); if ($p.MainWindowHandle -ne 0) { break }; Start-Sleep -Milliseconds 1 }
        $ms = $clock.Elapsed.TotalMilliseconds
        Stop-Process -Id $p.Id -Force; Start-Sleep -Milliseconds 400
    } else {
        Start-Process $Edge -ArgumentList (Edge-Args $Url) | Out-Null
        $done = $false
        while (-not $done -and $clock.Elapsed.TotalSeconds -lt 30) {
            foreach ($q in Get-Process msedge -ErrorAction SilentlyContinue) { if ($q.MainWindowTitle -eq "ready") { $done = $true } }
            Start-Sleep -Milliseconds 1
        }
        $ms = $clock.Elapsed.TotalMilliseconds
        Stop-Edge
    }
    [math]::Round($ms)
}

New-Item -ItemType Directory -Force $Work | Out-Null
$needEdge = $Which -in "all", "start", "edge", "edgestir"
$url = if ($needEdge) { Prepare-Web } else { "" }
$out = [ordered]@{}
if ($Which -in "all", "start") {
    Stop-Edge
    Time-Start "edge" $url | Out-Null   # the first run creates the profile: not counted
    $out.edge_start_ms = @(1..5 | ForEach-Object { Time-Start "edge" $url })
    Time-Start "bare" | Out-Null
    $out.bare_start_ms = @(1..5 | ForEach-Object { Time-Start "bare" })
}
if ($Which -in "all", "base") {   # nothing running (this script's own cost shows as CPU)
    $out.baseline = Measure-Pids { @($PID) } 15
}
if ($Which -in "all", "bare") {
    $p = Start-Process $Bench -PassThru; Start-Sleep 1; $p.Refresh(); Pin $p.MainWindowHandle; Start-Sleep 4
    $out.bare_active = Measure-Pids { $p.Id } 15
    Stop-Process -Id $p.Id -Force; Start-Sleep 1
    $p = Start-Process $Bare -PassThru; Start-Sleep 1; $p.Refresh(); Pin $p.MainWindowHandle; Start-Sleep 14   # settles after ~7 s
    $out.bare_idle = Measure-Pids { $p.Id } 15
    Stop-Process -Id $p.Id -Force; Start-Sleep 1
}
if ($Which -in "all", "edge") {
    Start-Edge $url
    $out.edge = Measure-Pids { Get-EdgePids } 15
    Stop-Edge
}
if ($Which -in "all", "edgestir") {
    Start-Edge "$url`?stir"
    $out.edge_stirred = Measure-Pids { Get-EdgePids } 15
    Stop-Edge
}
$out | ConvertTo-Json -Depth 4
