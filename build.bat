@echo off
rem Builds cordinal. The real work is in tools\build.ps1 (read that file for the incremental model,
rem the compile groups and every flag); this launcher exists because cmd is the supported entry point.
rem
rem   build.bat              local experimental packed build (with the address filter)
rem   build.bat stock-nrv    stock UPX release build
rem   build.bat unpacked     development build, plain executables, no UPX download
rem   build.bat full         ignore the incremental record and rebuild everything
rem   build.bat experimental  local development build with the sibling UPX research decoder
rem   build.bat experimental-addresses  also use its smaller x64 address-filter format
rem   build.bat cdpack       independent decoder, PE packer and loader (local development)
rem   build.bat themes       also include the themes beyond Light and Dark
setlocal
cd /d "%~dp0"

set "ARGS="
:args
if "%~1"=="" goto run
if /i "%~1"=="unpacked" (set "ARGS=%ARGS% -Unpacked") else if /i "%~1"=="full" (set "ARGS=%ARGS% -Full") else if /i "%~1"=="experimental" (set "ARGS=%ARGS% -Experimental") else if /i "%~1"=="experimental-addresses" (set "ARGS=%ARGS% -ExperimentalAddresses") else if /i "%~1"=="cdpack" (set "ARGS=%ARGS% -Cdpack") else if /i "%~1"=="stock-nrv" (set "ARGS=%ARGS% -StockNrv") else if /i "%~1"=="themes" (set "ARGS=%ARGS% -Themes") else (
    echo Usage: build.bat [unpacked ^| experimental ^| experimental-addresses ^| cdpack ^| stock-nrv] [full] [themes]
    exit /b 2
)
shift
goto args

:run
set "PS=powershell.exe"
where pwsh.exe >nul 2>nul && set "PS=pwsh.exe"
"%PS%" -NoLogo -NoProfile -ExecutionPolicy Bypass -File "tools\build.ps1" %ARGS%
exit /b %ERRORLEVEL%
