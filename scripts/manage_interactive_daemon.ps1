<#
.SYNOPSIS
    Manages the CPower Interactive Background Daemon (Start, Stop, Status, InstallTask, UninstallTask).
.DESCRIPTION
    Runs CPower in the user's interactive Windows desktop session (Session 1) with
    full UIPI bypass (cpower_uiaccess.exe) or standard mode (cpower.exe), allowing
    AI agents, MCP servers, and local clients to control the desktop without
    session-isolation barriers.
.PARAMETER Action
    Start, Stop, Status, Restart, InstallTask, UninstallTask. Default: Status.
.PARAMETER Transport
    IPC transport: shm (Shared Memory) or pipe (Named Pipe). Default: shm.
.PARAMETER PreferStandard
    If set, forces the use of cpower.exe instead of cpower_uiaccess.exe.
#>
[CmdletBinding()]
param(
    [ValidateSet("Start", "Stop", "Status", "Restart", "InstallTask", "UninstallTask")]
    [string]$Action = "Status",

    [ValidateSet("shm", "pipe")]
    [string]$Transport = "shm",

    [switch]$PreferStandard
)

$ErrorActionPreference = "Stop"
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$UiAccessExe = Join-Path $RepoRoot "bin\cpower_uiaccess.exe"
$StandardExe = Join-Path $RepoRoot "bin\cpower.exe"
$PythonExe = "python.exe"
$TaskName = "CPowerInteractiveDaemon"

function Get-RunningDaemonProcesses {
    $procs = @()
    foreach ($p in (Get-Process -Name "cpower", "cpower_uiaccess" -ErrorAction SilentlyContinue)) {
        try {
            $cmdline = (Get-CimInstance Win32_Process -Filter "ProcessId = $($p.Id)" -ErrorAction SilentlyContinue).CommandLine
            if ($cmdline -and $cmdline -match "\bdaemon\b") {
                $procs += $p
            }
        } catch {}
    }
    return $procs
}

function Test-IpcConnection {
    param([string]$TransportMode)
    $pyTest = "import sys; sys.path.insert(0, r'$RepoRoot\clients\python'); from cpower_client import CPowerClient; c = CPowerClient(transport='$TransportMode', auto_spawn=False, timeout_ms=1000); print(len(c.list_windows())); c.close()"
    try {
        $out = & $PythonExe -c $pyTest 2>$null
        if ($LASTEXITCODE -eq 0) {
            return [int]($out.Trim())
        }
    } catch {}
    return -1
}

function Stop-DaemonGraceful {
    Write-Host "[*] Stopping running CPower daemon..." -ForegroundColor Cyan
    $pyStop = "import sys; sys.path.insert(0, r'$RepoRoot\clients\python'); from cpower_client import CPowerClient, CMD_SHUTDOWN, EXEC_MODE_DIRECT_INSTANT; " +
              "try:`n  c = CPowerClient(transport='auto', auto_spawn=False, timeout_ms=800); c._execute(CMD_SHUTDOWN, mode=EXEC_MODE_DIRECT_INSTANT); c.close()`nexcept: pass"
    try {
        & $PythonExe -c $pyStop 2>$null
    } catch {}

    Start-Sleep -Milliseconds 600
    $running = Get-RunningDaemonProcesses
    foreach ($p in $running) {
        Write-Host "    Terminating PID $($p.Id)..." -ForegroundColor Yellow
        Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    }
    Start-Sleep -Milliseconds 300
    Write-Host "[+] CPower daemon stopped." -ForegroundColor Green
}

function Show-DaemonStatus {
    Write-Host "========================================================================" -ForegroundColor Cyan
    Write-Host "  CPower Interactive Daemon Status" -ForegroundColor Cyan
    Write-Host "========================================================================" -ForegroundColor Cyan

    $running = Get-RunningDaemonProcesses
    if ($running.Count -eq 0) {
        Write-Host "  Daemon Process : NOT RUNNING" -ForegroundColor Yellow
    } else {
        foreach ($p in $running) {
            $sig = Get-AuthenticodeSignature -FilePath $p.Path -ErrorAction SilentlyContinue
            Write-Host "  Daemon Process : RUNNING (PID=$($p.Id), Session=$($p.SessionId))" -ForegroundColor Green
            Write-Host "  Executable     : $($p.Path)"
            Write-Host "  Signature      : $($sig.Status) ($($sig.SignerCertificate.Subject))"
            Write-Host "  Working Set    : $([math]::Round($p.WorkingSet64 / 1MB, 2)) MB"
        }
    }

    $shmCount = Test-IpcConnection -TransportMode "shm"
    if ($shmCount -ge 0) {
        Write-Host "  SHM IPC Channel: ACTIVE (Connected! Desktop visible windows: $shmCount)" -ForegroundColor Green
    } else {
        Write-Host "  SHM IPC Channel: Inactive (no response on Local\CPowerAgentIPC)" -ForegroundColor Gray
    }

    $pipeCount = Test-IpcConnection -TransportMode "pipe"
    if ($pipeCount -ge 0) {
        Write-Host "  PIPE Channel   : ACTIVE (Connected! Desktop visible windows: $pipeCount)" -ForegroundColor Green
    } else {
        Write-Host "  PIPE Channel   : Inactive (no response on \\.\pipe\cpower_ipc)" -ForegroundColor Gray
    }

    $task = Get-ScheduledTask -TaskName $TaskName -ErrorAction SilentlyContinue
    if ($task) {
        Write-Host "  Logon Task     : INSTALLED ($TaskName, State: $($task.State))" -ForegroundColor Green
    } else {
        Write-Host "  Logon Task     : Not installed" -ForegroundColor Gray
    }
    Write-Host "========================================================================" -ForegroundColor Cyan
}

