<#
.SYNOPSIS
    Creates and installs a local self-signed code-signing root certificate for uiAccess execution.
.DESCRIPTION
    Generates an X.509 CodeSigning certificate in Cert:\LocalMachine\My (or reuses an
    existing certificate matching -CertName) and installs it into both
    Cert:\LocalMachine\Root (Trusted Root Certification Authorities) and
    Cert:\LocalMachine\TrustedPublisher.
.PARAMETER CertName
    Common Name (CN) for the local signing certificate. Default: "CPowerLocalRoot".
.PARAMETER ValidityYears
    Validity period in years for newly generated certificates. Default: 5.
#>
#Requires -RunAsAdministrator
[CmdletBinding()]
param(
    [string]$CertName = "CPowerLocalRoot",
    [int]$ValidityYears = 5
)

$ErrorActionPreference = "Stop"

Write-Host "[*] Checking Cert:\LocalMachine\My for '$CertName' (Code Signing EKU 1.3.6.1.5.5.7.3.3)..." -ForegroundColor Cyan
$codeSigningOid = "1.3.6.1.5.5.7.3.3"
$existing = Get-ChildItem -Path "Cert:\LocalMachine\My" |
    Where-Object {
        $_.Subject -eq "CN=$CertName" -and
        $_.HasPrivateKey -and
        $_.NotAfter -gt (Get-Date) -and
        ($_.EnhancedKeyUsageList | Where-Object { $_.ObjectId -eq $codeSigningOid })
    } |
    Sort-Object NotAfter -Descending |
    Select-Object -First 1

if ($existing) {
    $cert = $existing
    Write-Host "[+] Found existing valid certificate: $($cert.Thumbprint)" -ForegroundColor Green
} else {
    Write-Host "[*] Creating self-signed code-signing certificate 'CN=$CertName'..." -ForegroundColor Yellow
    $cert = New-SelfSignedCertificate `
        -Type CodeSigningCert `
        -Subject "CN=$CertName" `
        -CertStoreLocation "Cert:\LocalMachine\My" `
        -FriendlyName "CPower Native Automation Code-Signing" `
        -HashAlgorithm SHA256 `
        -KeyExportPolicy NonExportable `
        -NotAfter (Get-Date).AddYears($ValidityYears)
    Write-Host "[+] Created certificate with thumbprint: $($cert.Thumbprint)" -ForegroundColor Green
}

function Install-CertToMachineStore {
    param(
        [Parameter(Mandatory = $true)]
        [System.Security.Cryptography.X509Certificates.StoreName]$StoreName,
        [Parameter(Mandatory = $true)]
        [System.Security.Cryptography.X509Certificates.X509Certificate2]$Certificate
    )

    $store = New-Object System.Security.Cryptography.X509Certificates.X509Store(
        $StoreName,
        [System.Security.Cryptography.X509Certificates.StoreLocation]::LocalMachine
    )
    try {
        $store.Open([System.Security.Cryptography.X509Certificates.OpenFlags]::ReadWrite)
        $found = $store.Certificates.Find(
            [System.Security.Cryptography.X509Certificates.X509FindType]::FindByThumbprint,
            $Certificate.Thumbprint,
            $false
        )
        if ($found.Count -eq 0) {
            $store.Add($Certificate)
            Write-Host "[+] Installed certificate into LocalMachine\$StoreName." -ForegroundColor Green
        } else {
            Write-Host "[=] Certificate already present in LocalMachine\$StoreName." -ForegroundColor Gray
        }
    } finally {
        $store.Close()
    }
}

# Export public-only certificate (no private key reference) for Root and TrustedPublisher stores
$publicCert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2 (,$cert.RawData)
Install-CertToMachineStore -StoreName Root -Certificate $publicCert
Install-CertToMachineStore -StoreName TrustedPublisher -Certificate $publicCert

Write-Host "[+] Certificate setup complete. Thumbprint: $($cert.Thumbprint)" -ForegroundColor Green
