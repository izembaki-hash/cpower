@echo off
setlocal enabledelayedexpansion
pushd "%~dp0"

set "TARGET=%~1"
if "%TARGET%"=="" set "TARGET=all"

if /I "%TARGET%"=="clean" (
    if exist "obj\*.o" del /Q "obj\*.o"
    if exist "obj\*.obj" del /Q "obj\*.obj"
    if exist "obj\*.res" del /Q "obj\*.res"
    if exist "bin\*.exe" del /Q "bin\*.exe"
    if exist "bin\*.dll" del /Q "bin\*.dll"
    if exist "tests\*.exe" del /Q "tests\*.exe"
    echo [+] Clean complete.
    popd
    exit /b 0
)

if not exist "obj" mkdir "obj"
if not exist "bin" mkdir "bin"

set "TOOLCHAIN=none"

if /I "%TARGET%"=="gcc" (
    set "TARGET=all"
    goto :try_gcc
)
if /I "%TARGET%"=="msvc" (
    set "TARGET=all"
    goto :try_msvc
)

:try_msvc
where cl.exe >nul 2>&1
if %errorlevel%==0 (
    set "TOOLCHAIN=msvc"
    goto :build_start
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if exist "%VSWHERE%" (
    for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%i\VC\Auxiliary\Build\vcvars64.bat" (
            call "%%i\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
            where cl.exe >nul 2>&1
            if !errorlevel!==0 (
                set "TOOLCHAIN=msvc"
                goto :build_start
            )
        )
    )
)
if /I "%~1"=="msvc" (
    echo [ERROR] MSVC cl.exe not found on this machine.
    popd
    exit /b 1
)

:try_gcc
where gcc.exe >nul 2>&1
if %errorlevel%==0 (
    set "TOOLCHAIN=gcc"
    goto :build_start
)
if exist "C:\ProgramData\mingw64\mingw64\bin\gcc.exe" (
    set "PATH=C:\ProgramData\mingw64\mingw64\bin;%PATH%"
    set "TOOLCHAIN=gcc"
    goto :build_start
)

echo [ERROR] Neither MSVC cl.exe nor MinGW-w64 gcc.exe was found.
echo         Install MinGW-w64 or add C:\ProgramData\mingw64\mingw64\bin to PATH.
popd
exit /b 1

:build_start
echo [*] Active Toolchain: %TOOLCHAIN%

set "CORE_SRCS=src\common\core.c src\common\logger.c src\common\timer.c src\perception\uia_tree.c src\perception\winevent_hook.c src\perception\fallback_text.c src\perception\dxgi_capture.c src\perception\tree_diff.c src\input\direct_control.c src\input\bezier_mouse.c src\input\human_keyboard.c src\input\window_ctrl.c src\ipc\shm_transport.c src\ipc\pipe_transport.c src\security\privilege.c src\hooks\hook_injector.c"

if "%TOOLCHAIN%"=="gcc" goto :build_gcc
goto :build_msvc

:build_gcc
set "CFLAGS=-m64 -Wall -Wextra -Werror -pedantic -std=c11 -O2 -DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DCOBJMACROS -Iinclude"
set "TEST_CFLAGS=%CFLAGS% -Wno-stringop-truncation"
set "LDFLAGS=-m64 -O2 -s -static-libgcc -Wl,--dynamicbase -Wl,--high-entropy-va -Wl,--nxcompat"
set "LIBS=-lole32 -loleaut32 -luiautomationcore -luser32 -lgdi32 -ld3d11 -ldxgi -ladvapi32 -lm"
set "CORE_OBJS="

for %%f in (%CORE_SRCS%) do (
    set "OBJ=obj\%%~nf.o"
    echo   [CC] %%f
    gcc %CFLAGS% -c "%%f" -o "!OBJ!"
    if errorlevel 1 goto :build_fail
    set "CORE_OBJS=!CORE_OBJS! !OBJ!"
)

echo   [CC] src\main.c
gcc %CFLAGS% -c "src\main.c" -o "obj\main.o"
if errorlevel 1 goto :build_fail

echo   [RC] res\app_dev.rc
windres -F pe-x86-64 -Ires -i res\app_dev.rc -o obj\app_dev.res.o
if errorlevel 1 goto :build_fail

echo   [RC] res\app.rc
windres -F pe-x86-64 -Ires -i res\app.rc -o obj\app.res.o
if errorlevel 1 goto :build_fail

echo   [LD] bin\cpower.exe
gcc -municode %LDFLAGS% obj\main.o %CORE_OBJS% obj\app_dev.res.o -o bin\cpower.exe %LIBS%
if errorlevel 1 goto :build_fail

echo   [LD] bin\cpower_uiaccess.exe
gcc -municode %LDFLAGS% obj\main.o %CORE_OBJS% obj\app.res.o -o bin\cpower_uiaccess.exe %LIBS%
if errorlevel 1 goto :build_fail

