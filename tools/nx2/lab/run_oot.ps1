# Ocarina of Time lab run: boots the game hidden with eden-cli, loads the newest save, plays in
# Kokiri Forest, and keeps the log, a screenshot and any stall dumps. See README.md here.
#
#   powershell -ExecutionPolicy Bypass -File tools\nx2\lab\run_oot.ps1 -Name before -Game "<path to the game .nup>"
#   ... -Env EDEN_NX2_GPU_PROFILE=1      (environment switches, comma-separated)
#   ... -Quick                           (40 s of play, no screenshot: for verify runs)
#   ... -Stay                            (no walk: only the save's spawn spot)
#   ... -Flash 30                        (30 captures at the spawn spot: brightness flashes; no walk)
#   python -I tools\nx2\lab\perf_summary.py tools\nx2\lab\runs\before
#
# Exit codes: 0 gameplay reached, no device loss; 1 DEVICE_LOST or the emulator exited early;
# 2 not started (bad arguments, an emulator already running); 3 INVALID, gameplay never started
# (title screen frames only); 6 a lab step failed (python, launch, a key press, or stopping the emulator).
param(
    [Parameter(Mandatory = $true)][string]$Name,
    [string]$Game = $env:EGLANTINE_OOT_GAME,
    [string[]]$Env = @(),
    [string]$Config = "",
    [int]$TitleWait = 100,
    [int]$GameplayWait = 100,
    [switch]$Quick,
    [switch]$Stay,
    [int]$Flash = 0,
    [int]$MaxSeconds = 420,
    [string]$Binary = "",
    [string]$Keys = "",
    # The first log line of gameplay: Kokiri Forest's fog volumes (64x36x96 3D textures), about 11 s
    # after the save is confirmed. Change it only for a save somewhere else.
    [string]$GameplayMarker = "64x36x96"
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
$Python = (Get-Command python -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1).Source
if (-not $Python) {
    Write-Output "ABORT: python not found on PATH."
    exit 2
}
$D = Join-Path (Join-Path $Lab "runs") $Name
New-Item -ItemType Directory -Force $D | Out-Null
Remove-Item "$D\phases.txt", "$D\flash.jsonl", "$D\eden_log.txt" -ErrorAction SilentlyContinue
Set-Location $Repo
$t0 = Get-Date
function Seconds() { return ((Get-Date) - $t0).TotalSeconds }
function Log($m) { Write-Output ("[{0,6:F1}s] {1}" -f (Seconds), $m) }
# phases.txt: when gameplay and the walk started, for perf_summary.py (seconds since launch, which
# is about the log's own clock).
function Phase($name) { "{0} {1:F1}" -f $name, ((Get-Date) - $script:launched).TotalSeconds | Add-Content "$D\phases.txt" }
$AppLog = if ($env:EGLANTINE_LOG) { $env:EGLANTINE_LOG } else { "$env:APPDATA\eden\log\eden_log.txt" }

# --- child processes -------------------------------------------------------------------------------
# PowerShell 5.1 sets the console title before it runs a native command. Once the console it started
# from is gone (the terminal or the shell that launched this script exited), that fails and the command
# is skipped without stopping the script, so a run could carry on without its key presses and still
# report a result. Process.Start has no such step; CreateNoWindow gives the child a hidden console.
function ConvertTo-LabArg([string]$a) {
    if ($a -ne "" -and $a -notmatch '[\s"]') { return $a }
    $q = $a -replace '(\\*)"', '$1$1\"'
    $q = $q -replace '(\\+)$', '$1$1'
    return '"' + $q + '"'
}
# Runs $File; its stdout lines go to the pipeline, stderr to ours. $global:LASTEXITCODE is -1 when it
# could not start.
function Invoke-LabProc([string]$File, [string[]]$ArgList) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $File
    $psi.Arguments = (@($ArgList | ForEach-Object { ConvertTo-LabArg $_ }) -join " ")
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.WorkingDirectory = $Repo
    try {
        $p = [System.Diagnostics.Process]::Start($psi)
    } catch {
        $global:LASTEXITCODE = -1
        $script:LabProcError = "could not start $File`: $($_.Exception.Message)"
        return
    }
    $err = $p.StandardError.ReadToEndAsync()
    while ($null -ne ($line = $p.StandardOutput.ReadLine())) { $line }
    $p.WaitForExit()
    if ($err.Result) { try { [Console]::Error.Write($err.Result) } catch {} }
    $global:LASTEXITCODE = $p.ExitCode
    $p.Dispose()
}
function Invoke-LabPy([string[]]$ArgList) { Invoke-LabProc -File $Python -ArgList (@("-I") + $ArgList) }
# lab_ctl.py -d $D <command...>. -Fatal (launch, key presses): a failure ends the run with exit 6, since
# every later step would be meaningless. Python that cannot start is always fatal.
function Invoke-LabCtl([string[]]$CtlArgs, [switch]$Fatal) {
    Invoke-LabPy (@("tools/nx2/lab_ctl.py", "-d", $D) + $CtlArgs)
    $code = $LASTEXITCODE
    if ($code -eq 0) { return }
    if ($code -eq -1 -or $Fatal) {
        $why = if ($code -eq -1) { $script:LabProcError } else { "exit $code" }
        Write-Output "ABORT: lab_ctl $($CtlArgs[0]) failed ($why)"
        throw "LABFATAL"
    }
    Write-Output "WARNING: lab_ctl $($CtlArgs[0]) failed (exit $code)"
}

