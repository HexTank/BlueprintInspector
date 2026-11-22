@echo off
:: Compile-Plugin.bat - thin wrapper for Build.ps1 -Build Plugin.
::
:: Builds the BlueprintInspector plugin module.  If the third-party libraries are
:: missing, or were built with a different MSVC toolset, they are rebuilt first.
:: The real implementation lives in Build.ps1.
::
::   Compile-Plugin.bat [Configuration] [Platform] [UnrealBuildTool options...]
::
:: Examples:
::   Compile-Plugin.bat
::   Compile-Plugin.bat -NoPause
::   Compile-Plugin.bat -Rebuild
::   Compile-Plugin.bat Development
::   Compile-Plugin.bat -SingleFile=Source\BlueprintInspector\Private\BlueprintScriptDisassembler.cpp
::
:: Configuration defaults to DebugGame because that is how the engine is currently
:: built; the matching project binaries are the ones the editor loads.
::
:: The engine root is read from UE_ROOT if set, otherwise from EngineAssociation
:: in the .uproject (a source-build GUID resolves through HKCU, an installed build
:: through HKLM).
::
:: Options handled by Build.ps1:
::   -NoPause              do not wait for a key press at the end
::   -DryRun               print the commands without running them
::   -Toolchain=auto|saved|<version>|<installId>/<version>
::   -NoAutoModules        fail instead of building modules first
::   -SingleFile=PATH      compile only the translation unit that owns PATH
::   -Configuration=NAME   same as the first positional argument
::   -Platform=NAME        same as the second positional argument
::   anything else         forwarded to UnrealBuildTool (-Rebuild and -Clean are
::                         the useful ones)
::
:: The exact MSVC toolset is pinned into UnrealBuildTool by temporarily adding
:: <WindowsPlatform><CompilerVersion> to the per-user BuildConfiguration.xml,
:: because UBT 4.25 has no -CompilerVersion switch.  The file is restored after
:: the build.  Run Select-BuildToolchain.bat to change the toolset.  See
:: Build.ps1 -Help.

setlocal
set "POWERSHELL=%SystemRoot%\System32\WindowsPowerShell\v1.0\powershell.exe"
if not exist "%POWERSHELL%" set "POWERSHELL=powershell.exe"

"%POWERSHELL%" -NoProfile -ExecutionPolicy Bypass -File "%~dp0Build.ps1" -Build Plugin %*
exit /b %ERRORLEVEL%