echo   [CC/LD] bin\cpower_hook64.dll
gcc %CFLAGS% -shared %LDFLAGS% "src\hooks\gdi_dwrite_hook.c" -o "bin\cpower_hook64.dll" -luser32 -lgdi32 -ladvapi32
if errorlevel 1 goto :build_fail

set "TEST_DEFS="
set "TEST_OBJS="
if exist "tests\test_perception.c" (
    set "TEST_DEFS=!TEST_DEFS! -DENABLE_PERCEPTION_TESTS"
    gcc %TEST_CFLAGS% -Itests -c "tests\test_perception.c" -o "obj\test_perception.o" || goto :build_fail
    set "TEST_OBJS=!TEST_OBJS! obj\test_perception.o"
)
if exist "tests\test_input.c" (
    set "TEST_DEFS=!TEST_DEFS! -DENABLE_INPUT_TESTS"
    gcc %TEST_CFLAGS% -Itests -c "tests\test_input.c" -o "obj\test_input.o" || goto :build_fail
    set "TEST_OBJS=!TEST_OBJS! obj\test_input.o"
)
if exist "tests\test_ipc.c" (
    set "TEST_DEFS=!TEST_DEFS! -DENABLE_IPC_TESTS"
    gcc %TEST_CFLAGS% -Itests -c "tests\test_ipc.c" -o "obj\test_ipc.o" || goto :build_fail
    set "TEST_OBJS=!TEST_OBJS! obj\test_ipc.o"
)
if exist "tests\test_security.c" (
    set "TEST_DEFS=!TEST_DEFS! -DENABLE_SECURITY_TESTS"
    gcc %TEST_CFLAGS% -Itests -c "tests\test_security.c" -o "obj\test_security.o" || goto :build_fail
    set "TEST_OBJS=!TEST_OBJS! obj\test_security.o"
)
if exist "tests\test_arch.c" (
    set "TEST_DEFS=!TEST_DEFS! -DENABLE_ARCH_TESTS"
    gcc %TEST_CFLAGS% -Itests -c "tests\test_arch.c" -o "obj\test_arch.o" || goto :build_fail
    set "TEST_OBJS=!TEST_OBJS! obj\test_arch.o"
)
if exist "tests\test_e2e.c" (
    set "TEST_DEFS=!TEST_DEFS! -DENABLE_E2E_TESTS"
    gcc %TEST_CFLAGS% -Itests -c "tests\test_e2e.c" -o "obj\test_e2e.o" || goto :build_fail
    set "TEST_OBJS=!TEST_OBJS! obj\test_e2e.o"
)

echo   [CC] tests\test_runner.c
gcc %TEST_CFLAGS% -Itests !TEST_DEFS! -c "tests\test_runner.c" -o "obj\test_runner.o"
if errorlevel 1 goto :build_fail

echo   [LD] tests\test_runner.exe
gcc %LDFLAGS% obj\test_runner.o !TEST_OBJS! %CORE_OBJS% -o tests\test_runner.exe %LIBS%
if errorlevel 1 goto :build_fail

goto :audit_dlls

:build_msvc
set "MCFLAGS=/nologo /std:c11 /W4 /WX /O2 /MT /GS /guard:cf /DUNICODE /D_UNICODE /DWIN32_LEAN_AND_MEAN /DCOBJMACROS /Iinclude"
set "MLINKFLAGS=/MACHINE:X64 /DYNAMICBASE /HIGHENTROPYVA /NXCOMPAT /GUARD:CF"
set "MLIBS=ole32.lib oleaut32.lib uiautomationcore.lib user32.lib gdi32.lib d3d11.lib dxgi.lib advapi32.lib"
set "CORE_OBJS="

for %%f in (%CORE_SRCS%) do (
    set "OBJ=obj\%%~nf.obj"
    echo   [CL] %%f
    cl.exe %MCFLAGS% /c "%%f" /Fo"!OBJ!"
    if errorlevel 1 goto :build_fail
    set "CORE_OBJS=!CORE_OBJS! !OBJ!"
)

echo   [CL] src\main.c
cl.exe %MCFLAGS% /c "src\main.c" /Fo"obj\main.obj"
if errorlevel 1 goto :build_fail

echo   [RC] res\app_dev.rc
rc.exe /nologo /i res /fo obj\app_dev.res res\app_dev.rc
if errorlevel 1 goto :build_fail

echo   [RC] res\app.rc
rc.exe /nologo /i res /fo obj\app.res res\app.rc
if errorlevel 1 goto :build_fail

echo   [LINK] bin\cpower.exe
cl.exe /nologo obj\main.obj %CORE_OBJS% obj\app_dev.res /Fe:bin\cpower.exe /link /SUBSYSTEM:CONSOLE %MLINKFLAGS% %MLIBS%
if errorlevel 1 goto :build_fail

echo   [LINK] bin\cpower_uiaccess.exe
cl.exe /nologo obj\main.obj %CORE_OBJS% obj\app.res /Fe:bin\cpower_uiaccess.exe /link /SUBSYSTEM:CONSOLE %MLINKFLAGS% %MLIBS%
if errorlevel 1 goto :build_fail