# This run's emulator, from emulator.pid; $null once it has exited. A pid whose process started long
# after the file was written belongs to someone else (pids are reused).
function Get-LabEmulator() {
    $pidFile = "$D\emulator.pid"
    if (-not (Test-Path $pidFile)) { return $null }
    $text = Get-Content $pidFile -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($text -notmatch "^\s*(\d+)\s*$") { return $null }
    $p = Get-Process -Id ([int]$Matches[1]) -ErrorAction SilentlyContinue
    if ($p -and [Math]::Abs(($p.StartTime - (Get-Item $pidFile).LastWriteTime).TotalSeconds) -le 30) { return $p }
    return $null
}
# lab_ctl.py stop checks that the emulator is gone; if it is not, kill it here. $true when it is gone.
function Stop-LabEmulator() {
    if (-not (Get-LabEmulator)) { return $true }
    Invoke-LabPy @("tools/nx2/lab_ctl.py", "-d", $D, "stop") | Out-Null
    foreach ($i in 1..20) { if (-not (Get-LabEmulator)) { return $true }; Start-Sleep -Milliseconds 500 }
    $p = Get-LabEmulator
    if ($p) {
        Write-Output "WARNING: lab_ctl stop left emulator pid $($p.Id) running; terminating it"
        try { $p.Kill() } catch {}
        foreach ($i in 1..20) { if (-not (Get-LabEmulator)) { return $true }; Start-Sleep -Milliseconds 500 }
    }
    return -not (Get-LabEmulator)
}

# DEVICE_LOST in emulator.log or in eden_log.txt: emulator.log has missed one. eden_log.txt counts only
# once this run's emulator has rewritten it.
function Lost() {
    $a = (Test-Path "$D\emulator.log") -and (Select-String -Path "$D\emulator.log" -Pattern "DEVICE_LOST" -Quiet)
    $b = (Test-Path $AppLog) -and ((Get-Item $AppLog).LastWriteTime -ge $script:launched) -and
         (Select-String -Path $AppLog -Pattern "DEVICE_LOST" -Quiet)
    return ($a -or $b)
}
function InGameplay() {
    return (Test-Path "$D\emulator.log") -and (Select-String -Path "$D\emulator.log" -Pattern $GameplayMarker -SimpleMatch -Quiet)
}
function Exited() { return -not (Get-LabEmulator) }

# Watchdog: the emulator logs every second in gameplay, but emulator.log grows in buffered chunks up to
# ~20 s apart. If it stops growing for 45 s, save every thread's stack (twice, to show whether anything
# moves).
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
    if ($age -lt 45) { return }
    $proc = Get-LabEmulator
    if (-not $proc) { return }
    $script:dumps++
    Log ("STALL: log silent for {0:N0} s; stack dump {1}" -f $age, $script:dumps)
    Invoke-LabProc -File "powershell" -ArgList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", "$Lab\stack_dump.ps1", "-ProcessId", "$($proc.Id)", "-Out", "$D\stall-$($script:dumps).dmp")
}

# "-Env A=1,B=2" arrives as an array; a value that itself holds commas (EDEN_NX2_PERF_ON=a,b) arrives
# split too, so pieces without "=" rejoin the previous one.
$flat = @()
foreach ($e in $Env) {
    foreach ($piece in ($e -split ",")) {
        if (-not $piece) { continue }
        if ($piece -match "=" -or $flat.Count -eq 0) { $flat += $piece } else { $flat[$flat.Count - 1] += "," + $piece }
    }
}
$envArgs = @()
foreach ($e in $flat) { $envArgs += "--env"; $envArgs += $e }
if ($Binary -ne "") { $envArgs += "--binary"; $envArgs += $Binary }
if ($Keys -ne "") { $envArgs += "--keys"; $envArgs += $Keys }