function Start-DaemonProcess {
    $existing = Get-RunningDaemonProcesses
    if ($existing.Count -gt 0) {
        Write-Host "[!] CPower daemon is already running (PID: $($existing[0].Id), Session: $($existing[0].SessionId))." -ForegroundColor Yellow
        Show-DaemonStatus
        return
    }

    $targetExe = $StandardExe
    if (-not $PreferStandard -and (Test-Path $UiAccessExe)) {
        $sig = Get-AuthenticodeSignature -FilePath $UiAccessExe
        if ($sig.Status -eq "Valid") {
            $targetExe = $UiAccessExe
        }
    }

    Write-Host "[*] Launching CPower daemon: '$targetExe' with transport '$Transport'..." -ForegroundColor Cyan
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $targetExe
    $psi.Arguments = "daemon --transport $Transport"
    $psi.UseShellExecute = $true
    $psi.WindowStyle = [System.Diagnostics.ProcessWindowStyle]::Hidden

    try {
        $proc = [System.Diagnostics.Process]::Start($psi)
    } catch {
        Write-Warning "ShellExecute failed ($_); falling back to standard cpower.exe..."
        $psi.FileName = $StandardExe
        $proc = [System.Diagnostics.Process]::Start($psi)
    }

    # Wait for daemon to respond on IPC
    $connected = $false
    $deadline = (Get-Date).AddSeconds(5)
    while ((Get-Date) -lt $deadline) {
        if ((Test-IpcConnection -TransportMode $Transport) -ge 0) {
            $connected = $true
            break
        }
        Start-Sleep -Milliseconds 150
    }

    if ($connected) {
        Write-Host "[+] CPower daemon started and listening successfully!" -ForegroundColor Green
    } else {
        Write-Warning "Daemon process started but IPC ping timed out after 5s."
    }
    Show-DaemonStatus
}

function Install-LogonTask {
    Write-Host "[*] Registering scheduled task '$TaskName' to start CPower daemon at user logon..." -ForegroundColor Cyan
    $actionScript = Join-Path $PSScriptRoot "manage_interactive_daemon.ps1"
    $argList = "-NoProfile -WindowStyle Hidden -ExecutionPolicy Bypass -File `"$actionScript`" -Action Start -Transport $Transport"
    
    $action = New-ScheduledTaskAction -Execute "powershell.exe" -Argument $argList
    $trigger = New-ScheduledTaskTrigger -AtLogOn
    $settings = New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Days 0)
    $principal = New-ScheduledTaskPrincipal -UserId $env:USERNAME -LogonType Interactive -RunLevel Highest

    try {
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
        Register-ScheduledTask -TaskName $TaskName -Action $action -Trigger $trigger -Settings $settings -Principal $principal | Out-Null
        Write-Host "[+] Scheduled Task '$TaskName' registered successfully with Highest Privileges!" -ForegroundColor Green
    } catch {
        Write-Error "Failed to register Scheduled Task: $_"
    }
}

function Uninstall-LogonTask {
    Write-Host "[*] Removing scheduled task '$TaskName'..." -ForegroundColor Cyan
    try {
        Unregister-ScheduledTask -TaskName $TaskName -Confirm:$false -ErrorAction SilentlyContinue
        Write-Host "[+] Scheduled Task '$TaskName' removed." -ForegroundColor Green
    } catch {
        Write-Warning "Task could not be removed: $_"
    }
}

switch ($Action) {
    "Status"         { Show-DaemonStatus }
    "Start"          { Start-DaemonProcess }
    "Stop"           { Stop-DaemonGraceful }
    "Restart"        { Stop-DaemonGraceful; Start-DaemonProcess }
    "InstallTask"    { Install-LogonTask }
    "UninstallTask"  { Uninstall-LogonTask }
}