echo   [CL/LD] bin\cpower_hook64.dll
cl.exe %MCFLAGS% /LD "src\hooks\gdi_dwrite_hook.c" /Fo"obj\gdi_dwrite_hook.obj" /Fe:bin\cpower_hook64.dll /link %MLINKFLAGS% user32.lib gdi32.lib advapi32.lib
if errorlevel 1 goto :build_fail

set "TEST_DEFS="
set "TEST_OBJS="
if exist "tests\test_perception.c" (
    set "TEST_DEFS=!TEST_DEFS! /DENABLE_PERCEPTION_TESTS"
    cl.exe %MCFLAGS% /Itests /c "tests\test_perception.c" /Fo"obj\test_perception.obj" || goto :build_fail
    set "TEST_OBJS=!TEST_OBJS! obj\test_perception.obj"
)
if exist "tests\test_input.c" (
    set "TEST_DEFS=!TEST_DEFS! /DENABLE_INPUT_TESTS"
    cl.exe %MCFLAGS% /Itests /c "tests\test_input.c" /Fo"obj\test_input.obj" || goto :build_fail
    set "TEST_OBJS=!TEST_OBJS! obj\test_input.obj"
)
if exist "tests\test_ipc.c" (
    set "TEST_DEFS=!TEST_DEFS! /DENABLE_IPC_TESTS"
    cl.exe %MCFLAGS% /Itests /c "tests\test_ipc.c" /Fo"obj\test_ipc.obj" || goto :build_fail
    set "TEST_OBJS=!TEST_OBJS! obj\test_ipc.obj"
)
if exist "tests\test_security.c" (
    set "TEST_DEFS=!TEST_DEFS! /DENABLE_SECURITY_TESTS"
    cl.exe %MCFLAGS% /Itests /c "tests\test_security.c" /Fo"obj\test_security.obj" || goto :build_fail
    set "TEST_OBJS=!TEST_OBJS! obj\test_security.obj"
)
if exist "tests\test_arch.c" (
    set "TEST_DEFS=!TEST_DEFS! /DENABLE_ARCH_TESTS"
    cl.exe %MCFLAGS% /Itests /c "tests\test_arch.c" /Fo"obj\test_arch.obj" || goto :build_fail
    set "TEST_OBJS=!TEST_OBJS! obj\test_arch.obj"
)
if exist "tests\test_e2e.c" (
    set "TEST_DEFS=!TEST_DEFS! /DENABLE_E2E_TESTS"
    cl.exe %MCFLAGS% /Itests /c "tests\test_e2e.c" /Fo"obj\test_e2e.obj" || goto :build_fail
    set "TEST_OBJS=!TEST_OBJS! obj\test_e2e.obj"
)

echo   [CL] tests\test_runner.c
cl.exe %MCFLAGS% /Itests !TEST_DEFS! /c "tests\test_runner.c" /Fo"obj\test_runner.obj"
if errorlevel 1 goto :build_fail

echo   [LINK] tests\test_runner.exe
cl.exe /nologo obj\test_runner.obj !TEST_OBJS! %CORE_OBJS% /Fe:tests\test_runner.exe /link /SUBSYSTEM:CONSOLE %MLINKFLAGS% %MLIBS%
if errorlevel 1 goto :build_fail

:audit_dlls
where objdump.exe >nul 2>&1
if %errorlevel%==0 (
    echo [*] Auditing DLL imports via objdump -p...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $re='^(KERNEL32|USER32|GDI32|ADVAPI32|ole32|OLEAUT32|UIAutomationCore|dxgi|d3d11|api-ms-win-crt-.*|ucrtbase|msvcrt)\.dll$'; foreach ($b in @('bin\cpower.exe','bin\cpower_uiaccess.exe','bin\cpower_hook64.dll','tests\test_runner.exe')) { Write-Host ('  --- ' + $b + ' ---'); $dlls = (& objdump.exe -p $b | Select-String 'DLL Name:\s+(\S+)' | ForEach-Object { $_.Matches[0].Groups[1].Value }); if (-not $dlls) { Write-Error ('No DLL imports parsed from ' + $b); exit 1 }; foreach ($d in $dlls) { Write-Host ('    ' + $d); if ($d -notmatch $re) { Write-Error ('Prohibited runtime DLL dependency detected in ' + $b + ': ' + $d); exit 1 } } }"
    if errorlevel 1 goto :build_fail
)

if /I "%TARGET%"=="verify" (
    echo [*] Running bin\cpower.exe verify...
    bin\cpower.exe verify
    if errorlevel 1 goto :build_fail
)
if /I "%TARGET%"=="test" (
    echo [*] Running tests\test_runner.exe --all...
    tests\test_runner.exe --all
    if errorlevel 1 goto :build_fail
)

echo [+] Build completed successfully.
popd
exit /b 0

:build_fail
echo [ERROR] Build failed!
popd
exit /b 1