$result = "INVALID"
$rc = 0
$script:launched = Get-Date
try {
    Log "launch env=[$($flat -join ' ')] config=$Config binary=$Binary keys=$Keys"
    # The --max-seconds watchdog runs in a hidden console of its own and stops the emulator even if
    # this script is gone.
    Invoke-LabCtl -Fatal (@("launch", "--hidden", "--config", $Config, "--game", $Game) + $envArgs + @("--max-seconds", "$MaxSeconds"))
    $script:launched = Get-Date
    Start-Sleep $TitleWait
    if ($Keys -eq "") {
        Log "press a (title)"
        Invoke-LabCtl -Fatal @("press", "a", "--hold", "220")
        Start-Sleep 15
        Log "press a (newest save)"
        Invoke-LabCtl -Fatal @("press", "a", "--hold", "220")
        Start-Sleep 8
        Log "press a (confirm)"
        Invoke-LabCtl -Fatal @("press", "a", "--hold", "220")
    } else {
        Log "timed key script drives the menus"
        Start-Sleep 24
    }
    # Press A again while gameplay has not started: the title or a menu may not have been up yet
    # when the first presses went out.
    $retries = 0
    while (-not (InGameplay) -and -not (Lost) -and -not (Exited) -and $retries -lt 6) {
        $polls = 0
        while ($polls -lt 10 -and -not (InGameplay)) { Start-Sleep 2; $polls++ }
        if (-not (InGameplay)) {
            $retries++
            Log "no gameplay yet; press a (retry $retries)"
            Invoke-LabCtl -Fatal @("press", "a", "--hold", "220")
        }
    }
    $gameplay = InGameplay
    if ($gameplay) {
        Log "gameplay started"
        Phase "gameplay"
        if ($Quick) { $GameplayWait = 40 }
        $waited = 0
        while ($waited -lt $GameplayWait) {
            Start-Sleep 5; $waited += 5
            Watch
            if ((Lost) -or (Exited)) { break }
        }
        if (-not (Lost) -and -not (Exited) -and -not $Quick) {
            Log "capture"
            Invoke-LabCtl @("capture", "--settle", "5", "--timeout", "60", "--out", "$D\game.png") | Out-Null
            Invoke-LabCtl @("state", "--kind", "p", "--limit", "12")
        }
        if ($Flash -gt 0 -and -not (Lost) -and -not (Exited)) {
            Log "flash: $Flash captures"
            Phase "flash"
            foreach ($i in 1..$Flash) {
                Invoke-LabCtl @("capture", "--settle", "1", "--timeout", "30") | Add-Content "$D\flash.jsonl"
                if ((Lost) -or (Exited)) { break }
            }
            Invoke-LabPy @("tools/nx2/lab/flash_stats.py", "$D\flash.jsonl")
        }
        if (-not $Stay -and $Flash -eq 0 -and -not (Lost) -and -not (Exited)) {
            # Walk around so more of the scene streams in.
            Log "walk"
            Phase "walk"
            foreach ($i in 1..4) {
                Invoke-LabCtl -Fatal @("press", "left", "--hold", "900")
                Invoke-LabCtl -Fatal @("press", "up", "--hold", "900")
                Start-Sleep 8
                Watch
                if ((Lost) -or (Exited)) { break }
            }
        }
    } elseif (-not (Lost) -and -not (Exited)) {
        Log "gameplay never started (no '$GameplayMarker' line in emulator.log)"
    }
    if (Lost) { $result = "DEVICE_LOST"; $rc = 1 }
    elseif (Exited) { $result = "EMULATOR EXITED before the end of the run (crash, or -MaxSeconds too short)"; $rc = 1 }
    elseif (-not $gameplay) { $result = "INVALID: never reached gameplay, the log holds only title screen and menu frames"; $rc = 3 }
    else { $result = "no device loss" }
    Phase "stop"
    Log "stop"
} catch {
    if ("$($_.Exception.Message)" -ne "LABFATAL") { throw }
    $result = "lab step failed"
    $rc = 6
} finally {
    # Also on Ctrl+C: never leave a hidden emulator running.
    if (-not (Stop-LabEmulator)) {
        Write-Output "ABORT: could not stop the emulator of $D; end eden-cli in Task Manager"
        $rc = 6
    }
}
if ((Test-Path $AppLog) -and (Get-Item $AppLog).LastWriteTime -ge $script:launched) {
    Copy-Item $AppLog "$D\eden_log.txt" -ErrorAction SilentlyContinue
}
if (Test-Path "$D\eden_log.txt") {
    Get-Content "$D\eden_log.txt" | Select-String -Pattern "DEVICE_LOST|Validation|VUID|ERROR_|Unhandled|crash" |
        Select-Object -First 25 | ForEach-Object { $_.Line.Substring(0, [Math]::Min(400, $_.Line.Length)) }
}
if ($rc -ne 6 -and (Test-Path "$D\emulator.log")) {
    Invoke-LabPy @("tools/nx2/lab/perf_summary.py", $D, "--marker", $GameplayMarker)
}
Log "result: $result"
exit $rc
