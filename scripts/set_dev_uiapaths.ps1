<#
.SYNOPSIS
    Configures HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System\EnableSecureUIAPaths.
.DESCRIPTION
    By default Windows enforces EnableSecureUIAPaths=1, requiring uiAccess="true"
    binaries to reside in %ProgramFiles% or %SystemRoot%\System32. Setting
    EnableSecureUIAPaths=0 allows running signed uiAccess="true" binaries directly
    from development workspaces (such as bin\cpower_uiaccess.exe).
.PARAMETER EnableSecurePaths
    Pass -EnableSecurePaths:$true (or -EnableSecurePaths) to restore strict
    production enforcement (1). Omit or pass -EnableSecurePaths:$false to set 0
    for development testing.
#>
#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [switch]$EnableSecurePaths = $false
)

$ErrorActionPreference = "Stop"

$regPath = "HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Policies\System"
$val = if ($EnableSecurePaths) { 1 } else { 0 }

if (-not (Test-Path -LiteralPath $regPath)) {
    New-Item -Path $regPath -Force | Out-Null
}

Set-ItemProperty -Path $regPath -Name "EnableSecureUIAPaths" -Value $val -Type DWord
$current = (Get-ItemProperty -Path $regPath -Name "EnableSecureUIAPaths").EnableSecureUIAPaths

Write-Host "[+] EnableSecureUIAPaths is now set to $current in $regPath" -ForegroundColor Green
if ($current -eq 0) {
    Write-Host "[i] Development mode active: signed uiAccess binaries can run from workspace directories." -ForegroundColor Yellow
    Write-Warning "EnableSecureUIAPaths=0 relaxes machine-wide uiAccess path enforcement. Restore strict enforcement when finished by running: .\scripts\set_dev_uiapaths.ps1 -EnableSecurePaths"
} else {
    Write-Host "[i] Production mode active: uiAccess binaries must reside in ProgramFiles or System32." -ForegroundColor Gray
}
