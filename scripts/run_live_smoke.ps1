<#
.SYNOPSIS
    Interactive Desktop Live Smoke Script for CPower v1.1.0.
.DESCRIPTION
    Launches a temporary WinForms test form, exercises CLI subcommands
    (windows, find, action --verify, diff, capture PNG, focus), tests
    ShellExecute launch behavior of bin\cpower_uiaccess.exe, and cleans up.
    Note: DXGI capture requires an active user desktop session; in headless
    WinStation sandboxes DXGI capture gracefully reports E_ACCESSDENIED.
#>

param(
    [switch]$SkipUiAccessLaunch
)

$ErrorActionPreference = "Stop"

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$CpowerExe = Join-Path $RepoRoot "bin\cpower.exe"
$UiAccessExe = Join-Path $RepoRoot "bin\cpower_uiaccess.exe"

if (-not (Test-Path $CpowerExe)) {
    throw "Missing $CpowerExe. Run mingw32-make clean all first."
}

$Tag = "CPowerSmoke_$([System.Diagnostics.Process]::GetCurrentProcess().Id)"
$OutPng = Join-Path $env:TEMP "$Tag.png"
$GuiProc = $null

Write-Host "========================================================================" -ForegroundColor Cyan
Write-Host "  CPower v1.1.0 Live Desktop Smoke Verification" -ForegroundColor Cyan
Write-Host "========================================================================" -ForegroundColor Cyan

