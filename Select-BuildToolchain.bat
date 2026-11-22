@echo off
:: Select-BuildToolchain.bat - choose and save the Visual Studio/MSVC toolset.
::
:: The choice is written to Saved\BuildToolchain.cfg and used by Compile-Plugin.bat.
:: The engine's MSVC line is detected from its compiled binaries, so the menu can
:: mark the matching toolset.
::
::   Select-BuildToolchain.bat              show the menu
::   Select-BuildToolchain.bat -ListToolchains
::   Select-BuildToolchain.bat -NoPause

setlocal
set "POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%POWERSHELL%" set "POWERSHELL=powershell.exe"

"%POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build.ps1" -SelectToolchain %*
exit /b %ERRORLEVEL%
