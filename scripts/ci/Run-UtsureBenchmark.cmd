@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Run-UtsureBenchmark.ps1" %*
exit /b %ERRORLEVEL%
