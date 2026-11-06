# Ocarina of Time lab run: boots the game hidden with eden-cli, loads the newest save, plays in
# Kokiri Forest, and keeps the log, a screenshot and any stall dumps. See README.md here.
#
#   powershell -ExecutionPolicy Bypass -File tools\nx2\lab\run_oot.ps1 -Name before -Game "<path to the game .nup>"
#   ... -Env EDEN_NX2_GPU_PROFILE=1      (environment switches, comma-separated)
#   ... -Quick                           (40 s of play, no screenshot: for verify runs)
#   python -I tools\nx2\lab\perf_summary.py tools\nx2\lab\runs\before\emulator.log
param(
    [Parameter(Mandatory = $true)][string]$Name,
    [string]$Game = $env:EGLANTINE_OOT_GAME,
    [string[]]$Env = @(),
    [string]$Config = "",
    [int]$TitleWait = 100,
    [int]$GameplayWait = 100,
    [switch]$Quick,
    [int]$MaxSeconds = 420,
    [string]$Binary = "",
    [string]$Keys = ""
)
$ErrorActionPreference = "Continue"
if (Get-Process eglantine, eden-cli -ErrorAction SilentlyContinue) {
    Write-Output "ABORT: an emulator is already running; close it first (two instances share the GPU and the pipeline cache)."
    exit 2
}
$Lab = $PSScriptRoot
$Repo = (Resolve-Path (Join-Path $Lab "..\..\..")).Path
if (-not $Game -or -not (Test-Path -LiteralPath $Game)) {
    Write-Output "ABORT: pass -Game <Ocarina of Time .nup> or set EGLANTINE_OOT_GAME."
    exit 2
}
if ($Config -eq "") { $Config = Join-Path $Lab "lab-config.ini" }
if (-not (Test-Path -LiteralPath $Config)) {
    Write-Output "ABORT: $Config not found. Run: python -I tools\nx2\lab\make_lab_config.py"
    exit 2
}
$S = Join-Path $Lab "runs"
$D = "$S\$Name"
New-Item -ItemType Directory -Force $D | Out-Null
Set-Location $Repo
$game = $Game
$t0 = Get-Date
function Log($m) { Write-Output ("[{0,6:F1}s] {1}" -f ((Get-Date) - $t0).TotalSeconds, $m) }
$AppLog = if ($env:EGLANTINE_LOG) { $env:EGLANTINE_LOG } else { "$env:APPDATA\eden\log\eden_log.txt" }
function Lost() {
    $a = (Test-Path "$D\emulator.log") -and (Select-String -Path "$D\emulator.log" -Pattern "DEVICE_LOST" -Quiet)
    $b = (Test-Path $AppLog) -and (Select-String -Path $AppLog -Pattern "DEVICE_LOST" -Quiet)
    return ($a -or $b)
}
$envArgs = @()
foreach ($e in (($Env -join ",") -split ",")) { if ($e) { $envArgs += "--env"; $envArgs += $e } }
if ($Binary -ne "") { $envArgs += "--binary"; $envArgs += $Binary }
if ($Keys -ne "") { $envArgs += "--keys"; $envArgs += $Keys }
Log "launch env=[$($Env -join ' ')] config=$Config binary=$Binary keys=$Keys"
python -I tools/nx2/lab_ctl.py -d $D launch --hidden --config $Config --game $game @envArgs --max-seconds $MaxSeconds
Start-Sleep $TitleWait
if ($Keys -eq "") {
    Log "press a (title)"
    python -I tools/nx2/lab_ctl.py -d $D press a --hold 220
    Start-Sleep 15
    Log "press a (newest save)"
    python -I tools/nx2/lab_ctl.py -d $D press a --hold 220
    Start-Sleep 8
    Log "press a (confirm)"
    python -I tools/nx2/lab_ctl.py -d $D press a --hold 220
} else {
    Log "timed key script drives the menus"
    Start-Sleep 24
}
# Gameplay has started once Kokiri Forest's fog volumes (64x36x96 3D textures) are created, about
# 11 s after the save is confirmed. Press A again while it has not appeared: the title or a menu
# may not have been up yet when the first presses went out.
# Watchdog: the emulator logs at least once a second in gameplay. If its log stops growing for
# 15 s, save every thread's stack (twice, 10 s apart, to show whether anything moves).
$script:dumps = 0
$script:logSize = -1
$script:logGrew = Get-Date
function Watch() {
    if ($script:dumps -ge 2 -or -not (Test-Path "$D\emulator.log")) { return }
    # Open the file: the directory entry's size, like its timestamp, is updated lazily.
    $fs = [System.IO.File]::Open("$D\emulator.log", "Open", "Read", "ReadWrite, Delete")
    $size = $fs.Length; $fs.Close()
    if ($size -ne $script:logSize) { $script:logSize = $size; $script:logGrew = Get-Date; return }
    $age = ((Get-Date) - $script:logGrew).TotalSeconds
    if ($age -lt 15) { return }
    $proc = Get-Process eden-cli -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $proc) { return }
    $script:dumps++
    Log ("STALL: log silent for {0:N0} s; stack dump {1}" -f $age, $script:dumps)
    powershell -NoProfile -ExecutionPolicy Bypass -File "$Lab\stack_dump.ps1" -ProcessId $proc.Id -Out "$D\stall-$($script:dumps).dmp"
}
function InGameplay() {
    return (Test-Path "$D\emulator.log") -and (Select-String -Path "$D\emulator.log" -Pattern "64x36x96" -Quiet)
}
$retries = 0
while (-not (InGameplay) -and -not (Lost) -and $retries -lt 6) {
    $polls = 0
    while ($polls -lt 10 -and -not (InGameplay)) { Start-Sleep 2; $polls++ }
    if (-not (InGameplay)) {
        $retries++
        Log "no gameplay yet; press a (retry $retries)"
        python -I tools/nx2/lab_ctl.py -d $D press a --hold 220
    }
}
if (InGameplay) { Log "gameplay started" } else { Log "WARNING: gameplay marker never appeared" }
if ($Quick) { $GameplayWait = 40 }
$waited = 0
while ($waited -lt $GameplayWait) {
    Start-Sleep 5; $waited += 5
    Watch
    if (Lost) { Log "DEVICE_LOST seen in emulator.log"; break }
}
if (-not (Lost) -and -not $Quick) {
    Log "capture"
    python -I tools/nx2/lab_ctl.py -d $D capture --settle 5 --timeout 60 --out "$D\game.png"
    Log "state"
    python -I tools/nx2/lab_ctl.py -d $D state --kind p --limit 12
}
if (-not (Lost)) {
    # Play on a while longer: walk around so more of the scene streams in.
    foreach ($i in 1..4) {
        python -I tools/nx2/lab_ctl.py -d $D press left --hold 900
        python -I tools/nx2/lab_ctl.py -d $D press up --hold 900
        Start-Sleep 8
        Watch
        if (Lost) { Log "DEVICE_LOST seen while walking"; break }
    }
}
Log "stop"
python -I tools/nx2/lab_ctl.py -d $D stop
Log ("result: " + $(if (Lost) { "DEVICE_LOST" } else { "no device loss" }))
Copy-Item $AppLog "$D\eden_log.txt" -ErrorAction SilentlyContinue
Get-Content "$D\eden_log.txt" | Select-String -Pattern "DEVICE_LOST|Nx2Perf:|Validation|VUID|ERROR_|Unhandled|crash" | Select-Object -First 25 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(400, $_.Line.Length)) }
