<#
.SYNOPSIS
    Multi-toolchain build, DLL import audit, and verification script for cpower.
.DESCRIPTION
    Auto-detects MinGW-w64 GCC (primary) or MSVC cl.exe, builds bin\cpower.exe
    (dev manifest), bin\cpower_uiaccess.exe (uiAccess="true" manifest), and
    tests\test_runner.exe (auto-enabling present test suites), and audits PE
    imports via objdump -p to confirm zero third-party DLL dependencies.
.PARAMETER Toolchain
    Auto (default: detects MSVC or GCC), Gcc, or Msvc.
.PARAMETER Clean
    Removes obj/, bin/, and tests/*.exe artifacts before exiting (or before building if -BuildAlso is not needed; cleans and exits if sole action).
.PARAMETER Test
    Runs tests\test_runner.exe --all after building.
.PARAMETER Verify
    Runs bin\cpower.exe verify after building.
.PARAMETER Sign
    Invokes scripts\sign_app.ps1 on bin\cpower_uiaccess.exe and bin\cpower_hook64.dll after building.
#>
[CmdletBinding()]
param(
    [ValidateSet("Auto", "Gcc", "Msvc")]
    [string]$Toolchain = "Auto",
    [switch]$Clean,
    [switch]$Test,
    [switch]$Verify,
    [switch]$Sign
)

$ErrorActionPreference = "Stop"
Push-Location $PSScriptRoot

try {
    if ($Clean) {
        Write-Host "[*] Cleaning build artifacts..." -ForegroundColor Yellow
        Remove-Item -Path "obj\*.o", "obj\*.obj", "obj\*.res", "bin\*.exe", "bin\*.dll", "tests\*.exe" -Force -ErrorAction SilentlyContinue
        if (-not ($Test -or $Verify -or $Sign)) {
            Write-Host "[+] Clean complete." -ForegroundColor Green
            exit 0
        }
    }

    New-Item -ItemType Directory -Path "obj" -Force | Out-Null
    New-Item -ItemType Directory -Path "bin" -Force | Out-Null

    function Find-Msvc {
        $cl = Get-Command cl.exe -ErrorAction SilentlyContinue
        if ($cl) { return $true }
        $vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
        if (Test-Path $vswhere) {
            $inst = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
            if ($inst) {
                $vcvars = Join-Path $inst "VC\Auxiliary\Build\vcvars64.bat"
                if (Test-Path $vcvars) {
                    $envDump = cmd.exe /c "`"$vcvars`" >nul 2>&1 && set"
                    foreach ($line in $envDump) {
                        if ($line -match "^([^=]+)=(.*)$") {
                            [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
                        }
                    }
                    return [bool](Get-Command cl.exe -ErrorAction SilentlyContinue)
                }
            }
        }
        return $false
    }

    function Find-Gcc {
        $gcc = Get-Command gcc.exe -ErrorAction SilentlyContinue
        if ($gcc) { return $true }
        $fallback = "C:\ProgramData\mingw64\mingw64\bin"
        if (Test-Path (Join-Path $fallback "gcc.exe")) {
            $env:PATH = "$fallback;$env:PATH"
            return $true
        }
        return $false
    }

    $activeToolchain = $null
    if ($Toolchain -eq "Msvc") {
        if (-not (Find-Msvc)) { throw "MSVC cl.exe was requested but not found." }
        $activeToolchain = "Msvc"
    } elseif ($Toolchain -eq "Gcc") {
        if (-not (Find-Gcc)) { throw "MinGW-w64 gcc.exe was requested but not found." }
        $activeToolchain = "Gcc"
    } else {
        if (Find-Gcc) {
            $activeToolchain = "Gcc"
        } elseif (Find-Msvc) {
            $activeToolchain = "Msvc"
        } else {
            throw "Neither MinGW-w64 gcc.exe nor MSVC cl.exe was found."
        }
    }

    Write-Host "[*] Using Toolchain: $activeToolchain" -ForegroundColor Cyan

    $coreSources = @(
        "src\common\core.c",
        "src\common\logger.c",
        "src\common\timer.c",
        "src\perception\uia_tree.c",
        "src\perception\winevent_hook.c",
        "src\perception\fallback_text.c",
        "src\perception\dxgi_capture.c",
        "src\perception\tree_diff.c",
        "src\input\direct_control.c",
        "src\input\bezier_mouse.c",
        "src\input\human_keyboard.c",
        "src\input\window_ctrl.c",
        "src\ipc\shm_transport.c",
        "src\ipc\pipe_transport.c",
        "src\security\privilege.c",
        "src\hooks\hook_injector.c"
    )

    $optionalSuites = @(
        @{ File = "tests\test_perception.c"; Def = "ENABLE_PERCEPTION_TESTS" },
        @{ File = "tests\test_input.c";      Def = "ENABLE_INPUT_TESTS" },
        @{ File = "tests\test_ipc.c";        Def = "ENABLE_IPC_TESTS" },
        @{ File = "tests\test_security.c";   Def = "ENABLE_SECURITY_TESTS" },
        @{ File = "tests\test_arch.c";       Def = "ENABLE_ARCH_TESTS" },
        @{ File = "tests\test_e2e.c";        Def = "ENABLE_E2E_TESTS" }
    )

    if ($activeToolchain -eq "Gcc") {
        $cflags = @("-m64", "-Wall", "-Wextra", "-Werror", "-pedantic", "-std=c11", "-O2",
                    "-DUNICODE", "-D_UNICODE", "-DWIN32_LEAN_AND_MEAN", "-DCOBJMACROS", "-Iinclude")
        $testCflags = $cflags + @("-Wno-stringop-truncation")
        $ldflags = @("-m64", "-O2", "-s", "-static-libgcc", "-Wl,--dynamicbase", "-Wl,--high-entropy-va", "-Wl,--nxcompat")
        $libs = @("-lole32", "-loleaut32", "-luiautomationcore", "-luser32", "-lgdi32",
                  "-ld3d11", "-ldxgi", "-ladvapi32", "-lm")

        $coreObjs = @()
        foreach ($src in $coreSources) {
            $obj = "obj\" + [System.IO.Path]::GetFileNameWithoutExtension($src) + ".o"
            Write-Host "  [CC] $src"
            & gcc @cflags -c $src -o $obj
            if ($LASTEXITCODE -ne 0) { throw "Compilation failed for $src" }
            $coreObjs += $obj
        }

        Write-Host "  [CC] src\main.c"
        & gcc @cflags -c "src\main.c" -o "obj\main.o"
        if ($LASTEXITCODE -ne 0) { throw "Compilation failed for src\main.c" }

        Write-Host "  [RC] res\app_dev.rc"
        & windres -F pe-x86-64 -Ires -i "res\app_dev.rc" -o "obj\app_dev.res.o"
        if ($LASTEXITCODE -ne 0) { throw "windres failed for res\app_dev.rc" }

        Write-Host "  [RC] res\app.rc"
        & windres -F pe-x86-64 -Ires -i "res\app.rc" -o "obj\app.res.o"
        if ($LASTEXITCODE -ne 0) { throw "windres failed for res\app.rc" }

        Write-Host "  [LD] bin\cpower.exe"
        & gcc -municode @ldflags "obj\main.o" @coreObjs "obj\app_dev.res.o" -o "bin\cpower.exe" @libs
        if ($LASTEXITCODE -ne 0) { throw "Link failed for bin\cpower.exe" }

        Write-Host "  [LD] bin\cpower_uiaccess.exe"
        & gcc -municode @ldflags "obj\main.o" @coreObjs "obj\app.res.o" -o "bin\cpower_uiaccess.exe" @libs
        if ($LASTEXITCODE -ne 0) { throw "Link failed for bin\cpower_uiaccess.exe" }

        Write-Host "  [CC/LD] bin\cpower_hook64.dll"
        & gcc @cflags -shared @ldflags "src\hooks\gdi_dwrite_hook.c" -o "bin\cpower_hook64.dll" -luser32 -lgdi32 -ladvapi32
        if ($LASTEXITCODE -ne 0) { throw "Build failed for bin\cpower_hook64.dll" }

        $testDefs = @()
        $testObjs = @()
        foreach ($suite in $optionalSuites) {
            if (Test-Path $suite.File) {
                $testDefs += "-D$($suite.Def)"
                $tobj = "obj\" + [System.IO.Path]::GetFileNameWithoutExtension($suite.File) + ".o"
                Write-Host "  [CC] $($suite.File)"
                & gcc @testCflags -Itests -c $suite.File -o $tobj
                if ($LASTEXITCODE -ne 0) { throw "Compilation failed for $($suite.File)" }
                $testObjs += $tobj
            }
        }

        Write-Host "  [CC] tests\test_runner.c"
        & gcc @testCflags -Itests @testDefs -c "tests\test_runner.c" -o "obj\test_runner.o"
        if ($LASTEXITCODE -ne 0) { throw "Compilation failed for tests\test_runner.c" }

        Write-Host "  [LD] tests\test_runner.exe"
        & gcc @ldflags "obj\test_runner.o" @testObjs @coreObjs -o "tests\test_runner.exe" @libs
        if ($LASTEXITCODE -ne 0) { throw "Link failed for tests\test_runner.exe" }
    } else {
        function Find-WindowsSdkRc {
            $rcCmd = Get-Command rc.exe -ErrorAction SilentlyContinue
            if ($rcCmd) { return $rcCmd.Source }

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
                    $found = Get-ChildItem -Path (Join-Path $binDir "*\x64\rc.exe") -ErrorAction SilentlyContinue
                    if ($found) { $candidates += $found }
                }
            }
            if ($candidates.Count -gt 0) {
                return ($candidates | Sort-Object FullName -Descending | Select-Object -First 1).FullName
            }
            throw "Windows SDK rc.exe (x64) was not found in PATH or Windows Kits 10."
        }

        $cflags = @("/nologo", "/std:c11", "/W4", "/WX", "/O2", "/MT", "/GS", "/guard:cf",
                    "/DUNICODE", "/D_UNICODE", "/DWIN32_LEAN_AND_MEAN", "/DCOBJMACROS", "/Iinclude")
        $linkFlags = @("/MACHINE:X64", "/DYNAMICBASE", "/HIGHENTROPYVA", "/NXCOMPAT", "/GUARD:CF")
        $libs = @("ole32.lib", "oleaut32.lib", "uiautomationcore.lib", "user32.lib", "gdi32.lib",
                  "d3d11.lib", "dxgi.lib", "advapi32.lib")

        $rcExe = Find-WindowsSdkRc

        $coreObjs = @()
        foreach ($src in $coreSources) {
            $obj = "obj\" + [System.IO.Path]::GetFileNameWithoutExtension($src) + ".obj"
            Write-Host "  [CL] $src"
            & cl.exe @cflags /c $src "/Fo$obj"
            if ($LASTEXITCODE -ne 0) { throw "Compilation failed for $src" }
            $coreObjs += $obj
        }

        Write-Host "  [CL] src\main.c"
        & cl.exe @cflags /c "src\main.c" "/Foobj\main.obj"
        if ($LASTEXITCODE -ne 0) { throw "Compilation failed for src\main.c" }

        & $rcExe /nologo /i res /fo "obj\app_dev.res" "res\app_dev.rc"
        if ($LASTEXITCODE -ne 0) { throw "rc.exe failed for res\app_dev.rc" }

        & $rcExe /nologo /i res /fo "obj\app.res" "res\app.rc"
        if ($LASTEXITCODE -ne 0) { throw "rc.exe failed for res\app.rc" }

        Write-Host "  [LINK] bin\cpower.exe"
        & cl.exe /nologo "obj\main.obj" @coreObjs "obj\app_dev.res" "/Febin\cpower.exe" /link /SUBSYSTEM:CONSOLE @linkFlags @libs
        if ($LASTEXITCODE -ne 0) { throw "Link failed for bin\cpower.exe" }

        Write-Host "  [LINK] bin\cpower_uiaccess.exe"
        & cl.exe /nologo "obj\main.obj" @coreObjs "obj\app.res" "/Febin\cpower_uiaccess.exe" /link /SUBSYSTEM:CONSOLE @linkFlags @libs
        if ($LASTEXITCODE -ne 0) { throw "Link failed for bin\cpower_uiaccess.exe" }

        Write-Host "  [CL/LD] bin\cpower_hook64.dll"
        & cl.exe @cflags /LD "src\hooks\gdi_dwrite_hook.c" "/Foobj\gdi_dwrite_hook.obj" "/Febin\cpower_hook64.dll" /link @linkFlags user32.lib gdi32.lib advapi32.lib
        if ($LASTEXITCODE -ne 0) { throw "Build failed for bin\cpower_hook64.dll" }

        $testDefs = @()
        $testObjs = @()
        foreach ($suite in $optionalSuites) {
            if (Test-Path $suite.File) {
                $testDefs += "/D$($suite.Def)"
                $tobj = "obj\" + [System.IO.Path]::GetFileNameWithoutExtension($suite.File) + ".obj"
                & cl.exe @cflags /Itests /c $suite.File "/Fo$tobj"
                if ($LASTEXITCODE -ne 0) { throw "Compilation failed for $($suite.File)" }
                $testObjs += $tobj
            }
        }

        & cl.exe @cflags /Itests @testDefs /c "tests\test_runner.c" "/Foobj\test_runner.obj"
        if ($LASTEXITCODE -ne 0) { throw "Compilation failed for tests\test_runner.c" }

        & cl.exe /nologo "obj\test_runner.obj" @testObjs @coreObjs "/Fetests\test_runner.exe" /link /SUBSYSTEM:CONSOLE @linkFlags @libs
        if ($LASTEXITCODE -ne 0) { throw "Link failed for tests\test_runner.exe" }
    }

    # Audit PE DLL imports via objdump -p
    $objdump = Get-Command objdump.exe -ErrorAction SilentlyContinue
    if ($objdump) {
        $allowedPattern = '^(KERNEL32|USER32|GDI32|ADVAPI32|ole32|OLEAUT32|UIAutomationCore|dxgi|d3d11|api-ms-win-crt-.*|ucrtbase|msvcrt)\.dll$'
        foreach ($exe in @("bin\cpower.exe", "bin\cpower_uiaccess.exe", "bin\cpower_hook64.dll", "tests\test_runner.exe")) {
            Write-Host "[*] Auditing DLL imports for $exe..." -ForegroundColor Cyan
            $dllLines = & $objdump.Source -p $exe | Select-String "DLL Name:\s+(\S+)"
            if ($LASTEXITCODE -ne 0) { throw "objdump failed for $exe" }
            $dlls = @()
            foreach ($m in $dllLines) {
                $dll = $m.Matches[0].Groups[1].Value
                $dlls += $dll
                if ($dll -notmatch $allowedPattern) {
                    throw "Non-system/third-party DLL dependency detected in ${exe}: $dll"
                }
            }
            if ($dlls.Count -eq 0) { throw "No DLL imports parsed from $exe" }
            Write-Host ("  [+] Verified native OS imports only ({0} DLLs): {1}" -f $dlls.Count, ($dlls -join ", ")) -ForegroundColor Green
        }
    }

    if ($Sign) {
        $signTargets = @(
            (Join-Path $PSScriptRoot "bin\cpower.exe"),
            (Join-Path $PSScriptRoot "bin\cpower_uiaccess.exe"),
            (Join-Path $PSScriptRoot "bin\cpower_hook64.dll")
        )
        & (Join-Path $PSScriptRoot "scripts\sign_app.ps1") -BinaryPath $signTargets
        if ($LASTEXITCODE -ne 0) { throw "Signing failed." }
    }

    if ($Verify) {
        Write-Host "[*] Running bin\cpower.exe verify..." -ForegroundColor Cyan
        & ".\bin\cpower.exe" verify
        if ($LASTEXITCODE -ne 0) { throw "cpower verify failed." }
    }

    if ($Test) {
        Write-Host "[*] Running tests\test_runner.exe --all..." -ForegroundColor Cyan
        & ".\tests\test_runner.exe" --all
        if ($LASTEXITCODE -ne 0) { throw "Test suite failed." }
    }

    Write-Host "[+] Build pipeline completed successfully!" -ForegroundColor Green
} finally {
    Pop-Location
}
