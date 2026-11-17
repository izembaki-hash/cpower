@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0.."

if "%~1"=="" (
    python clients\python\cpower_hybrid_agent.py "ادخل الى reddit وتنقل بين المقالات"
) else (
    python clients\python\cpower_hybrid_agent.py "%*"
)
pause
