<#
.SYNOPSIS
    Signs compiled cpower binaries using the local code-signing certificate for uiAccess.
.DESCRIPTION
    Locates a valid Code Signing EKU (1.3.6.1.5.5.7.3.3) certificate in
    Cert:\LocalMachine\My (or Cert:\CurrentUser\My), signs the target PE binaries
    with SHA-256 Authenticode (preferring dynamically discovered Windows SDK
    signtool.exe when available, or PowerShell Set-AuthenticodeSignature), and
    verifies signature status.

    By default (when -BinaryPath is omitted), signs both bin\cpower_uiaccess.exe
    and bin\cpower_hook64.dll.
.PARAMETER BinaryPath
    One or more binary paths to sign. Defaults to ..\bin\cpower_uiaccess.exe
    and ..\bin\cpower_hook64.dll.
.PARAMETER CertName
    Common Name (CN) of the signing certificate. Default: "CPowerLocalRoot".
.PARAMETER TimestampUrl
    Optional RFC 3161 timestamp server URL. Omit for offline local signing.
#>
[CmdletBinding()]
param(
    [string[]]$BinaryPath = @(
        (Join-Path $PSScriptRoot "..\bin\cpower.exe"),
        (Join-Path $PSScriptRoot "..\bin\cpower_uiaccess.exe"),
        (Join-Path $PSScriptRoot "..\bin\cpower_hook64.dll")
    ),
    [string]$CertName = "CPowerLocalRoot",
    [string]$TimestampUrl = ""
)

$ErrorActionPreference = "Stop"

$resolvedTargets = @()
foreach ($pathItem in $BinaryPath) {
    if (-not (Test-Path -LiteralPath $pathItem)) {
        Write-Error "Target binary not found: '$pathItem'"
        exit 1
    }
    $resolvedTargets += (Resolve-Path -LiteralPath $pathItem).Path
}

$codeSigningOid = "1.3.6.1.5.5.7.3.3"
$cert = Get-ChildItem -Path "Cert:\LocalMachine\My" -ErrorAction SilentlyContinue |
    Where-Object {
        $_.Subject -eq "CN=$CertName" -and
        $_.HasPrivateKey -and
        $_.NotAfter -gt (Get-Date) -and
        ($_.EnhancedKeyUsageList | Where-Object { $_.ObjectId -eq $codeSigningOid })
    } |
    Sort-Object NotAfter -Descending |
    Select-Object -First 1

$storeScope = "Machine"
if (-not $cert) {
    $cert = Get-ChildItem -Path "Cert:\CurrentUser\My" -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Subject -eq "CN=$CertName" -and
            $_.HasPrivateKey -and
            $_.NotAfter -gt (Get-Date) -and
            ($_.EnhancedKeyUsageList | Where-Object { $_.ObjectId -eq $codeSigningOid })
        } |
        Sort-Object NotAfter -Descending |
        Select-Object -First 1
    $storeScope = "User"
}

if (-not $cert) {
    Write-Error "Code-signing certificate 'CN=$CertName' (EKU $codeSigningOid) not found in My store. Run scripts\setup_cert.ps1 as Administrator first."
    exit 1
}

function Find-WindowsSdkSignTool {
    $cmdSigntool = Get-Command signtool.exe -ErrorAction SilentlyContinue
    if ($cmdSigntool) {
        return $cmdSigntool.Source
    }

    $kitRoots = @()
    foreach ($regPath in @(
        "HKLM:\SOFTWARE\Microsoft\Windows Kits\Installed Roots",
        "HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots"
    )) {
        if (Test-Path -LiteralPath $regPath) {
            $props = Get-ItemProperty -Path $regPath -ErrorAction SilentlyContinue
            if ($props -and $props.KitsRoot10) {
                $kitRoots += $props.KitsRoot10
            }
        }
    }

    foreach ($envRoot in @(${env:ProgramFiles(x86)}, $env:ProgramFiles)) {
        if (-not [string]::IsNullOrWhiteSpace($envRoot)) {
            $kitRoots += (Join-Path $envRoot "Windows Kits\10")
        }
    }

    $candidates = @()
    foreach ($root in ($kitRoots | Select-Object -Unique)) {
        $binDir = Join-Path $root "bin"
        if (Test-Path -LiteralPath $binDir) {
            $found = Get-ChildItem -Path (Join-Path $binDir "*\x64\signtool.exe") -ErrorAction SilentlyContinue
            if ($found) {
                $candidates += $found
            }
        }
    }

    if ($candidates.Count -gt 0) {
        $best = $candidates | Sort-Object FullName -Descending | Select-Object -First 1
        return $best.FullName
    }
    return $null
}

$signtool = Find-WindowsSdkSignTool

foreach ($resolvedBinary in $resolvedTargets) {
    if ($signtool -and $storeScope -eq "Machine") {
        Write-Host "[*] Signing '$resolvedBinary' using signtool.exe ($($cert.Thumbprint))..." -ForegroundColor Cyan
        $signArgs = @("sign", "/sm", "/s", "My", "/sha1", $cert.Thumbprint, "/fd", "SHA256")
        if (-not [string]::IsNullOrWhiteSpace($TimestampUrl)) {
            $signArgs += @("/tr", $TimestampUrl, "/td", "SHA256")
        }
        $signArgs += $resolvedBinary
        & $signtool @signArgs
        if ($LASTEXITCODE -ne 0) {
            Write-Error "signtool.exe failed for '$resolvedBinary' with exit code $LASTEXITCODE"
            exit $LASTEXITCODE
        }
    } else {
        Write-Host "[*] Signing '$resolvedBinary' using Set-AuthenticodeSignature ($($cert.Thumbprint))..." -ForegroundColor Cyan
        if (-not [string]::IsNullOrWhiteSpace($TimestampUrl)) {
            $sig = Set-AuthenticodeSignature -FilePath $resolvedBinary -Certificate $cert -HashAlgorithm SHA256 -TimestampServer $TimestampUrl
        } else {
            $sig = Set-AuthenticodeSignature -FilePath $resolvedBinary -Certificate $cert -HashAlgorithm SHA256
        }
        if ($sig.Status -ne "Valid") {
            Write-Warning "Signature status for '$resolvedBinary' is '$($sig.Status)': $($sig.StatusMessage)"
            exit 1
        }
    }

    $verify = Get-AuthenticodeSignature -FilePath $resolvedBinary
    Write-Host "[+] Verified '$resolvedBinary': $($verify.Status) | Signer: $($verify.SignerCertificate.Subject)" -ForegroundColor Green
    if ($verify.Status -ne "Valid") {
        Write-Warning "Signature verification status for '$resolvedBinary': $($verify.Status) ($($verify.StatusMessage))"
        exit 1
    }
}