try {
    Write-Host "[1/6] Running 'cpower.exe verify'..." -ForegroundColor Yellow
    & $CpowerExe verify
    if ($LASTEXITCODE -ne 0) { throw "cpower verify failed ($LASTEXITCODE)" }

    Write-Host "[2/6] Launching temporary WinForms target ($Tag)..." -ForegroundColor Yellow
    $psForm = (
        "Add-Type -AssemblyName System.Windows.Forms; " +
        "`$script:cnt=0; " +
        "`$f=New-Object Windows.Forms.Form; `$f.Text='$Tag'; `$f.Width=380; `$f.Height=250; " +
        "`$tb=New-Object Windows.Forms.TextBox; `$tb.Name='SmokeEdit'; `$tb.Text='BeforeSmoke'; `$tb.SetBounds(20,20,260,26); `$f.Controls.Add(`$tb); " +
        "`$lbl=New-Object Windows.Forms.Label; `$lbl.Name='SmokeStatus'; `$lbl.Text='Status:0'; `$lbl.SetBounds(20,60,180,24); `$f.Controls.Add(`$lbl); " +
        "`$btn=New-Object Windows.Forms.Button; `$btn.Name='SmokeApplyBtn'; `$btn.Text='ApplySmoke'; `$btn.SetBounds(20,96,160,32); " +
        "`$btn.Add_Click({ `$script:cnt++; `$lbl.Text='Status:' + `$script:cnt }); `$f.Controls.Add(`$btn); " +
        "`$guard=New-Object Windows.Forms.Timer; `$guard.Interval=30000; `$guard.Add_Tick({ `$guard.Stop(); `$f.Close() }); " +
        "`$f.Add_Shown({ `$f.Activate(); `$tb.Focus(); `$guard.Start() }); " +
        "[Windows.Forms.Application]::Run(`$f)"
    )
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "powershell.exe"
    $psi.Arguments = "-NoProfile -NonInteractive -Command `"$psForm`""
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $GuiProc = [System.Diagnostics.Process]::Start($psi)

    $targetWin = $null
    $deadline = (Get-Date).AddSeconds(10)
    while ((Get-Date) -lt $deadline) {
        $winsJson = & $CpowerExe windows --json | ConvertFrom-Json
        $targetWin = $winsJson.windows | Where-Object { $_.title -eq $Tag } | Select-Object -First 1
        if ($targetWin) { break }
        Start-Sleep -Milliseconds 100
    }
    if (-not $targetWin) { throw "Target form '$Tag' did not appear in cpower windows --json" }
    $hwnd = $targetWin.hwnd
    Write-Host "  Found target HWND=$hwnd rect=$($targetWin.rect -join ',')" -ForegroundColor Green

    Write-Host "[3/6] Finding UI elements via 'cpower.exe find'..." -ForegroundColor Yellow
    $findJson = $null
    $findDeadline = (Get-Date).AddSeconds(6)
    while ((Get-Date) -lt $findDeadline) {
        $findJson = & $CpowerExe find --hwnd $hwnd --json | ConvertFrom-Json
        if ($findJson.matches.Count -ge 4) { break }
        Start-Sleep -Milliseconds 100
    }
    $editNode = $findJson.matches | Where-Object { $_.automationId -eq "SmokeEdit" } | Select-Object -First 1
    $btnNode  = $findJson.matches | Where-Object { $_.automationId -eq "SmokeApplyBtn" } | Select-Object -First 1
    if (-not $editNode -or -not $btnNode) { throw "Could not locate SmokeEdit / SmokeApplyBtn via cpower find" }
    Write-Host "  Located SmokeEdit node=$($editNode.nodeId) SmokeApplyBtn node=$($btnNode.nodeId)" -ForegroundColor Green

    Write-Host "[4/6] Driving UIA actions with closed-loop --verify + cpower diff..." -ForegroundColor Yellow
    & $CpowerExe action --cmd set_value --hwnd $hwnd --node $editNode.nodeId --text "SmokeVerified_110" --verify
    if ($LASTEXITCODE -ne 0) { throw "set_value --verify failed" }

    & $CpowerExe action --cmd invoke --hwnd $hwnd --node $btnNode.nodeId --verify
    if ($LASTEXITCODE -ne 0) { throw "invoke --verify failed" }

    $verifyFind = & $CpowerExe find --hwnd $hwnd --name "Status:1" --json | ConvertFrom-Json
    if ($verifyFind.totalMatches -lt 1) { throw "Expected Status:1 after invoke" }

    $diffIdle = & $CpowerExe diff --hwnd $hwnd --delay 100 --format json | ConvertFrom-Json
    if ($diffIdle.entryCount -ne 0) { throw "Expected idle diff entryCount == 0, got $($diffIdle.entryCount)" }
    Write-Host "  Closed-loop verified: SmokeEdit='SmokeVerified_110', Status:1, idle diff=0" -ForegroundColor Green

    Write-Host "[5/6] Testing DXGI VRAM PNG micro-region capture..." -ForegroundColor Yellow
    $r = $targetWin.rect
    $capL = [int]$r[0]; $capT = [int]$r[1]
    $capR = [Math]::Min($capL + 160, [int]$r[2])
    $capB = [Math]::Min($capT + 120, [int]$r[3])
    & $CpowerExe capture --left $capL --top $capT --right $capR --bottom $capB --out $OutPng
    if ($LASTEXITCODE -eq 0 -and (Test-Path $OutPng)) {
        $bytes = (Get-Item $OutPng).Length
        Write-Host "  Captured PNG ($bytes bytes) at $OutPng" -ForegroundColor Green
    } else {
        Write-Host "  [WARN] DXGI capture returned $LASTEXITCODE (expected in headless/non-default desktop)" -ForegroundColor DarkYellow
    }

    if (-not $SkipUiAccessLaunch -and (Test-Path $UiAccessExe)) {
        Write-Host "[6/6] Checking ShellExecute launch of cpower_uiaccess.exe..." -ForegroundColor Yellow
        $sig = Get-AuthenticodeSignature -FilePath $UiAccessExe
        Write-Host "  cpower_uiaccess.exe Authenticode Status: $($sig.Status) ($($sig.SignerCertificate.Subject))"
        $hookDll = Join-Path $RepoRoot "bin\cpower_hook64.dll"
        if (Test-Path $hookDll) {
            $hookSig = Get-AuthenticodeSignature -FilePath $hookDll
            Write-Host "  cpower_hook64.dll Authenticode Status: $($hookSig.Status) ($($hookSig.SignerCertificate.Subject))"
        }
        try {
            $p = Start-Process -FilePath $UiAccessExe -ArgumentList @("verify") -PassThru -Wait -WindowStyle Hidden
            Write-Host "  ShellExecute cpower_uiaccess.exe exited with code $($p.ExitCode)" -ForegroundColor Green
        } catch {
            Write-Host "  [WARN] ShellExecute launch threw (expected if unprivileged/UIPI restricted or non-interactive session): $_" -ForegroundColor DarkYellow
        }
    } else {
        Write-Host "[6/6] Skipped cpower_uiaccess.exe launch check." -ForegroundColor Gray
    }
}
finally {
    if ($GuiProc -and -not $GuiProc.HasExited) {
        try {
            if ($hwnd) { & $CpowerExe focus --hwnd $hwnd --action close | Out-Null }
        } catch {}
        Start-Sleep -Milliseconds 150
        if (-not $GuiProc.HasExited) { Stop-Process -Id $GuiProc.Id -Force -ErrorAction SilentlyContinue }
    }
    Remove-Item $OutPng -ErrorAction SilentlyContinue
}

Write-Host "========================================================================" -ForegroundColor Cyan
Write-Host "  Live Smoke Verification Finished" -ForegroundColor Cyan
Write-Host "========================================================================" -ForegroundColor Cyan
