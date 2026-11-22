@echo off
:: Build.bat - interactive build menu.
::
:: With no arguments this shows a menu for building the plugin and for changing
:: the Visual Studio/MSVC toolset.
::
:: Automation should use the explicit wrappers or pass -Build directly:
::   Build.bat -Build Plugin
::   Build.bat -Build Package
::   Build.bat -ListToolchains
::   Build.bat -Help

setlocal
set "POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%POWERSHELL%" set "POWERSHELL=powershell.exe"

"%POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build.ps1" %*
exit /b %ERRORLEVEL%
